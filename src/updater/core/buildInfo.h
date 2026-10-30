#pragma once

// The classification of the running build (tasks 17.1 and 17.4; Requirements 13.2, 13.3, 13.10):
// the compile-time flags that cmake/KytyBuildInfo.cmake and the KYTY_FLATPAK_BUILD option set, and
// the update UI mode they select.

#include "updater/core/updatePolicy.h"

namespace kyty::updater {

// KYTY_FLATPAK, KYTY_OFFICIAL_BUILD, and KYTY_RELEASE_TAG (cmake_config.h), and KYTY_GIT_HASH
// (kytyGitVersion.h) of this build. The strings have static storage duration.
BuildFlags CurrentBuildFlags();

// SelectUpdateUiMode(CurrentBuildFlags()).
UpdateUiMode CurrentUpdateUiMode();

} // namespace kyty::updater
