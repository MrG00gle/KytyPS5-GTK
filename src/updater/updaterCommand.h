#pragma once

// The kyty_updater command line (task 17.3; design.md "Updater flow"), moved out of main.cpp so
// that tests can run it in a forked process:
//
//   kyty_updater --validate SOURCE TARGET LAUNCHER_REL
//   kyty_updater --install SOURCE TARGET LAUNCHER_REL PARENT_PID WORKDIR
//
// --validate checks the extracted release in SOURCE against the installation TARGET. --install
// waits up to two minutes for the launcher PARENT_PID to exit, installs the release (rolling back
// on failure), writes TARGET/.kyty-update-result, and starts TARGET/LAUNCHER_REL again in WORKDIR;
// after a successful installation it removes the update workspace that holds SOURCE.

#include <filesystem>
#include <vector>

namespace Kyty::Updater {

// arguments[0] is the program name. Returns the exit status: 0 on success, 1 on failure, 2 for a
// usage error (printed to standard error). Throws std::exception for an invalid PARENT_PID; main()
// prints what() and exits 1.
int RunUpdaterCommand(const std::vector<std::filesystem::path>& arguments);

} // namespace Kyty::Updater
