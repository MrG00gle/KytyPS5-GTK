#pragma once

// The download retry schedule of the Compatibility_Database (Requirement 8.2): at most 4 attempts
// per launcher start, waiting 750 ms before the first retry, 1500 ms before the second, and
// 2250 ms before the third.

#include <chrono>
#include <optional>

namespace kyty::core {

// The largest number of download attempts per launcher start.
inline constexpr int kMaxDownloadAttempts = 4;

// The wait between two retries grows by this step: retry n waits n * kRetryDelayStep.
inline constexpr std::chrono::milliseconds kRetryDelayStep {750};

// The delay before the attempt that follows failed attempt number failed_attempt (1-based): 750 ms
// after attempt 1, 1500 ms after attempt 2, 2250 ms after attempt 3. nullopt when no attempt
// remains (failed_attempt >= kMaxDownloadAttempts) or failed_attempt is not a valid attempt number
// (< 1).
std::optional<std::chrono::milliseconds> RetryDelayAfter(int failed_attempt);

} // namespace kyty::core
