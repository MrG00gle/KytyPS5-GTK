#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "emulatorOptions.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <string>
#include <vector>
#include <fmt/format.h>

using namespace Common;
using namespace Emulator;

static std::string GetBuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif

	std::string compiler = Debug::GetCompiler() + "-" + Debug::GetLinker();

	std::string str =
	    fmt::format("{}, {}, ver = {}, git = {}, date = {}", type.c_str(), compiler.c_str(),
	                KYTY_VERSION, KYTY_GIT_VERSION, date.ToString().c_str());

	return str;
}

static int Main(int argc, char* argv[]) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) {
		args.emplace_back(argv[i]);
	}

	// Parse before any initialization, so help and errors start nothing.
	const ParseOutcome outcome = ParseOptions(args);

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

	VirtualMemory::Init();
	InitializeThreads();

	Run(outcome.options);

	return 0;
}


int main(int argc, char* argv[]) {
    return Main(argc, argv);
}

