// Example tests of kyty::updater::CurrentBuildFlags and CurrentUpdateUiMode
// (src/updater/core/buildInfo.h, tasks 17.1 and 17.4): the flags are the ones this build was
// configured with (cmake_config.h and the KYTY_FLATPAK definition, which this test sees too), and
// the mode is SelectUpdateUiMode of them. A local build, such as the developer trees, is neither
// official nor a Flatpak build, so its mode is Other.

#include "cmake_config.h"
#include "updater/core/buildInfo.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

using kyty::updater::UpdateUiMode;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "BuildInfoTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

const char* ModeName(UpdateUiMode mode) {
	switch (mode) {
		case UpdateUiMode::Official: return "Official";
		case UpdateUiMode::Flatpak: return "Flatpak";
		case UpdateUiMode::Other: return "Other";
	}
	return "?";
}

void TestFlagsMatchTheConfiguration() {
	const auto flags = kyty::updater::CurrentBuildFlags();
#if defined(KYTY_FLATPAK)
	constexpr bool kFlatpak = true;
#else
	constexpr bool kFlatpak = false;
#endif
#if defined(KYTY_OFFICIAL_BUILD)
	constexpr bool kOfficial = true;
#else
	constexpr bool kOfficial = false;
#endif
	Check(flags.flatpak == kFlatpak, "flatpak follows KYTY_FLATPAK");
	Check(flags.official == kOfficial, "official follows KYTY_OFFICIAL_BUILD");
	Check(flags.release_tag == std::string_view(KYTY_RELEASE_TAG),
	      "release_tag is KYTY_RELEASE_TAG");
	Check(!flags.git_hash.empty(), "git_hash is set ('unknown' without git)");
	Check(kyty::updater::CurrentUpdateUiMode() == kyty::updater::SelectUpdateUiMode(flags),
	      "the mode is SelectUpdateUiMode of the flags");
	std::printf(
	    "BuildInfoTests: flatpak=%d official=%d release_tag='%.*s' git_hash='%.*s' mode=%s\n",
	    flags.flatpak ? 1 : 0, flags.official ? 1 : 0, static_cast<int>(flags.release_tag.size()),
	    flags.release_tag.data(), static_cast<int>(flags.git_hash.size()), flags.git_hash.data(),
	    ModeName(kyty::updater::CurrentUpdateUiMode()));

	// A local build (the developer trees, CI pull requests): not official, not Flatpak, mode Other.
	if (!kFlatpak && !kOfficial) {
		Check(kyty::updater::CurrentUpdateUiMode() == UpdateUiMode::Other,
		      "a local build is Other");
	}
	if (kFlatpak) {
		Check(kyty::updater::CurrentUpdateUiMode() == UpdateUiMode::Flatpak, "a Flatpak build");
	}
}

} // namespace

int main() {
	TestFlagsMatchTheConfiguration();
	std::printf("BuildInfoTests: all tests passed\n");
	return 0;
}
