# Launcher_Core (design.md, "Layering and dependency rules"): the toolkit-free launcher logic that the
# GTK launcher and the tests share. It links no GTK, libadwaita, GLib, GIO, or libsoup library, it is
# part of the all target whatever KYTY_BUILD_LAUNCHER is, and the coverage walker in
# cmake/KytyCoverage.cmake instruments it like every other first-party target.
file(GLOB kyty_launcher_core_sources CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.h")

# The interfaces that the Updater shares with Launcher_Core: HttpClient, Scheduler, and Expected.
add_library(kyty_net_interfaces INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/expected.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/httpClient.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/scheduler.h")
target_include_directories(kyty_net_interfaces INTERFACE "${KYTY_SOURCE_DIR}")

add_library(kyty_launcher_core STATIC ${kyty_launcher_core_sources})
target_link_libraries(kyty_launcher_core
	PUBLIC common kyty_net_interfaces
	PRIVATE SDL3::SDL3-static nlohmann_json::nlohmann_json LibAtrac9 ZArchive::zarchive)
# The property test executables compile with -frtti. Classes they derive from or whose typeinfo
# they need must come from objects compiled with RTTI as well.
target_compile_options(kyty_launcher_core PRIVATE -frtti)
