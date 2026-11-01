#include "api/xxxgram_policy.h"

#include "base/unixtime.h"
#include "data/components/ephemeral_messages.h"
#include "history/history.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mtproto/xxxgram_policy.h"

namespace Api {

TimeId XxxgramAutomaticDate(not_null<History*> history) {
	return MTP::Xxxgram::AutomaticDate(
		base::unixtime::now(),
		history->session().settings().xxxgramSendDelaySeconds());
}

void ApplyXxxgramSchedule(SendAction &action, const QString &text) {
	const auto &options = action.options;
	if (options.scheduled
		|| options.shortcutId
		|| options.welcomeTemplate
		|| options.suggest
		|| action.replaceMediaOf) {
		return;
	}
	if (action.history->session().ephemeralMessages().wouldSendMedia(
			action.history->peer,
			action.replyTo,
			text)) {
		return;
	}
	action.options.scheduled = XxxgramAutomaticDate(action.history);
	action.options.xxxgramAutomaticSchedule = true;
}

XxxgramScheduleDate XxxgramSchedule(const SendOptions &options) {
	return { options.scheduled, options.xxxgramAutomaticSchedule };
}

} // namespace Api
