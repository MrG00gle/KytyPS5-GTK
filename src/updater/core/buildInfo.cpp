#include "updater/core/buildInfo.h"

// Generated in the build directory: kytyGitVersion.h by the KytyGitVersion target, and the
// cmake_config.h it includes at configure time.
#include "kytyGitVersion.h"

namespace kyty::updater {

BuildFlags CurrentBuildFlags() {
	BuildFlags flags;
#if defined(KYTY_FLATPAK)
	flags.flatpak = true;
#endif
#if defined(KYTY_OFFICIAL_BUILD)
	flags.official = true;
#endif
	flags.release_tag = KYTY_RELEASE_TAG;
	flags.git_hash    = KYTY_GIT_HASH;
	return flags;
}

UpdateUiMode CurrentUpdateUiMode() {
	return SelectUpdateUiMode(CurrentBuildFlags());
}

} // namespace kyty::updater
