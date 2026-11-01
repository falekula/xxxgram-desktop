#include "history/view/controls/xxxgram_queue_button.h"

#include "api/api_common.h"
#include "base/unixtime.h"
#include "data/components/scheduled_messages.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/painter.h"

#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"

namespace HistoryView {

XxxgramQueueButton::XxxgramQueueButton(
		QWidget *parent,
		Fn<std::pair<History*, MsgId>()> context)
: Ui::IconButton(parent, st::historyScheduledToggle)
, _context(std::move(context))
, _timer([=] {
	if (isVisible()) {
		update();
	}
}) {
	resize(st().width + st::msgDateFont->width(u"0:00"_q) + st::lineWidth * 8,
		st().height);
	setAccessibleName(u"Очередь отправки"_q);
}

QString XxxgramQueueButton::statusText() {
	const auto [history, topicRootId] = _context
		? _context() : std::pair<History*, MsgId>{ nullptr, 0 };
	auto nearest = TimeId(0);
	auto pending = false;
	auto failed = false;
	if (history) {
		const auto &session = history->session();
		for (const auto id : session.scheduledMessages().list(history).ids) {
			const auto item = session.data().message(id);
			if (!item || (topicRootId && item->topicRootId() != topicRootId)) {
				continue;
			} else if (item->hasFailed()) {
				failed = true;
			} else if (item->isSending()) {
				pending = true;
			} else if (item->date() < Api::kScheduledUntilOnlineTimestamp) {
				nearest = nearest ? std::min(nearest, item->date()) : item->date();
			}
		}
	}
	const auto remaining = nearest ? std::max(TimeId(0),
		nearest - base::unixtime::now()) : TimeId(0);
	auto text = QString();
	auto description = QString();
	if (nearest && remaining <= 300) {
		text = u"%1:%2"_q.arg(remaining / 60).arg(remaining % 60, 2, 10, QChar('0'));
		description = remaining
			? u"Отправка через %1 с"_q.arg(remaining)
			: u"Ожидаем отправку"_q;
	} else if (pending) {
		text = u"↑"_q;
		description = u"Подготовка или загрузка"_q;
	} else if (failed) {
		text = u"!"_q;
		description = u"Ошибка отправки"_q;
	}
	if (failed && text != u"!"_q) {
		description += u" · Есть ошибка"_q;
	}
	if (!description.isEmpty()) {
		description += u" · "_q;
	}
	description += u"Открыть очередь"_q;
	if (_statusDescription != description) {
		_statusDescription = description;
		setAccessibleName(description);
		setToolTip(description);
	}
	return text;
}

void XxxgramQueueButton::paintEvent(QPaintEvent*) {
	const auto text = statusText();
	auto p = Painter(this);
	paintRipple(p, st().rippleAreaPosition);
	const auto over = iconOverOpacity();
	const auto position = QPoint(
		st().iconPosition.x() < 0
			? (st().width - st().icon.width()) / 2 : st().iconPosition.x(),
		st().iconPosition.y() < 0
			? (height() - st().icon.height()) / 2 : st().iconPosition.y());
	st().icon.paint(p, position, width());
	if (over > 0. && !st().iconOver.empty()) {
		p.setOpacity(over);
		st().iconOver.paint(p, position, width());
		p.setOpacity(1.);
	}
	if (!text.isEmpty()) {
		p.setFont(st::msgDateFont);
		p.setPen(isOver() ? st::historyComposeIconFgOver : st::historyComposeIconFg);
		p.drawTextLeft(st().width, (height() - st::msgDateFont->height) / 2,
			width(), text);
	}
	if (!_timer.isActive() && isVisible()) {
		_timer.callOnce(1000);
	}
}

void XxxgramQueueButton::hideEvent(QHideEvent *event) {
	_timer.cancel();
	Ui::IconButton::hideEvent(event);
}

} // namespace HistoryView
