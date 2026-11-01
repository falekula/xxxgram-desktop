#include "history/view/controls/xxxgram_copy_progress.h"

#include "base/weak_ptr.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_session_controller.h"

#include <QtCore/QPointer>

namespace HistoryView {

struct XxxgramCopyProgress::Impl {
	base::weak_ptr<Main::Session> session;
	QPointer<Ui::GenericBox> box;
};

XxxgramCopyProgress::XxxgramCopyProgress(not_null<Main::Session*> session)
: _impl(std::make_unique<Impl>()) {
	_impl->session = base::make_weak(session.get());
}

XxxgramCopyProgress::~XxxgramCopyProgress() {
	if (_impl->box) {
		_impl->box->closeBox();
	}
}

void XxxgramCopyProgress::show(Fn<void()> cancel) {
	const auto session = _impl->session.get();
	const auto window = session ? session->tryResolveWindow() : nullptr;
	if (!window) {
		return;
	}
	auto box = Ui::MakeInformBox(Ui::ConfirmBoxArgs{
		.text = u"Загружаем файлы для отправки копии…"_q,
		.confirmed = [=] { cancel(); },
		.cancelled = [=] { cancel(); },
		.confirmText = u"Отмена"_q,
		.title = u"Копирование"_q,
	});
	_impl->box = box.data();
	window->show(std::move(box));
}

void XxxgramCopyProgress::finish(QString error, QString successText) {
	if (_impl->box) {
		_impl->box->closeBox();
		_impl->box = nullptr;
	}
	const auto session = _impl->session.get();
	const auto window = session ? session->tryResolveWindow() : nullptr;
	if (!window) {
		return;
	}
	if (!error.isEmpty()) {
		window->show(Ui::MakeInformBox(std::move(error)));
	} else if (!successText.isEmpty()) {
		window->showToast(std::move(successText));
	}
}

} // namespace HistoryView
