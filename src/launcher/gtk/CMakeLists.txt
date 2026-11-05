# The GTK launcher (task 19.1; design.md, "GTK presentation components"), added by the top-level
# CMakeLists.txt only when KYTY_BUILD_LAUNCHER is ON:
# - kyty_launcher_gtk: every source here and in services/ except main.cpp, plus the compiled
#   GResource bundle. The GTK API floor is PUBLIC, so every consumer (kyty_launcher and the test
#   executables) compiles against the same floor.
# - kyty_launcher: the launcher executable (Requirement 6.4), written to
#   ${CMAKE_BINARY_DIR}/launcher with the staged kyty_updater next to it, and installed next to
#   kyty_emulator and kyty_updater.
# - update-pot (not part of all): extracts the strings of the files in po/POTFILES.in into
#   ${CMAKE_BINARY_DIR}/po/kyty-launcher.pot.
include(KytyGtk)
kyty_require_gtk_modules()

find_program(KYTY_GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

# ---- GResource bundle (prefix /io/github/MrG00gle/KytyPS5_GTK). The hidden trophy image is read
# from assets/images through the second source directory.
set(kyty_gtk_resource_xml "${CMAKE_CURRENT_SOURCE_DIR}/resources/kyty-launcher.gresource.xml")
set(kyty_gtk_resource_dirs
	--sourcedir "${CMAKE_CURRENT_SOURCE_DIR}/resources"
	--sourcedir "${CMAKE_SOURCE_DIR}/assets")
set(kyty_gtk_resource_source "${CMAKE_CURRENT_BINARY_DIR}/kyty-launcher-resources.c")
execute_process(
	COMMAND "${KYTY_GLIB_COMPILE_RESOURCES}" --generate-dependencies ${kyty_gtk_resource_dirs}
	        "${kyty_gtk_resource_xml}"
	OUTPUT_VARIABLE kyty_gtk_resource_files
	RESULT_VARIABLE kyty_gtk_resource_result
	ERROR_VARIABLE kyty_gtk_resource_error)
if(NOT kyty_gtk_resource_result EQUAL 0)
	message(FATAL_ERROR "glib-compile-resources --generate-dependencies failed for "
		"${kyty_gtk_resource_xml}: ${kyty_gtk_resource_error}")
endif()
string(REPLACE "\n" ";" kyty_gtk_resource_files "${kyty_gtk_resource_files}")
list(FILTER kyty_gtk_resource_files EXCLUDE REGEX "^$")
# A new file in the bundle is a change of the XML file, which configures again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kyty_gtk_resource_xml}")
# --manual-register: main.cpp calls kyty_launcher_register_resource(), so the linker never drops the
# bundle from the static library.
add_custom_command(
	OUTPUT "${kyty_gtk_resource_source}"
	COMMAND "${KYTY_GLIB_COMPILE_RESOURCES}" ${kyty_gtk_resource_dirs} --c-name kyty_launcher
	        --manual-register --generate-source --target "${kyty_gtk_resource_source}"
	        "${kyty_gtk_resource_xml}"
	DEPENDS "${kyty_gtk_resource_xml}" ${kyty_gtk_resource_files}
	VERBATIM)

# ---- kyty_launcher_gtk
file(GLOB kyty_launcher_gtk_sources CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/services/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/services/*.h")
list(REMOVE_ITEM kyty_launcher_gtk_sources "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
add_library(kyty_launcher_gtk STATIC ${kyty_launcher_gtk_sources} "${kyty_gtk_resource_source}")
target_link_libraries(kyty_launcher_gtk PUBLIC
	kyty_launcher_core kyty_updater_core
	PkgConfig::KYTY_GTK4 PkgConfig::KYTY_ADW PkgConfig::KYTY_SOUP
	SDL3::SDL3-static Vulkan::Headers)
# Sources include "launcher/gtk/<name>.h".
target_include_directories(kyty_launcher_gtk PUBLIC "${KYTY_SOURCE_DIR}")
# The GTK API floor (GTK 4.14, libadwaita 1.5, GLib 2.80, libsoup 3.4): with
# -Werror=deprecated-declarations, calling a function newer than the floor ("Not available before
# ...") or one deprecated at or below it is an error.
target_compile_definitions(kyty_launcher_gtk PUBLIC
	GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14
	GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14
	ADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5
	ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5
	GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_80
	GLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_80
	SOUP_VERSION_MIN_REQUIRED=SOUP_VERSION_3_4
	SOUP_VERSION_MAX_ALLOWED=SOUP_VERSION_3_4)
target_compile_options(kyty_launcher_gtk PUBLIC -Werror=deprecated-declarations)
target_compile_definitions(kyty_launcher_gtk PRIVATE "G_LOG_DOMAIN=\"kyty-launcher\"")
# As in Launcher_Core: the test executables compile with -frtti, so the classes they use come from
# objects with RTTI as well.
target_compile_options(kyty_launcher_gtk PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-frtti>)

# ---- kyty_launcher
add_executable(kyty_launcher main.cpp)
target_link_libraries(kyty_launcher PRIVATE kyty_launcher_gtk)
target_compile_definitions(kyty_launcher PRIVATE "G_LOG_DOMAIN=\"kyty-launcher\"")
kyty_link_with_lld(kyty_launcher)
# Release builds ship the executable stripped, as kyty_emulator.
target_link_options(kyty_launcher PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
set_target_properties(kyty_launcher PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launcher")
add_dependencies(kyty_launcher kyty_emulator)
# src/updater/CMakeLists.txt skips kyty_updater in Flatpak builds.
if(TARGET kyty_updater)
	add_custom_target(stage_updater
		COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:kyty_launcher>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:kyty_updater>
			$<TARGET_FILE_DIR:kyty_launcher>/$<TARGET_FILE_NAME:kyty_updater>
		DEPENDS kyty_updater
		VERBATIM)
	add_dependencies(kyty_launcher stage_updater)
endif()
install(TARGETS kyty_launcher RUNTIME DESTINATION .)

# ---- update-pot
set(kyty_gtk_potfiles "${CMAKE_SOURCE_DIR}/po/POTFILES.in")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kyty_gtk_potfiles}")
file(STRINGS "${kyty_gtk_potfiles}" kyty_gtk_potfile_lines)
set(kyty_gtk_pot_sources "")
set(kyty_gtk_pot_ui_files "")
foreach(kyty_gtk_line IN LISTS kyty_gtk_potfile_lines)
	string(STRIP "${kyty_gtk_line}" kyty_gtk_line)
	if(kyty_gtk_line STREQUAL "" OR kyty_gtk_line MATCHES "^#")
		continue()
	endif()
	if(kyty_gtk_line MATCHES "\\.xml$")
		list(APPEND kyty_gtk_pot_ui_files "${kyty_gtk_line}")
	else()
		list(APPEND kyty_gtk_pot_sources "${kyty_gtk_line}")
	endif()
endforeach()
find_program(KYTY_XGETTEXT xgettext)
if(KYTY_XGETTEXT)
	set(kyty_gtk_pot "${CMAKE_BINARY_DIR}/po/kyty-launcher.pot")
	set(kyty_gtk_xgettext_options --from-code=UTF-8 --force-po --package-name=kyty-launcher
		--add-comments=TRANSLATORS: --output "${kyty_gtk_pot}")
	set(kyty_gtk_pot_commands "")
	if(kyty_gtk_pot_sources)
		list(APPEND kyty_gtk_pot_commands
			COMMAND "${KYTY_XGETTEXT}" ${kyty_gtk_xgettext_options} --language=C++ --keyword=_
			        --keyword=N_ --keyword=C_:1c,2 --keyword=NC_:1c,2 --keyword=NGettext:1,2
			        --keyword=NGettextNoop:1,2
			        ${kyty_gtk_pot_sources})
	endif()
	if(kyty_gtk_pot_ui_files)
		if(kyty_gtk_pot_sources)
			set(kyty_gtk_join --join-existing)
		else()
			set(kyty_gtk_join "")
		endif()
		list(APPEND kyty_gtk_pot_commands
			COMMAND "${KYTY_XGETTEXT}" ${kyty_gtk_xgettext_options} ${kyty_gtk_join} --language=Glade
			        ${kyty_gtk_pot_ui_files})
	endif()
	add_custom_target(update-pot
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/po"
		${kyty_gtk_pot_commands}
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "Writing ${kyty_gtk_pot}"
		VERBATIM)
else()
	message(STATUS "KytyGtk: xgettext not found; no update-pot target")
endif()
