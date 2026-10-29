#pragma once

// The process test helpers of tests/helpers/ (task 15.6): argv_echo and fake_emulator are test
// executables written to the build directory, which tests/CMakeLists.txt passes as
// KYTY_TEST_BINARY_DIR (a PUBLIC definition of kyty_test_support, which also depends on both
// helpers, so they are built before any test that links it).

#include <filesystem>
#include <string_view>

#ifndef KYTY_TEST_BINARY_DIR
#error "KYTY_TEST_BINARY_DIR must be defined (kyty_test_support sets it)"
#endif

namespace KytyTests {

// HelperPath("argv_echo") or HelperPath("fake_emulator"): the absolute path of the helper.
inline std::filesystem::path HelperPath(std::string_view name) {
	return std::filesystem::path(KYTY_TEST_BINARY_DIR) / name;
}

} // namespace KytyTests
