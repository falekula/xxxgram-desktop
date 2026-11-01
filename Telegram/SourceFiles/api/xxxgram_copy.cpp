#include "api/xxxgram_copy.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_document.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/view/controls/xxxgram_copy_progress.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QBuffer>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>

namespace Api {
namespace {

constexpr auto kDownloadTimeout = crl::time(5 * 60 * 1000);
constexpr auto kMemoryDownloadLimit = int64(256) * 1024 * 1024;

class CopyJob final : public std::enable_shared_from_this<CopyJob> {
public:
	CopyJob(
		const HistoryItemsList &items,
		std::vector<SendAction> actions,
		Data::ForwardOptions options,
		TextWithTags comment,
		FnMut<void()> queued,
		bool retry = false);
	void start();

private:
	struct Source {
		FullMsgId id;
		TextWithTags text;
		MessageGroupId group;
		std::shared_ptr<Data::PhotoMedia> photo;
		std::shared_ptr<Data::DocumentMedia> document;
		QByteArray voiceBytes;
		bool spoiler = false;
	};
	void check();
	void enqueue();
	void stop(QString error = {}, bool queued = false);
	[[nodiscard]] Ui::PreparedFile prepare(const Source &source) const;

	base::weak_ptr<Main::Session> _session;
	std::vector<SendAction> _actions;
	std::vector<Source> _sources;
	TextWithTags _comment;
	FnMut<void()> _queued;
	QString _error;
	base::Timer _timer;
	crl::time _started = 0;
	bool _finished = false;
	bool _retry = false;
	std::shared_ptr<CopyJob> _keepAlive;
	HistoryView::XxxgramCopyProgress _progress;

};

CopyJob::CopyJob(
		const HistoryItemsList &items,
		std::vector<SendAction> actions,
		Data::ForwardOptions options,
		TextWithTags comment,
		FnMut<void()> queued,
		bool retry)
: _session(base::make_weak(&actions.front().history->session()))
, _actions(std::move(actions))
, _comment(std::move(comment))
, _queued(std::move(queued))
, _timer([=] { check(); })
, _retry(retry)
, _progress(&_actions.front().history->session()) {
	if (ranges::any_of(_actions, [&](const SendAction &action) {
		return &action.history->session() != _session.get();
	})) {
		_error = u"Выберите получателей в одном аккаунте."_q;
		return;
	}
	for (const auto item : items) {
		if (&item->history()->session() != _session.get()) {
			_error = u"Нельзя копировать сообщения между аккаунтами напрямую."_q;
			return;
		}
		const auto media = item->media();
		if (item->richPage() || (media && media->invoice())
			|| (!item->xxxgramNeedsCopy() && item->forbidsSaving())) {
			_error = u"Этот тип сообщения нельзя скопировать."_q;
			return;
		}
		auto source = Source();
		source.id = item->fullId();
		source.group = item->groupId();
		source.spoiler = media && media->hasSpoiler();
		const auto &text = item->originalText();
		source.text = {
			text.text,
			TextUtilities::ConvertEntitiesToTextTags(text.entities),
		};
		if (media) {
			if (const auto photo = media->photo()) {
				source.photo = photo->createMediaView();
			} else if (const auto document = media->document()) {
				source.document = document->createMediaView();
			} else if (!media->webpage()) {
				_error = u"Выберите текст, фото, видео или файл."_q;
				return;
			}
		}
		if (options == Data::ForwardOptions::NoNamesAndCaptions
			&& (source.photo || source.document)) {
			source.text = {};
		}
		if (source.text.text.isEmpty() && !source.photo && !source.document) {
			_error = u"Сообщение не содержит доступного содержимого."_q;
			return;
		}
		_sources.push_back(std::move(source));
	}
}

void CopyJob::start() {
	_keepAlive = shared_from_this();
	if (!_error.isEmpty()) {
		stop(_error);
		return;
	}
	const auto weak = std::weak_ptr<CopyJob>(_keepAlive);
	_session->lifetime().add([weak] {
		if (const auto job = weak.lock()) {
			job->stop();
		}
	});
	_started = crl::now();
	auto downloadBytes = int64(0);
	for (const auto &source : _sources) {
		if (source.photo && !source.photo->loaded()) {
			downloadBytes += source.photo->owner()->imageByteSize(Data::PhotoSize::Large);
		} else if (source.document && !source.document->loaded(true)) {
			downloadBytes += source.document->owner()->size;
		}
		if (downloadBytes > kMemoryDownloadLimit) {
			stop(u"Сначала скачайте большие файлы в чат, затем повторите копирование."_q);
			return;
		}
	}
	for (const auto &source : _sources) {
		if (source.photo && !source.photo->loaded()) {
			source.photo->wanted(Data::PhotoSize::Large, source.id);
		} else if (source.document && !source.document->loaded(true)) {
			const auto document = source.document->owner();
			if (!document->loading()) {
				document->save(source.id, QString());
			}
		}
	}
	check();
	if (_finished) {
		return;
	}
	_progress.show([weak] {
		if (const auto job = weak.lock()) {
			job->stop();
		}
	});
}

void CopyJob::check() {
	if (_finished) {
		return;
	} else if (!_session) {
		stop();
		return;
	} else if (crl::now() - _started > kDownloadTimeout) {
		stop(u"Не удалось загрузить файлы. Проверьте подключение и повторите."_q);
		return;
	}
	auto ready = true;
	for (const auto &source : _sources) {
		if (source.photo && !source.photo->loaded()) {
			if (source.photo->owner()->failed(Data::PhotoSize::Large)) {
				stop(u"Не удалось загрузить фото. Повторите попытку."_q);
				return;
			}
			ready = false;
		} else if (source.document && !source.document->loaded(true)) {
			const auto document = source.document->owner();
			if (document->cancelled() || document->status == FileDownloadFailed) {
				stop(u"Загрузка файла прервана. Повторите попытку."_q);
				return;
			}
			ready = false;
		}
	}
	if (ready) {
		enqueue();
	} else {
		_timer.callOnce(250);
	}
}

Ui::PreparedFile CopyJob::prepare(const Source &source) const {
	auto file = Ui::PreparedFile(QString());
	file.caption = source.text;
	file.spoiler = source.spoiler;
	if (source.photo) {
		file.type = Ui::PreparedFile::Type::Photo;
		file.displayName = u"photo.jpg"_q;
		file.content = source.photo->imageBytes(Data::PhotoSize::Large);
		if (file.content.isEmpty()) {
			if (const auto image = source.photo->image(Data::PhotoSize::Large)) {
				auto buffer = QBuffer(&file.content);
				image->original().save(&buffer, "JPG", 95);
			}
		}
	} else {
		const auto document = source.document->owner();
		file.path = document->filepath(true);
		file.content = file.path.isEmpty() ? source.document->bytes() : QByteArray();
		file.displayName = document->filename();
		file.type = document->isVideoFile()
			? Ui::PreparedFile::Type::Video
			: document->isSong()
			? Ui::PreparedFile::Type::Music
			: Ui::PreparedFile::Type::File;
	}
	file.size = file.path.isEmpty() ? file.content.size() : QFileInfo(file.path).size();
	return file;
}

void CopyJob::enqueue() {
	const auto guard = shared_from_this();
	for (const auto &action : _actions) {
		const auto peer = action.history->peer;
		const auto error = GetErrorForSending(peer, {
			.topicRootId = action.replyTo.topicRootId,
			.text = &_comment,
			.messagesCount = int(_sources.size()) + !_comment.text.isEmpty(),
		});
		if (error) {
			stop(error.text);
			return;
		}
		for (const auto &source : _sources) {
			const auto right = source.photo
				? ChatRestriction::SendPhotos
				: source.document
				? source.document->owner()->requiredSendRight()
				: ChatRestriction::SendOther;
			if (!Data::CanSend(peer, right, false)) {
				stop(u"В этом чате нельзя отправить выбранные сообщения."_q);
				return;
			}
		}
	}
	auto voiceBytes = int64(0);
	for (auto &source : _sources) {
		if (source.photo || source.document) {
			const auto file = prepare(source);
			if (file.size <= 0) {
				stop(u"Файл больше недоступен. Скачайте его и повторите."_q);
				return;
			}
			const auto sizeLimit = _session->user()->isPremium()
				? kFileSizePremiumLimit : kFileSizeLimit;
			if (file.size > sizeLimit) {
				stop(u"Файл превышает лимит Telegram."_q);
				return;
			}
			if (!file.path.isEmpty()) {
				auto input = QFile(file.path);
				if (!input.open(QIODevice::ReadOnly)) {
					stop(u"Не удалось открыть скачанный файл."_q);
					return;
				}
			}
			if (source.document && (source.document->owner()->isVoiceMessage()
				|| source.document->owner()->isVideoMessage())) {
				voiceBytes += file.size;
				if (voiceBytes > kMemoryDownloadLimit) {
					stop(u"Выберите меньше голосовых сообщений за один раз."_q);
					return;
				}
				source.voiceBytes = source.document->bytes();
				if (source.voiceBytes.isEmpty()) {
					auto input = QFile(file.path);
					if (input.open(QIODevice::ReadOnly)) {
						source.voiceBytes = input.read(kMemoryDownloadLimit);
					}
				}
				if (source.voiceBytes.size() != file.size) {
					stop(u"Не удалось прочитать голосовое сообщение."_q);
					return;
				}
			}
		}
	}
	for (auto action : _actions) {
		action.clearDraft = false;
		if (!_comment.text.isEmpty()) {
			auto message = MessageToSend(action);
			message.textWithTags = _comment;
			_session->api().sendMessage(std::move(message));
		}
		for (auto i = size_t(0); i < _sources.size();) {
			const auto &source = _sources[i];
			const auto document = source.document
				? source.document->owner().get() : nullptr;
			if (document && (document->isVoiceMessage() || document->isVideoMessage())) {
				_session->api().sendVoiceMessage(
					source.voiceBytes,
					document->voice() ? document->voice()->waveform : VoiceWaveform(),
					document->duration(),
					document->isVideoMessage(),
					action);
				if (!source.text.text.isEmpty()) {
					auto message = MessageToSend(action);
					message.textWithTags = source.text;
					_session->api().sendMessage(std::move(message));
				}
				++i;
				continue;
			}
			if (!source.photo && !source.document) {
				auto message = MessageToSend(action);
				message.textWithTags = source.text;
				_session->api().sendMessage(std::move(message));
				++i;
				continue;
			}
			auto list = Ui::PreparedList();
			list.files.push_back(prepare(source));
			const auto first = i++;
			while (source.group && i < _sources.size() && i - first < 10
				&& _sources[i].group == source.group
				&& (_sources[i].photo || _sources[i].document)) {
				list.files.push_back(prepare(_sources[i++]));
			}
			const auto album = list.files.size() > 1
				? std::make_shared<SendingAlbum>() : nullptr;
			const auto type = source.photo || album
				|| (source.document && source.document->owner()->isVideoFile())
				? SendMediaType::Photo : SendMediaType::File;
			_session->api().sendFiles(std::move(list), type, album, action);
		}
	}
	auto queued = std::move(_queued);
	stop({}, true);
	if (queued) {
		queued();
	}
}

void CopyJob::stop(QString error, bool queued) {
	const auto guard = shared_from_this();
	if (std::exchange(_finished, true)) {
		return;
	}
	_timer.cancel();
	_progress.finish(std::move(error), queued
		? (_retry
			? u"Новая попытка создана. Исходное сообщение сохранено."_q
			: u"Копия добавлена в очередь"_q)
		: QString());
	_keepAlive.reset();
}

} // namespace

bool XxxgramCopyIfNeeded(
		const HistoryItemsList &items,
		std::vector<SendAction> actions,
		Data::ForwardOptions options,
		TextWithTags comment,
		FnMut<void()> &&queued) {
	if (items.empty() || actions.empty()
		|| ranges::none_of(items, &HistoryItem::xxxgramNeedsCopy)) {
		return false;
	}
	std::make_shared<CopyJob>(
		items, std::move(actions), options, std::move(comment), std::move(queued)
	)->start();
	return true;
}

void XxxgramRetryCopy(
		const HistoryItemsList &items,
		SendAction action) {
	if (items.empty()) {
		return;
	}
	std::make_shared<CopyJob>(
		items,
		std::vector<SendAction>{ action },
		Data::ForwardOptions::NoSenderNames,
		TextWithTags(),
		nullptr,
		true
	)->start();
}

} // namespace Api
