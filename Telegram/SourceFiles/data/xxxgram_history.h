#pragma once

#include "base/timer.h"
#include "base/flat_map.h"
#include "data/data_msg_id.h"

class History;
class HistoryItem;

namespace Data {

class Session;

class XxxgramHistory final {
public:
	explicit XxxgramHistory(not_null<Session*> owner);
	~XxxgramHistory();

	[[nodiscard]] static bool Eligible(not_null<const HistoryItem*> item);
	void observe(not_null<History*> history, const MTPMessage &message);
	[[nodiscard]] bool retainDeleted(MsgId id);
	[[nodiscard]] bool isDeleted(FullMsgId id) const;
	void forget(FullMsgId id);
	void forgetPeer(PeerId peer);
	[[nodiscard]] MTPMessage messageForDisplay(
		not_null<History*> history,
		const MTPMessage &message) const;
	[[nodiscard]] QString label(FullMsgId id) const;
	[[nodiscard]] QString versions(FullMsgId id) const;
	[[nodiscard]] QVector<MTPMessage> mergeSlice(
		not_null<History*> history,
		const QVector<MTPMessage> &slice,
		bool older) const;
	[[nodiscard]] bool ignoresExpiredMedia(
		FullMsgId id,
		const MTPMessage &message) const;
	[[nodiscard]] bool ignoresExpiredMedia(
		FullMsgId id,
		const MTPDmessage &message) const;
	void restoreMedia(not_null<HistoryItem*> item);
	void limitsChanged();
	void clear();
	[[nodiscard]] int count() const;
	[[nodiscard]] qint64 bytes() const;

private:
	struct Record {
		QByteArray message;
		QString text;
		QStringList versions;
		QByteArray media;
		quint64 order = 0;
		int timer = 0;
		bool deleted = false;
		bool photo = false;
	};
	void read();
	void write();
	void changed();
	void trim();
	void captureMedia();
	void releaseItem(
		FullMsgId id,
		const QByteArray &message,
		bool deleted,
		bool timed);
	[[nodiscard]] qint64 size(const Record &record) const;

	const not_null<Session*> _owner;
	base::flat_map<FullMsgId, Record> _records;
	quint64 _order = 0;
	bool _dirty = false;
	bool _restoringMedia = false;
	base::Timer _writeTimer;
	rpl::lifetime _lifetime;
};

} // namespace Data
