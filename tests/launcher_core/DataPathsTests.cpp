// Example tests of the run-time data locations (src/launcher/core/dataPaths.h, task 15.2): every
// environment combination of the Flatpak marker, XDG_CONFIG_HOME, XDG_DATA_HOME, HOME, and a
// portable Kyty.ini; the path accessors; and EnsureWritableDirectory, with unwritable directories
// simulated by injected failures, never by permissions.

#include "launcher/core/dataPaths.h"
#include "support/fakeEnvironment.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::core::EnsureWritableDirectory;
using kyty::core::ResolvedPaths;
using kyty::core::ResolvePaths;
using kyty::core::SetWritableProbeFaultForTesting;
using kyty::core::WritableProbeStep;
using KytyTests::FakeEnvironment;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "DataPathsTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

bool Contains(const std::string& text, const std::string& part) {
	return text.find(part) != std::string::npos;
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

// ---- ResolvePaths

enum class Value { Unset, Empty, Absolute, Relative };

constexpr Value kValues[] = {Value::Unset, Value::Empty, Value::Absolute, Value::Relative};

const char* ValueName(Value value) {
	switch (value) {
		case Value::Unset: return "unset";
		case Value::Empty: return "empty";
		case Value::Absolute: return "absolute";
		case Value::Relative: return "relative";
	}
	return "?";
}

void Apply(FakeEnvironment& env, const char* name, Value value, const char* absolute,
           const char* relative) {
	switch (value) {
		case Value::Unset: env.Unset(name); break;
		case Value::Empty: env.Set(name, ""); break;
		case Value::Absolute: env.Set(name, absolute); break;
		case Value::Relative: env.Set(name, relative); break;
	}
}

const std::string kCwd         = "/run/launcher/cwd";
const std::string kEmulatorDir = "/opt/kyty";

// The expected results, written out as strings: Qt's QDir::homePath() and QStandardPaths rules
// (a relative XDG value is ignored, a relative HOME resolves against the working directory).
std::string ExpectedHome(Value home) {
	switch (home) {
		case Value::Unset:
		case Value::Empty: return "";
		case Value::Absolute: return "/home/player";
		case Value::Relative: return kCwd + "/rel/home";
	}
	return "?";
}

void TestEveryCombination() {
	int count = 0;
	for (const bool flatpak: {false, true}) {
		for (const Value config: kValues) {
			for (const Value data: kValues) {
				for (const Value home: kValues) {
					for (const bool portable: {false, true}) {
						FakeEnvironment env;
						if (flatpak) {
							env.AddExistingPath("/.flatpak-info");
						}
						if (portable) {
							env.AddExistingPath(kCwd + "/Kyty.ini");
						}
						Apply(env, "XDG_CONFIG_HOME", config, "/xdg/config", "rel/config");
						Apply(env, "XDG_DATA_HOME", data, "/xdg/data", "rel/data");
						Apply(env, "HOME", home, "/home/player", "rel/home");

						const std::string home_text   = ExpectedHome(home);
						const std::string config_text = config == Value::Absolute
						                                    ? std::string("/xdg/config")
						                                    : home_text + "/.config";
						const std::string data_text   = data == Value::Absolute
						                                    ? std::string("/xdg/data")
						                                    : home_text + "/.local/share";
						std::string       settings;
						std::string       data_dir;
						if (flatpak) {
							settings = config_text + "/Kyty/Kyty.ini";
							data_dir = data_text + "/Kyty";
						} else {
							settings =
							    portable ? kCwd + "/Kyty.ini" : config_text + "/Kyty/Kyty.ini";
							data_dir = kEmulatorDir;
						}

						const auto paths = ResolvePaths(env, kCwd, kEmulatorDir);
						const auto what =
						    std::string(flatpak ? "flatpak" : "host") + " XDG_CONFIG_HOME " +
						    ValueName(config) + " XDG_DATA_HOME " + ValueName(data) + " HOME " +
						    ValueName(home) + (portable ? " with" : " without") + " ./Kyty.ini: ";
						Check(paths.flatpak == flatpak, what + "flatpak");
						Check(paths.settings_file == fs::path(settings),
						      what + "settings " + paths.settings_file.string() + ", expected " +
						          settings);
						Check(paths.data_dir == fs::path(data_dir),
						      what + "data " + paths.data_dir.string() + ", expected " + data_dir);
						Check(kyty::core::SpawnWorkingDirectory(paths) == paths.data_dir,
						      what + "the spawn working directory is the Data_Directory");
						Check(kyty::core::SettingsDirectory(paths) ==
						          fs::path(settings).parent_path(),
						      what + "SettingsDirectory");
						count++;
					}
				}
			}
		}
	}
	Check(count == 256, "256 combinations");
}

void TestNormalization() {
	FakeEnvironment env;
	env.Set("HOME", "/home/u/../v/./");
	auto paths = ResolvePaths(env, kCwd, "/opt/./kyty/../kyty/");
	Check(paths.settings_file == "/home/v/.config/Kyty/Kyty.ini", "HOME is cleaned as by Qt");
	Check(paths.data_dir == "/opt/kyty", "the emulator directory is normal, without a separator");

	env.Set("XDG_CONFIG_HOME", "/x/../cfg/");
	env.Set("XDG_DATA_HOME", "/x/./data//");
	env.AddExistingPath("/.flatpak-info");
	paths = ResolvePaths(env, kCwd, kEmulatorDir);
	Check(paths.settings_file == "/cfg/Kyty/Kyty.ini", "XDG_CONFIG_HOME is cleaned");
	Check(paths.data_dir == "/x/data/Kyty", "XDG_DATA_HOME is cleaned");

	env.Set("XDG_CONFIG_HOME", "/");
	env.Set("XDG_DATA_HOME", "/");
	paths = ResolvePaths(env, kCwd, kEmulatorDir);
	Check(paths.settings_file == "/Kyty/Kyty.ini", "XDG_CONFIG_HOME=/ as in Qt");
	Check(paths.data_dir == "/Kyty", "XDG_DATA_HOME=/");

	env.Set("HOME", "/");
	env.Unset("XDG_CONFIG_HOME");
	env.Unset("XDG_DATA_HOME");
	paths = ResolvePaths(env, kCwd, kEmulatorDir);
	Check(paths.settings_file == "/.config/Kyty/Kyty.ini", "HOME=/");
	Check(paths.data_dir == "/.local/share/Kyty", "HOME=/ data");
}

void TestWorkingAndEmulatorDirectories() {
	FakeEnvironment env;
	env.Set("HOME", "/home/player");
	auto paths = ResolvePaths(env, kCwd, "bin/../emulator");
	Check(paths.data_dir == kCwd + "/emulator", "a relative emulator directory");

	// Only the working directory's Kyty.ini makes the store portable.
	env.AddExistingPath("/opt/kyty/Kyty.ini");
	env.AddExistingPath("/home/player/Kyty.ini");
	paths = ResolvePaths(env, kCwd, kEmulatorDir);
	Check(paths.settings_file == "/home/player/.config/Kyty/Kyty.ini",
	      "a Kyty.ini next to the emulator or in HOME does not count");

	env.AddExistingPath("/work/Kyty.ini");
	paths = ResolvePaths(env, "work", kEmulatorDir);
	Check(paths.settings_file == "/work/Kyty.ini", "a relative working directory is below /");
	paths = ResolvePaths(env, "/work/", kEmulatorDir);
	Check(paths.settings_file == "/work/Kyty.ini", "a working directory with a separator");
	paths = ResolvePaths(env, "/work/./sub/..", "");
	Check(paths.settings_file == "/work/Kyty.ini", "an unnormalized working directory");
	Check(paths.data_dir == "/work", "an empty emulator directory is the working directory");
}

void TestAccessors() {
	const fs::path data = "/data/Kyty";
	Check(kyty::core::CompatibilityCachePath(data) == "/data/Kyty/compatibility_cache.json",
	      "CompatibilityCachePath");
	Check(kyty::core::CheatDirectory(data) == "/data/Kyty/_Patches", "CheatDirectory");
	Check(kyty::core::TrophyDataDirectory(data) == "/data/Kyty/_Trophies", "TrophyDataDirectory");
	Check(kyty::core::SaveDataDirectory(data) == "/data/Kyty/_SaveData", "SaveDataDirectory");

	ResolvedPaths paths;
	paths.settings_file = "/config/Kyty/Kyty.ini";
	paths.data_dir      = data;
	Check(kyty::core::SettingsDirectory(paths) == "/config/Kyty", "SettingsDirectory");
	Check(kyty::core::SpawnWorkingDirectory(paths) == data, "SpawnWorkingDirectory");
}

// ---- EnsureWritableDirectory

std::vector<std::string> Names(const fs::path& dir) {
	std::vector<std::string> names;
	std::error_code          ec;
	for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
	     it.increment(ec)) {
		names.push_back(it->path().filename().string());
	}
	Check(!ec, "list " + dir.string());
	return names;
}

bool IsProbeName(const std::string& name) {
	const std::string prefix = ".kyty-write-probe-";
	if (name.size() != prefix.size() + 16 || name.compare(0, prefix.size(), prefix) != 0) {
		return false;
	}
	for (size_t i = prefix.size(); i < name.size(); i++) {
		const char c = name[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

void TestWritableSuccess() {
	const TempDir temp("kyty_data_paths");
	const auto    nested = temp.Path() / "a" / "b c" / "Kyty";
	Check(EnsureWritableDirectory(nested).has_value(), "creates missing directories");
	std::error_code ec;
	Check(fs::is_directory(nested, ec), "the directory exists");
	Check(Names(nested).empty(), "no probe is left");

	temp.WriteFile("existing/keep.txt", "keep");
	const auto existing = temp.Path() / "existing";
	Check(EnsureWritableDirectory(existing).has_value(), "an existing directory");
	const auto names = Names(existing);
	Check(names.size() == 1 && names[0] == "keep.txt", "only the existing file remains");

	const auto link = temp.MakeSymlink(existing, "link");
	Check(EnsureWritableDirectory(link).has_value(), "a symbolic link to a directory");
}

void TestWritableFailures() {
	const TempDir temp("kyty_data_paths");
	const auto    file = temp.WriteFile("file.txt", "content");

	auto result = EnsureWritableDirectory(file);
	Check(!result.has_value(), "a regular file in the way");
	Check(result.error().starts_with(file.string() + ": cannot create the directory: "),
	      "the message names the path and the step: " + result.error());
	Check(result.error().size() > (file.string() + ": cannot create the directory: ").size(),
	      "the message has a reason: " + result.error());

	const auto below = file / "sub";
	result           = EnsureWritableDirectory(below);
	Check(!result.has_value() && Contains(result.error(), below.string() + ": cannot create"),
	      "a directory below a regular file: " + (result ? std::string() : result.error()));

	result = EnsureWritableDirectory("");
	Check(!result.has_value(), "the empty path");

	std::error_code ec;
	Check(fs::file_size(file, ec) == 7 && !ec, "the file in the way is unchanged");
}

struct FaultCase {
	WritableProbeStep step;
	int               error_number;
	const char*       step_text;
	bool              probe_left;
};

void TestInjectedFaults() {
	const FaultCase cases[] = {
	    {WritableProbeStep::CreateDirectories, EACCES, "create the directory", false},
	    {WritableProbeStep::Create, EACCES, "create the write probe ", false},
	    {WritableProbeStep::Create, EROFS, "create the write probe ", false},
	    {WritableProbeStep::Write, ENOSPC, "write the write probe ", false},
	    {WritableProbeStep::Close, EIO, "close the write probe ", false},
	    {WritableProbeStep::Remove, EBUSY, "remove the write probe ", true},
	};
	for (const auto& fault: cases) {
		const TempDir                    temp("kyty_data_paths");
		const auto                       dir = temp.Path() / "Kyty";
		std::map<WritableProbeStep, int> calls;
		SetWritableProbeFaultForTesting([&](WritableProbeStep step) {
			calls[step]++;
			return step == fault.step ? fault.error_number : 0;
		});
		const auto result = EnsureWritableDirectory(dir);
		SetWritableProbeFaultForTesting({});
		const std::string what = std::string("fault in '") + fault.step_text + "': ";
		Check(!result.has_value(), what + "fails");
		const std::string prefix = dir.string() + ": cannot " + fault.step_text;
		Check(result.error().starts_with(prefix), what + "message " + result.error());
		Check(Contains(result.error(), ": " + ErrorText(fault.error_number)),
		      what + "the reason " + result.error());
		Check(calls[fault.step] == 1, what + "the step was asked once");
		if (fault.step == WritableProbeStep::CreateDirectories) {
			std::error_code ec;
			Check(!fs::exists(dir, ec), what + "nothing was created");
			continue;
		}
		Check(Contains(result.error(), (dir / ".kyty-write-probe-").string()),
		      what + "the message names the probe file");
		const auto names = Names(dir);
		if (fault.probe_left) {
			Check(names.size() == 1 && IsProbeName(names[0]),
			      what + "the probe that could not be removed is left");
		} else {
			Check(names.empty(), what + "no probe is left");
		}
	}
}

void TestNameCollisions() {
	const TempDir temp("kyty_data_paths");
	int           creates = 0;
	SetWritableProbeFaultForTesting([&](WritableProbeStep step) {
		if (step != WritableProbeStep::Create) {
			return 0;
		}
		creates++;
		return creates <= 2 ? EEXIST : 0;
	});
	auto result = EnsureWritableDirectory(temp.Path());
	Check(result.has_value(), "two collisions, then a free name");
	Check(creates == 3, "three names were tried");
	Check(Names(temp.Path()).empty(), "no probe is left after the collisions");

	creates = 0;
	SetWritableProbeFaultForTesting([&](WritableProbeStep step) {
		if (step == WritableProbeStep::Create) {
			creates++;
			return EEXIST;
		}
		return 0;
	});
	result = EnsureWritableDirectory(temp.Path());
	SetWritableProbeFaultForTesting({});
	Check(!result.has_value() && Contains(result.error(), ": " + ErrorText(EEXIST)),
	      "every name taken: " + (result ? std::string() : result.error()));
	Check(creates == 100, "100 names were tried, not " + std::to_string(creates));

	Check(EnsureWritableDirectory(temp.Path()).has_value(), "the hook was removed");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("DataPathsTests");
	TestEveryCombination();
	TestNormalization();
	TestWorkingAndEmulatorDirectories();
	TestAccessors();
	TestWritableSuccess();
	TestWritableFailures();
	TestInjectedFaults();
	TestNameCollisions();
	std::printf("DataPathsTests: all tests passed\n");
	return 0;
}
