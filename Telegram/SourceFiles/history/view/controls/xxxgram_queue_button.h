#pragma once

#include "base/timer.h"
#include "data/data_types.h"
#include "ui/widgets/buttons.h"

class History;

namespace HistoryView {

class XxxgramQueueButton final : public Ui::IconButton {
public:
	XxxgramQueueButton(
		QWidget *parent,
		Fn<std::pair<History*, MsgId>()> context);

protected:
	void paintEvent(QPaintEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	[[nodiscard]] QString statusText();

	Fn<std::pair<History*, MsgId>()> _context;
	base::Timer _timer;
	QString _statusDescription;

};

} // namespace HistoryView
