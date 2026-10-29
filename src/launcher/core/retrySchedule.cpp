#include "launcher/core/retrySchedule.h"

namespace kyty::core {

std::optional<std::chrono::milliseconds> RetryDelayAfter(int failed_attempt) {
	if (failed_attempt < 1 || failed_attempt >= kMaxDownloadAttempts) {
		return std::nullopt;
	}
	return kRetryDelayStep * failed_attempt;
}

} // namespace kyty::core
