#pragma once

#include "mtproto/mtproto_proxy_data.h"

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <rpl/variable.h>

class QProcess;

namespace Core {

class Application;

class XxxgramWsProxy final : public QObject {
public:
	explicit XxxgramWsProxy(not_null<Application*> application);
	~XxxgramWsProxy() override;

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);
	[[nodiscard]] bool fallbackEnabled() const;
	void setFallbackEnabled(bool enabled);
	[[nodiscard]] rpl::producer<QString> statusValue() const;

private:
	void start();
	void stopProcess();
	void readOutput();
	void fail();
	void applyProxy();
	void restoreProxy();

	const not_null<Application*> _application;
	std::unique_ptr<QProcess> _process;
	QTimer _startupTimer;
	QByteArray _output;
	QByteArray _previousProxy;
	MTP::ProxyData _ownedProxy;
	bool _enabled = false;
	bool _fallbackEnabled = false;
	bool _applyingProxy = false;
	bool _ready = false;
	rpl::variable<QString> _status;
	rpl::lifetime _lifetime;

};

} // namespace Core
