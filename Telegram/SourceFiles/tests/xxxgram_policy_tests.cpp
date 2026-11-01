#include "mtproto/xxxgram_policy.h"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
	using namespace MTP::Xxxgram;
	assert(ClampDelay(std::numeric_limits<int>::min()) == 15);
	assert(ClampDelay(0) == 15);
	assert(ClampDelay(14) == 15);
	assert(ClampDelay(15) == 15);
	assert(ClampDelay(300) == 300);
	assert(ClampDelay(301) == 300);
	assert(ClampDelay(std::numeric_limits<int>::max()) == 300);
	for (auto delay = 15; delay <= 300; ++delay) {
		assert(ClampDelay(delay) == delay);
		const auto created = std::int64_t(1800000000);
		const auto deadline = AutomaticDate(created, delay);
		assert(!ScheduleExpired(deadline, created));
		assert(!ScheduleExpired(deadline, deadline - 13));
		assert(ScheduleExpired(deadline, deadline - 12));
		assert(ScheduleExpired(deadline, deadline - 10));
		assert(ScheduleExpired(deadline, deadline));
		assert(ScheduleExpired(deadline, deadline + 86400));
		assert(!ScheduleExpired(0, deadline + 86400));
		const auto afterUpload = created + 900;
		assert(ScheduleExpired(deadline, afterUpload));
		assert(!ScheduleExpired(AutomaticDate(afterUpload, delay), afterUpload));
	}
}
