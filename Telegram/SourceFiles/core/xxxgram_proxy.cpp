#include "core/xxxgram_proxy.h"

#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>

namespace Core {
namespace {

constexpr auto kEnabled = "xxxgram.ws.enabled";
constexpr auto kFallback = "xxxgram.ws.thirdPartyFallback";
constexpr auto kPreviousProxy = "xxxgram.ws.previousProxy";
constexpr auto kRuntimeSecret = "xxxgram.ws.runtimeSecret";
constexpr auto kStartupTimeout = 15000;

} // namespace

XxxgramWsProxy::XxxgramWsProxy(not_null<Application*> application)
: QObject(application)
, _application(application) {
	auto &settings = _application->settings();
	_previousProxy = settings.readPref<QByteArray>(kPreviousProxy);
	const auto previousSecret = settings.readPref<QByteArray>(kRuntimeSecret);
	const auto selected = settings.proxy().selected();
	if (!previousSecret.isEmpty()
		&& selected.type == MTP::ProxyData::Type::Mtproto
		&& selected.host == u"127.0.0.1"_q
		&& selected.password.toLatin1() == previousSecret) {
		_ownedProxy = selected;
	}
	restoreProxy();
	_enabled = settings.readPref<bool>(kEnabled, true);
	_fallbackEnabled = settings.readPref<bool>(kFallback, false);
	_startupTimer.setSingleShot(true);
	QObject::connect(&_startupTimer, &QTimer::timeout, this, [=] {
		fail();
	});
	_application->proxyChanges() | rpl::on_next([=] {
		const auto &proxy = _application->settings().proxy();
		if (_enabled && !_applyingProxy
			&& (!proxy.isEnabled() || proxy.selected() != _ownedProxy)) {
			setEnabled(false);
		}
	}, _lifetime);
	if (_enabled) {
		start();
	} else {
		_status = u"WS-прокси выключен"_q;
	}
}

XxxgramWsProxy::~XxxgramWsProxy() {
	_lifetime.destroy();
	stopProcess();
	restoreProxy();
}

bool XxxgramWsProxy::enabled() const {
	return _enabled;
}

void XxxgramWsProxy::setEnabled(bool enabled) {
	if (_enabled == enabled) {
		if (enabled && !_ready) {
			start();
		}
		return;
	}
	_enabled = enabled;
	_application->settings().writePref<bool>(kEnabled, enabled);
	if (enabled) {
		start();
	} else {
		stopProcess();
		restoreProxy();
		_status = u"WS-прокси выключен"_q;
	}
}

bool XxxgramWsProxy::fallbackEnabled() const {
	return _fallbackEnabled;
}

void XxxgramWsProxy::setFallbackEnabled(bool enabled) {
	if (_fallbackEnabled == enabled) {
		return;
	}
	_fallbackEnabled = enabled;
	_application->settings().writePref<bool>(kFallback, enabled);
	if (_enabled) {
		start();
	}
}

rpl::producer<QString> XxxgramWsProxy::statusValue() const {
	return _status.value();
}

void XxxgramWsProxy::start() {
	stopProcess();
	auto &settings = _application->settings();
	if (_previousProxy.isEmpty()) {
		_previousProxy = settings.proxy().serialize();
		settings.writePref<QByteArray>(kPreviousProxy, _previousProxy);
	}
	auto secret = QByteArray(16, '\0');
	base::RandomFill(secret.data(), secret.size());
	_ownedProxy = MTP::ProxyData();
	_ownedProxy.type = MTP::ProxyData::Type::Mtproto;
	_ownedProxy.host = u"127.0.0.1"_q;
	_ownedProxy.port = 1;
	_ownedProxy.password = u"dd"_q + QString::fromLatin1(secret.toHex());
	settings.writePref<QByteArray>(
		kRuntimeSecret,
		_ownedProxy.password.toLatin1());
	settings.proxy().setProxyRotationEnabled(false);
	applyProxy();
	_status = u"Запуск WS-прокси…"_q;

	const auto directory = QCoreApplication::applicationDirPath() + u"/proxy/"_q;
	const auto python = directory + u"venv/bin/python3"_q;
	const auto bridge = directory + u"bridge.py"_q;
	if (!QFileInfo(python).isExecutable() || !QFileInfo(bridge).isFile()) {
		_status = u"WS-прокси не установлен · выключите его для обычного подключения"_q;
		return;
	}
	_process = std::make_unique<QProcess>();
	_process->setStandardErrorFile(QProcess::nullDevice());
	QObject::connect(_process.get(), &QProcess::started, this, [=] {
		const auto configuration = QJsonDocument(QJsonObject{
			{ u"secret"_q, QString::fromLatin1(secret.toHex()) },
			{ u"fallback"_q, _fallbackEnabled },
		}).toJson(QJsonDocument::Compact) + '\n';
		if (_process->write(configuration) != configuration.size()) {
			fail();
		}
	});
	QObject::connect(_process.get(), &QProcess::readyReadStandardOutput, this, [=] {
		readOutput();
	});
	QObject::connect(_process.get(), &QProcess::errorOccurred, this, [=] {
		fail();
	});
	QObject::connect(
		_process.get(),
		qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
		this,
		[=] { fail(); });
	_startupTimer.start(kStartupTimeout);
	_process->start(python, { u"-E"_q, u"-s"_q, u"-B"_q, u"-u"_q, bridge });
}

void XxxgramWsProxy::stopProcess() {
	_startupTimer.stop();
	_ready = false;
	_output.clear();
	if (_process) {
		QObject::disconnect(_process.get(), nullptr, this, nullptr);
		_process->closeWriteChannel();
		if (_process->state() != QProcess::NotRunning
			&& !_process->waitForFinished(600)) {
			_process->kill();
			_process->waitForFinished(200);
		}
		_process = nullptr;
	}
}

void XxxgramWsProxy::readOutput() {
	_output += _process->readAllStandardOutput();
	if (_output.size() > 2048) {
		fail();
		return;
	}
	const auto newline = _output.indexOf('\n');
	if (newline < 0) {
		return;
	}
	const auto document = QJsonDocument::fromJson(_output.left(newline));
	_output.remove(0, newline + 1);
	const auto object = document.object();
	const auto port = object.value(u"port"_q).toInt();
	if (_ready || !document.isObject()
		|| object.value(u"state"_q).toString() != u"ready"_q
		|| port < 1024 || port > 65535) {
		fail();
		return;
	}
	_startupTimer.stop();
	_ownedProxy.port = port;
	_ready = true;
	applyProxy();
	_status = _fallbackEnabled
		? u"WS-прокси готов · сторонний резерв разрешён"_q
		: u"WS-прокси готов · только серверы Telegram"_q;
}

void XxxgramWsProxy::fail() {
	_startupTimer.stop();
	_ready = false;
	_status = u"Ошибка WS-прокси · выключите и включите его для повтора"_q;
	if (_process && _process->state() != QProcess::NotRunning) {
		_process->kill();
	}
}

void XxxgramWsProxy::applyProxy() {
	_applyingProxy = true;
	_application->setCurrentProxy(_ownedProxy, MTP::ProxyData::Settings::Enabled);
	_applyingProxy = false;
}

void XxxgramWsProxy::restoreProxy() {
	auto previous = SettingsProxy();
	auto &settings = _application->settings();
	if (!_previousProxy.isEmpty() && previous.setFromSerialized(_previousProxy)) {
		if (settings.proxy().selected() == _ownedProxy) {
			_applyingProxy = true;
			settings.proxy().setProxyRotationEnabled(previous.proxyRotationEnabled());
			_application->setCurrentProxy(previous.selected(), previous.settings());
			_applyingProxy = false;
		}
	}
	_previousProxy.clear();
	settings.clearPref(kPreviousProxy);
	settings.clearPref(kRuntimeSecret);
}

} // namespace Core
