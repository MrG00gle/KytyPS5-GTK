#include "common/common.h"
#include "common/threads.h"
#include "emulator.h"
#include "emulatorOptions.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <vector>

using namespace Common;
using namespace Emulator;

// The build date as YYYY.MM.DD, from __DATE__ ("Mmm dd yyyy", the day padded with a space).
static std::string GetBuildDate() {
	constexpr std::string_view months = "JanFebMarAprMayJunJulAugSepOctNovDec";
	constexpr std::string_view date   = __DATE__;

	const auto month = months.find(date.substr(0, 3)) / 3 + 1;
	const char tens  = date[4] == ' ' ? '0' : date[4];
	return fmt::format("{}.{:02}.{}{}", date.substr(7), month, tens, date[5]);
}

// "Release, clang-lld, ver = …, git = …, date = …", the first line of the help and error output.
static std::string GetBuildString() {
	const char* type = KYTY_BUILD == KYTY_BUILD_DEBUG ? "Debug" : "Release";

	return fmt::format("{}, {}, ver = {}, git = {}, date = {}", type, KYTY_TOOLCHAIN, KYTY_VERSION,
	                   KYTY_GIT_VERSION, GetBuildDate());
}

static int Main(int argc, char* argv[]) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) {
		args.emplace_back(argv[i]);
	}

	// Parse before any initialization, so help and errors start nothing.
	ParseOutcome outcome = ParseOptions(args);

	switch (outcome.status) {
		case ParseOutcome::Help:
			::printf("%s\n%s", GetBuildString().c_str(), UsageText().c_str());
			return 0;
		case ParseOutcome::Error:
			::printf("%s\n", outcome.error.c_str());
			::printf("%s\n%s", GetBuildString().c_str(), UsageText().c_str());
			return 1;
		case ParseOutcome::Run: break;
	}

	// Inside the Flatpak_Sandbox the Data_Directory is $XDG_DATA_HOME/Kyty (Requirement 12.2).
	if (const auto error = ApplyDataDirectory(outcome.options, CurrentDataDirectoryEnvironment());
	    !error.empty()) {
		::printf("%s\n", error.c_str());
		return 1;
	}

	InitializeThreads();

	Run(outcome.options);

	return 0;
}


int main(int argc, char* argv[]) {
    return Main(argc, argv);
}
