#include "settings/sections/settings_xxxgram.h"

#include "core/application.h"
#include "core/file_utilities.h"
#include "core/xxxgram_proxy.h"
#include "data/data_session.h"
#include "data/xxxgram_history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "settings/settings_builder.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/toast/toast.h"
#include "ui/wrap/padding_wrap.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtGui/QImageReader>

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace Settings {
namespace {

void ChooseIcon(not_null<Ui::GenericBox*> box) {
	FileDialog::GetOpenPath(
		box.get(),
		u"Иконка xxxgram"_q,
		u"Изображения (*.png *.jpg *.jpeg *.webp)"_q,
		crl::guard(box, [=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			auto reader = QImageReader(result.paths.front());
			const auto size = reader.size();
			if (!size.isValid() || qint64(size.width()) * size.height() > 16000000) {
				Ui::Toast::Show(u"Выберите изображение до 16 Мп"_q);
				return;
			}
			const auto image = reader.read();
			auto output = QSaveFile(cWorkingDir() + u"xxxgram-icon.png"_q);
			if (image.isNull()
				|| !output.open(QIODevice::WriteOnly)
				|| !image.save(&output, "PNG")
				|| !output.commit()) {
				Ui::Toast::Show(u"Не удалось сохранить иконку"_q);
				return;
			}
			const auto icons = QStandardPaths::writableLocation(
				QStandardPaths::GenericDataLocation) + u"/icons"_q;
			QDir().mkpath(icons);
			auto desktop = QSaveFile(icons + u"/org.xxxgram.desktop.png"_q);
			if (desktop.open(QIODevice::WriteOnly)) {
				if (image.save(&desktop, "PNG")) {
					desktop.commit();
				}
			}
			Core::App().refreshApplicationIcon();
			Ui::Toast::Show(u"Иконка обновлена"_q);
		}));
}

Ui::NumberInput *AddNumber(
		not_null<Ui::GenericBox*> box,
		const QString &title,
		int value,
		int maximum) {
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(title),
		st::boxLabel));
	const auto wrap = box->addRow(object_ptr<Ui::FixedHeightWidget>(
		box,
		st::defaultInputField.heightMin + 2 * st::boxPadding.bottom()));
	const auto input = Ui::CreateChild<Ui::NumberInput>(
		wrap,
		st::defaultInputField,
		rpl::single(QString()),
		QString::number(value),
		maximum);
	wrap->widthValue() | rpl::on_next([=](int width) {
		input->resize(width, input->height());
		input->moveToLeft(0, st::boxPadding.bottom());
	}, input->lifetime());
	input->setInputMethodHints(Qt::ImhDigitsOnly | Qt::ImhNoPredictiveText);
	return input;
}

void ShowLimits(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"xxxgram"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Скрытие активности включено. Полную невидимость Telegram не гарантирует."_q),
			st::boxLabel));
		const auto delay = AddNumber(box,
			u"Задержка отправки · 15–300 с"_q,
			session->settings().xxxgramSendDelaySeconds(),
			300);
		const auto records = AddNumber(box,
			u"История · 1–10 000 сообщений"_q,
			session->settings().xxxgramHistoryRecordLimit(),
			10000);
		const auto memory = AddNumber(box,
			u"Кэш истории · 1–64 МБ"_q,
			session->settings().xxxgramHistoryMegabytes(),
			64);
		const auto proxy = &Core::App().xxxgramWsProxy();
		const auto enabled = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"WS-прокси"_q,
			proxy->enabled()));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			proxy->statusValue(),
			st::boxLabel));
		const auto fallback = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Резерв через сторонние серверы"_q,
			proxy->fallbackEnabled()));
		const auto icon = box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Выбрать иконку…"_q)));
		icon->setClickedCallback([=] { ChooseIcon(box); });
		const auto save = [=] {
			const auto seconds = delay->getLastText().toInt();
			const auto count = records->getLastText().toInt();
			const auto megabytes = memory->getLastText().toInt();
			if (seconds < 15 || seconds > 300) {
				delay->showError();
				return;
			}
			if (count < 1 || count > 10000) {
				records->showError();
				return;
			}
			if (megabytes < 1 || megabytes > 64) {
				memory->showError();
				return;
			}
			session->settings().setXxxgramSendDelaySeconds(seconds);
			session->settings().setXxxgramHistoryRecordLimit(count);
			session->settings().setXxxgramHistoryMegabytes(megabytes);
			session->data().xxxgramHistory().limitsChanged();
			session->saveSettingsDelayed();
			if (!enabled->checked()) {
				proxy->setEnabled(false);
			}
			proxy->setFallbackEnabled(fallback->checked());
			proxy->setEnabled(enabled->checked());
			box->closeBox();
		};
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->addLeftButton(rpl::single(u"Очистить историю"_q), [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = u"Удалить локальные копии и прошлые версии?"_q,
				.confirmed = crl::guard(box, [=](Fn<void()> close) {
					session->data().xxxgramHistory().clear();
					close();
				}),
				.confirmText = u"Очистить"_q,
			}));
		});
	}));
}

} // namespace

void BuildXxxgramEntry(Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	builder.addButton({
		.id = u"main/xxxgram"_q,
		.title = rpl::single(u"xxxgram"_q),
		.icon = { &st::menuIconManage },
		.onClick = [=] { ShowLimits(controller); },
		.keywords = { u"xxxgram"_q, u"история"_q, u"задержка"_q, u"лимиты"_q },
	});
}

} // namespace Settings
