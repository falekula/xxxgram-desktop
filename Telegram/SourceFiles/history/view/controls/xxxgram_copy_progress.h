#pragma once

namespace Main {
class Session;
} // namespace Main

namespace HistoryView {

class XxxgramCopyProgress final {
public:
	explicit XxxgramCopyProgress(not_null<Main::Session*> session);
	~XxxgramCopyProgress();
	void show(Fn<void()> cancel);
	void finish(QString error, QString successText);

private:
	struct Impl;
	std::unique_ptr<Impl> _impl;

};

} // namespace HistoryView
