#pragma once

// The result file of an update installation (task 17.1; design.md "Release metadata (GitHub REST
// subset)"; Requirement 13.14): the kyty_updater helper writes <install>/.kyty-update-result after
// an installation attempt, and the next launcher start reports it once and deletes it.
//
// Format: "success" or "failure" and a line feed, then the message (at most 64 KiB, no terminator).

#include "launcher/core/expected.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kyty::updater {

inline constexpr std::string_view kUpdateResultFileName        = ".kyty-update-result";
inline constexpr size_t           kMaxUpdateResultMessageBytes = 65536;

struct PreviousResult {
	bool        success = false;
	std::string message;

	bool operator==(const PreviousResult&) const = default;
};

// Writes <install_dir>/.kyty-update-result with result.message cut to kMaxUpdateResultMessageBytes
// bytes, without splitting a UTF-8 sequence at the cut. The bytes go to a new temporary file in
// install_dir, which is then renamed over the destination, so a file or a symbolic link there is
// replaced, never opened or written through; an empty directory there is removed first (as the
// helper did). The error names the step and the reason; the temporary file never stays.
kyty::core::Expected<void, std::string> WriteUpdateResult(const std::filesystem::path& install_dir,
                                                          const PreviousResult&        result);

// Reads <install_dir>/.kyty-update-result without following a symbolic link, at most
// 8 + kMaxUpdateResultMessageBytes bytes, and deletes whatever is at that path (an empty
// directory included) whenever something is there. nullopt when nothing is there. A file that
// cannot be read, is not a regular file, is larger than that, or does not start with "success\n"
// or "failure\n" gives {false, "The previous update result could not be read: <reason>"}, so the
// user still gets one message (Requirement 13.14).
std::optional<PreviousResult> ReadAndClearPreviousResult(const std::filesystem::path& install_dir);

} // namespace kyty::updater
