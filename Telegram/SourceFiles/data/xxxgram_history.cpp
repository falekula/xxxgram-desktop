#include "data/xxxgram_history.h"

#include "base/call_delayed.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "storage/storage_account.h"

#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtGui/QImage>

namespace Data {
namespace {

constexpr auto kStorageKey = "xxxgram.history.v2";
constexpr auto kMagic = quint32(0x58584732);
constexpr auto kMaximumBytes = 64 * 1024 * 1024;
constexpr auto kMaximumRecords = 10000;
constexpr auto kMaximumVersions = 30;

bool EligiblePeer(not_null<History*> history) {
	const auto user = history->peer->asUser();
	return user && !user->isBot() && !user->isSelf()
		&& !user->isNotificationsUser()
		&& !user->isRepliesChat() && !user->isVerifyCodes();
}

QByteArray Encode(const MTPMessage &message) {
	auto words = mtpBuffer();
	message.write(words);
	return QByteArray(
		reinterpret_cast<const char*>(words.constData()),
		words.size() * sizeof(mtpPrime));
}

std::optional<MTPMessage> Decode(const QByteArray &bytes) {
	if (bytes.isEmpty() || (bytes.size() % sizeof(mtpPrime))) {
		return std::nullopt;
	}
	auto words = mtpBuffer(bytes.size() / sizeof(mtpPrime));
	memcpy(words.data(), bytes.constData(), bytes.size());
	auto from = words.constData();
	const auto end = from + words.size();
	auto result = MTPMessage();
	return (result.read(from, end) && from == end)
		? std::make_optional(std::move(result))
		: std::nullopt;
}

uint64 MediaIdentity(const MTPDmessage &message) {
	const auto media = message.vmedia();
	return media ? media->match([](const MTPDmessageMediaPhoto &photo) {
		return photo.vphoto() ? photo.vphoto()->match(
			[](const MTPDphoto &data) { return data.vid().v; },
			[](const auto &) { return uint64(0); }) : uint64(0);
	}, [](const MTPDmessageMediaDocument &document) {
		return document.vdocument() ? document.vdocument()->match(
			[](const MTPDdocument &data) { return data.vid().v; },
			[](const auto &) { return uint64(0); }) : uint64(0);
	}, [](const auto &) {
		return uint64(0);
	}) : uint64(0);
}

} // namespace

XxxgramHistory::XxxgramHistory(not_null<Session*> owner)
: _owner(owner)
, _writeTimer([=] { write(); }) {
	read();
	_owner->session().downloaderTaskFinished(
	) | rpl::on_next([=] {
		captureMedia();
	}, _lifetime);
}

XxxgramHistory::~XxxgramHistory() {
	write();
}

bool XxxgramHistory::Eligible(not_null<const HistoryItem*> item) {
	return EligiblePeer(item->history()) && item->isRegular()
		&& item->history()->owner().xxxgramHistory()._records.contains(item->fullId());
}

void XxxgramHistory::observe(
		not_null<History*> history,
		const MTPMessage &message) {
	if (!EligiblePeer(history) || message.type() != mtpc_message) {
		return;
	}
	const auto &data = message.c_message();
	if (data.vid().v <= 0) {
		return;
	}
	const auto id = FullMsgId(history->peer->id, data.vid().v);
	if (ignoresExpiredMedia(id, message)) {
		return;
	}
	const auto encoded = Encode(message);
	if (encoded.size() > kMaximumBytes) {
		return;
	}
	auto &record = _records[id];
	if (record.deleted || record.message == encoded) {
		return;
	}
	const auto text = qs(data.vmessage());
	const auto previous = Decode(record.message);
	const auto mediaChanged = previous && previous->type() == mtpc_message
		&& MediaIdentity(previous->c_message()) != MediaIdentity(data);
	if (!record.message.isEmpty() && (record.text != text || mediaChanged)) {
		record.versions.prepend((mediaChanged ? u"[Медиа заменено]\n"_q : QString())
			+ (record.text.isEmpty() ? u"[Без текста]"_q : record.text));
		while (record.versions.size() > kMaximumVersions) {
			record.versions.removeLast();
		}
	}
	if (mediaChanged) {
		record.media.clear();
	}
	record.message = encoded;
	record.text = text;
	record.order = ++_order;
	if (const auto ttl = data.vttl_period()) {
		record.timer = ttl->v;
	}
	if (const auto media = data.vmedia()) {
		media->match([&](const MTPDmessageMediaPhoto &photo) {
			if (const auto ttl = photo.vttl_seconds()) {
				record.timer = ttl->v;
			}
			record.photo = true;
		}, [&](const MTPDmessageMediaDocument &document) {
			if (const auto ttl = document.vttl_seconds()) {
				record.timer = ttl->v;
			}
		}, [](const auto &) {
		});
	}
	changed();
}

bool XxxgramHistory::retainDeleted(MsgId id) {
	for (auto &[key, record] : _records) {
		if (key.msg != id) {
			continue;
		}
		record.deleted = true;
		record.order = ++_order;
		if (const auto item = _owner->message(key)) {
			item->xxxgramDetachFromGroup();
			_owner->notifyItemDataChange(item);
			_owner->requestItemResize(item);
			_owner->requestItemRepaint(item);
		}
		captureMedia();
		changed();
		return true;
	}
	return false;
}

bool XxxgramHistory::isDeleted(FullMsgId id) const {
	const auto i = _records.find(id);
	return i != end(_records) && i->second.deleted;
}

void XxxgramHistory::forget(FullMsgId id) {
	if (_records.remove(id)) {
		changed();
	}
}

void XxxgramHistory::forgetPeer(PeerId peer) {
	for (auto i = begin(_records); i != end(_records);) {
		if (i->first.peer == peer) {
			i = _records.erase(i);
		} else {
			++i;
		}
	}
	changed();
}

MTPMessage XxxgramHistory::messageForDisplay(
		not_null<History*> history,
		const MTPMessage &message) const {
	const auto id = FullMsgId(history->peer->id, IdFromMessage(message));
	if (isDeleted(id) || ignoresExpiredMedia(id, message)) {
		const auto i = _records.find(id);
		if (i != end(_records)) {
			if (const auto saved = Decode(i->second.message)) {
				return *saved;
			}
		}
	}
	return message;
}

QString XxxgramHistory::label(FullMsgId id) const {
	const auto i = _records.find(id);
	if (i == end(_records)) {
		return {};
	}
	const auto &record = i->second;
	auto parts = QStringList();
	if (record.deleted) {
		parts.push_back(u"🗑 удалено"_q);
	} else if (!record.versions.isEmpty()) {
		parts.push_back(u"✎"_q);
	}
	if (record.timer) {
		parts.push_back(record.timer == 0x7FFFFFFF
			? u"1 просмотр"_q
			: u"%1 с"_q.arg(record.timer));
	}
	return parts.join(u" · "_q);
}

QString XxxgramHistory::versions(FullMsgId id) const {
	const auto i = _records.find(id);
	if (i == end(_records)) {
		return {};
	}
	return i->second.versions.join(u"\n\n────────\n\n"_q);
}

QVector<MTPMessage> XxxgramHistory::mergeSlice(
		not_null<History*> history,
		const QVector<MTPMessage> &slice,
		bool older) const {
	if (!EligiblePeer(history)) {
		return slice;
	}
	auto messages = base::flat_map<MsgId, MTPMessage>();
	for (const auto &message : slice) {
		messages.emplace(IdFromMessage(message), message);
	}
	const auto currentLow = history->minMsgId();
	const auto currentHigh = history->maxMsgId();
	if (messages.empty() && !currentLow && !currentHigh
		&& !history->loadedAtBottom()) {
		return slice;
	}
	const auto low = messages.empty()
		? (older ? MsgId(0) : currentHigh)
		: (!older && currentHigh
			? std::min(currentHigh, messages.begin()->first)
			: messages.begin()->first);
	const auto high = messages.empty()
		? (older && currentLow ? currentLow : ServerMaxMsgId)
		: (history->isEmpty() && history->loadedAtBottom() ? ServerMaxMsgId
			: older && currentLow
			? std::max(currentLow, messages.rbegin()->first)
			: messages.rbegin()->first);
	for (const auto &[id, record] : _records) {
		if (id.peer != history->peer->id || (!record.deleted && !record.timer)
			|| id.msg < low || id.msg > high) {
			continue;
		}
		if (const auto item = _owner->message(id); item && item->mainView()) {
			continue;
		}
		if (const auto message = Decode(record.message)) {
			messages.emplace(id.msg, *message);
		}
	}
	auto result = QVector<MTPMessage>();
	result.reserve(messages.size());
	for (auto i = messages.rbegin(); i != messages.rend(); ++i) {
		result.push_back(i->second);
	}
	return result;
}

bool XxxgramHistory::ignoresExpiredMedia(
		FullMsgId id,
		const MTPMessage &message) const {
	return message.type() == mtpc_message
		&& ignoresExpiredMedia(id, message.c_message());
}

bool XxxgramHistory::ignoresExpiredMedia(
		FullMsgId id,
		const MTPDmessage &message) const {
	const auto i = _records.find(id);
	if (i == end(_records) || !i->second.timer) {
		return false;
	}
	const auto media = message.vmedia();
	return media && media->match([](const MTPDmessageMediaPhoto &photo) {
		return photo.vttl_seconds().has_value()
			&& (!photo.vphoto() || photo.vphoto()->type() == mtpc_photoEmpty);
	}, [](const MTPDmessageMediaDocument &document) {
		return document.vttl_seconds().has_value()
			&& (!document.vdocument()
				|| document.vdocument()->type() == mtpc_documentEmpty);
	}, [](const auto &) {
		return false;
	});
}

void XxxgramHistory::restoreMedia(not_null<HistoryItem*> item) {
	const auto i = _records.find(item->fullId());
	if (i == end(_records) || i->second.media.isEmpty() || !item->media()) {
		return;
	}
	const auto &bytes = i->second.media;
	_restoringMedia = true;
	if (const auto photo = item->media()->photo()) {
		auto image = QImage::fromData(bytes);
		if (!image.isNull()) {
			const auto view = photo->createMediaView();
			view->set(
				PhotoSize::Large, PhotoSize::Large, std::move(image), bytes);
			_owner->keepAlive(view);
		}
	} else if (const auto document = item->media()->document()) {
		const auto view = document->createMediaView();
		view->setBytes(bytes);
		_owner->keepAlive(view);
	}
	_restoringMedia = false;
}

void XxxgramHistory::captureMedia() {
	if (_restoringMedia) {
		return;
	}
	const auto limit = _owner->session().settings().xxxgramHistoryMegabytes()
		* 1024LL * 1024;
	auto any = false;
	for (auto &[id, record] : _records) {
		if ((!record.timer && !record.deleted) || !record.media.isEmpty()) {
			continue;
		}
		const auto item = _owner->message(id);
		const auto media = item ? item->media() : nullptr;
		if (!media) {
			continue;
		}
		auto bytes = QByteArray();
		if (const auto photo = media->photo()) {
			if (const auto view = photo->activeMediaView()) {
				bytes = view->imageBytes(PhotoSize::Large);
			}
		} else if (const auto document = media->document()) {
			if (const auto view = document->activeMediaView()) {
				bytes = view->bytes();
			}
			if (bytes.isEmpty()) {
				auto file = QFile(document->filepath(true));
				if (file.size() > 0 && file.size() < limit
					&& file.open(QIODevice::ReadOnly)) {
					bytes = file.read(limit);
				}
			}
		}
		if (!bytes.isEmpty() && size(record) + bytes.size() < limit) {
			record.media = std::move(bytes);
			record.order = ++_order;
			any = true;
		}
	}
	if (any) {
		changed();
	}
}

void XxxgramHistory::clear() {
	const auto previous = std::exchange(_records, {});
	for (const auto &[id, record] : previous) {
		releaseItem(id, record.message, record.deleted, record.timer != 0);
	}
	_dirty = true;
	write();
}

void XxxgramHistory::releaseItem(
		FullMsgId id,
		const QByteArray &message,
		bool deleted,
		bool timed) {
	if (_records.contains(id)) {
		return;
	}
	const auto item = _owner->message(id);
	if (!item) {
		return;
	}
	if (deleted) {
		_owner->destroyMessageWithCacheCleanup(item);
		return;
	}
	if (timed) {
		if (const auto saved = Decode(message); saved && saved->type() == mtpc_message) {
			item->applyEdition(HistoryMessageEdition(
				&_owner->session(), saved->c_message()));
			item->clearMediaAsExpired();
		}
	}
	_owner->notifyItemDataChange(item);
	_owner->requestItemResize(item);
	_owner->requestItemRepaint(item);
}

void XxxgramHistory::limitsChanged() {
	trim();
	_dirty = true;
	write();
}

int XxxgramHistory::count() const {
	return _records.size();
}

qint64 XxxgramHistory::size(const Record &record) const {
	auto result = qint64(64) + record.message.size()
		+ record.text.size() * 2 + record.media.size();
	for (const auto &version : record.versions) {
		result += 4 + version.size() * 2;
	}
	return result;
}

qint64 XxxgramHistory::bytes() const {
	auto result = qint64(16);
	for (const auto &[id, record] : _records) {
		result += size(record);
	}
	return result;
}

void XxxgramHistory::trim() {
	const auto &settings = _owner->session().settings();
	const auto countLimit = std::clamp(
		settings.xxxgramHistoryRecordLimit(), 1, kMaximumRecords);
	const auto byteLimit = std::clamp(
		settings.xxxgramHistoryMegabytes(), 1, 64) * 1024LL * 1024;
	auto total = bytes();
	while (!_records.empty()
		&& (_records.size() > countLimit || total > byteLimit)) {
		const auto oldest = ranges::min_element(_records, {}, [](const auto &i) {
			return i.second.order;
		});
		const auto id = oldest->first;
		const auto deleted = oldest->second.deleted;
		const auto timed = oldest->second.timer != 0;
		const auto message = oldest->second.message;
		total -= size(oldest->second);
		_records.erase(oldest);
		// Defer eviction because an item may still be under construction.
		const auto session = &_owner->session();
		crl::on_main(session, [=] {
			session->data().xxxgramHistory().releaseItem(id, message, deleted, timed);
		});
	}
}

void XxxgramHistory::changed() {
	trim();
	_dirty = true;
	_writeTimer.callOnce(500);
}

void XxxgramHistory::write() {
	_writeTimer.cancel();
	if (!_dirty) {
		return;
	}
	_dirty = false;
	if (_records.empty()) {
		_owner->session().local().clearPref(kStorageKey);
		return;
	}
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kMagic << quint64(_owner->session().userId().bare)
		<< quint32(_records.size());
	for (const auto &[id, record] : _records) {
		stream << quint64(id.peer.value) << qint64(id.msg.bare)
			<< record.message << record.text << record.versions
			<< record.media << record.order << qint32(record.timer)
			<< record.deleted << record.photo;
	}
	_owner->session().local().writePref<QByteArray>(kStorageKey, bytes);
}

void XxxgramHistory::read() {
	const auto bytes = _owner->session().local().readPref<QByteArray>(kStorageKey);
	if (bytes.isEmpty() || bytes.size() > kMaximumBytes) {
		return;
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32(), count = quint32();
	auto owner = quint64();
	stream >> magic >> owner >> count;
	if (stream.status() != QDataStream::Ok || magic != kMagic
		|| owner != _owner->session().userId().bare
		|| count > kMaximumRecords) {
		return;
	}
	auto records = base::flat_map<FullMsgId, Record>();
	for (auto index = quint32(); index < count; ++index) {
		auto peer = quint64();
		auto messageId = qint64();
		auto record = Record();
		auto timer = qint32();
		stream >> peer >> messageId >> record.message >> record.text
			>> record.versions >> record.media >> record.order >> timer
			>> record.deleted >> record.photo;
		const auto message = Decode(record.message);
		if (stream.status() != QDataStream::Ok || messageId <= 0
			|| !message || message->type() != mtpc_message
			|| IdFromMessage(*message).bare != messageId
			|| peerFromMTP(message->c_message().vpeer_id()).value != peer
			|| record.versions.size() > kMaximumVersions) {
			return;
		}
		record.timer = timer;
		_order = std::max(_order, record.order);
		records.emplace(FullMsgId(PeerId(peer), MsgId(messageId)), std::move(record));
	}
	if (stream.status() == QDataStream::Ok && stream.atEnd()) {
		_records = std::move(records);
	}
}

} // namespace Data
