#pragma once

#include "api/api_common.h"

namespace Api {

struct XxxgramScheduleDate {
	TimeId value = 0;
	bool automatic = false;
};

void ApplyXxxgramSchedule(SendAction &action, const QString &text = {});
[[nodiscard]] XxxgramScheduleDate XxxgramSchedule(const SendOptions &options);
[[nodiscard]] TimeId XxxgramAutomaticDate(not_null<History*> history);

} // namespace Api
