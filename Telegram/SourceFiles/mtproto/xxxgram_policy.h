#pragma once

#include <algorithm>
#include <cstdint>

namespace MTP::Xxxgram {

[[nodiscard]] constexpr int ClampDelay(int seconds) {
	return std::clamp(seconds, 15, 300);
}

[[nodiscard]] constexpr std::int64_t AutomaticDate(
		std::int64_t now,
		int delay) {
	return now + ClampDelay(delay);
}

// WHY: Telegram may immediately send dates less than 10 seconds in the future.
// Stop before that threshold, including a small transport safety margin.
[[nodiscard]] constexpr bool ScheduleExpired(
		std::int64_t deadline,
		std::int64_t now) {
	return deadline && deadline <= now + 12;
}

} // namespace MTP::Xxxgram
