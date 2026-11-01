#pragma once

#include "api/api_common.h"
#include "data/data_types.h"

namespace Api {

// queued confirms local enqueueing, not delivery to Telegram.
[[nodiscard]] bool XxxgramCopyIfNeeded(
	const HistoryItemsList &items,
	std::vector<SendAction> actions,
	Data::ForwardOptions options,
	TextWithTags comment = {},
	FnMut<void()> &&queued = nullptr);

void XxxgramRetryCopy(
	const HistoryItemsList &items,
	SendAction action,
	FnMut<void()> &&queued = nullptr);

} // namespace Api
