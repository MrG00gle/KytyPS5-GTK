#pragma once

// The installation the GTK launcher runs from (task 19.3; plan D15): its own directory, the
// Emulator next to it, and the Emulator's version text.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace kyty::gtk {

inline constexpr std::string_view kEmulatorExecutableName = "kyty_emulator";

// The directory of the running executable, from /proc/self/exe; empty when it cannot be read.
std::filesystem::path LauncherDirectory();

struct EmulatorLocation {
	// The Emulator executable, or the first candidate when none exists (the "Emulator not found at
	// <path>" text names it, and its directory is the Data_Directory base outside Flatpak).
	std::filesystem::path executable;
	bool                  found = false;
};

// As in the Qt launcher: <launcher_dir>/kyty_emulator when it exists, else
// <launcher_dir>/../kyty_emulator when that exists, else the first one with found false. The
// result is lexically normal.
EmulatorLocation LocateEmulator(const std::filesystem::path& launcher_dir);

// The version text in the standard output of the Emulator run without arguments (the Qt launcher's
// rule): of the non-empty lines (split at '\r' and '\n'), the second when the first starts with
// "exe_name", else the first; "" when that line does not exist. Bytes that are not valid UTF-8 are
// replaced (core::SanitizeUtf8). Informational only: unlike the Qt launcher, a missing version
// never makes the Emulator "not found".
std::string EmulatorVersionFromOutput(std::string_view output);

// Runs the Emulator without arguments (standard input /dev/null, standard error discarded) for at
// most limit, then calls done once from the main loop with EmulatorVersionFromOutput of what it
// wrote to standard output ("" after the limit). Destroying the probe kills a process that is still
// running; done is never called after that.
class EmulatorVersionProbe {
public:
	using DoneFn = std::function<void(std::string version)>;

	// Starts the process; nullptr (with a log message) when it cannot be started.
	static std::unique_ptr<EmulatorVersionProbe>
	Start(const std::filesystem::path& executable, DoneFn done,
	      std::chrono::milliseconds limit = std::chrono::seconds(5));

	EmulatorVersionProbe(const EmulatorVersionProbe&)            = delete;
	EmulatorVersionProbe& operator=(const EmulatorVersionProbe&) = delete;
	~EmulatorVersionProbe();

private:
	struct State;

	explicit EmulatorVersionProbe(State* state): m_state(state) {}

	State* m_state; // owned by the pending operation, which outlives the probe
};

} // namespace kyty::gtk
