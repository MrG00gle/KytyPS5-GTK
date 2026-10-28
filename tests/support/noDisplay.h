#pragma once

// Launcher_Core tests run without a display (Requirement 6.7): CTest unsets WAYLAND_DISPLAY and
// DISPLAY for them (ENVIRONMENT_MODIFICATION in tests/CMakeLists.txt). Each test main() calls
// CheckNoDisplay() first, so a test that CTest would run with a display fails instead of passing.

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace KytyTests {

inline void CheckNoDisplay(const char* test) {
	for (const char* name: {"WAYLAND_DISPLAY", "DISPLAY"}) {
		if (std::getenv(name) != nullptr) {
			std::fprintf(stderr,
			             "%s: failed: %s is set; Launcher_Core tests must run without a display "
			             "(Requirement 6.7)\n",
			             test, name);
			std::abort();
		}
	}
}

} // namespace KytyTests
