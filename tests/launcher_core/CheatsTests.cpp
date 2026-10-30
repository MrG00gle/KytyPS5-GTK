// Example tests of the cheat file rules (src/launcher/core/cheats.h, task 16.2): which title IDs
// have cheats, and the path of their cheat file below the Data_Directory.

#include "launcher/core/cheats.h"
#include "launcher/core/dataPaths.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

using kyty::core::CheatFilePath;
using kyty::core::IsCheatTitleId;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "CheatsTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void TestCheatTitleIds() {
	for (const std::string_view title:
	     {"PPSA01234", "PPSA00000", "PPSA99999", "ppsa01234", "pPsA01234", "  PPSA01234\t",
	      "\nppsa01234 ", "\xE3\x80\x80PPSA01234\xC2\xA0"}) {
		Check(IsCheatTitleId(title), "'" + std::string(title) + "' has cheats");
	}
	for (const std::string_view title:
	     {"", "   ", "PPSA", "PPSA0123", "PPSA012345", "CUSA01234", "PPSB01234", "PPS01234",
	      "PPSA0123a", "PPSA01 234", "PPSA-1234", "PPSA+1234", "PPSA01234_00", "XPPSA01234",
	      // A fullwidth digit and a fullwidth letter are not ASCII.
	      "PPSA0123\xEF\xBC\x94", "\xEF\xBC\xB0PSA01234"}) {
		Check(!IsCheatTitleId(title), "'" + std::string(title) + "' has no cheats");
	}
	Check(!IsCheatTitleId(std::string_view("PPSA0123\0", 9)), "an embedded NUL is not a digit");
}

void TestCheatFilePath() {
	const std::filesystem::path data_dir = "/opt/kyty";
	const auto                  path     = CheatFilePath(data_dir, "  ppsa01234 ");
	Check(path.native() == "/opt/kyty/_Patches/PPSA01234.json",
	      "the cheat file is <data_dir>/_Patches/<TITLE_ID>.json: " + path.string());
	Check(path == kyty::core::CheatDirectory(data_dir) / "PPSA01234.json",
	      "the cheat file is in CheatDirectory()");
	Check(CheatFilePath("/opt/kyty/", "PPSA54321").native() == "/opt/kyty/_Patches/PPSA54321.json",
	      "a trailing separator on the Data_Directory");
	Check(CheatFilePath("data", "PPSA54321").native() == "data/_Patches/PPSA54321.json",
	      "a relative Data_Directory stays relative");
	for (const std::string_view title:
	     {"", "CUSA01234", "PPSA0123", "../PPSA01234", "PPSA01234/"}) {
		Check(CheatFilePath(data_dir, title).empty(),
		      "no cheat file for '" + std::string(title) + "'");
	}
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("CheatsTests");
	TestCheatTitleIds();
	TestCheatFilePath();
	std::printf("CheatsTests: all tests passed\n");
	return 0;
}
