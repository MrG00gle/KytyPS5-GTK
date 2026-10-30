# kyty_updater_core (task 17.1): release metadata, update policy, SHA-256, download verification, the
# update result file, and process running for the Updater. It is part of the all target whatever
# KYTY_BUILD_LAUNCHER is and links only kyty_net_interfaces (header only) plus the header-only JSON
# library, so the kyty_updater helper and the updater tests link no application library.
file(GLOB kyty_updater_core_sources CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_library(kyty_updater_core STATIC ${kyty_updater_core_sources})
target_link_libraries(kyty_updater_core
	PUBLIC kyty_net_interfaces
	PRIVATE nlohmann_json::nlohmann_json)
# The property test executables compile with -frtti and derive from ProcessRunner.
target_compile_options(kyty_updater_core PRIVATE -frtti)
# buildInfo.cpp includes kytyGitVersion.h, which KytyGitVersion generates (build order only).
add_dependencies(kyty_updater_core KytyGitVersion)
