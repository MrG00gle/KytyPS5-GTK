# install_layout_tree (task 26.1; Requirements 13.4, 14.6, 14.7, 14.14): "cmake --install" of this
# build tree into WORK_DIR, then the first-party files of the configured KYTY_INSTALL_LAYOUT,
# exactly:
#   portable  kyty_emulator, assets/sounds/trophy-unlock.wav, assets/cursors/<every file of
#             assets/cursors>, and with the launcher kyty_launcher and kyty_updater;
#   fhs       bin/kyty_emulator, share/kyty/assets/sounds/trophy-unlock.wav,
#             share/kyty/assets/cursors/<...>, and with the launcher bin/kyty_launcher,
#             bin/kyty_updater, share/applications/<App_ID>.desktop,
#             share/metainfo/<App_ID>.metainfo.xml, share/icons/hicolor/scalable/apps/<App_ID>.svg,
#             and share/icons/hicolor/256x256/apps/<App_ID>.png.
# A Flatpak build (FLATPAK=ON, fhs only) installs no kyty_updater (Requirement 13.4). Each data file
# equals its source byte for byte, and each executable is executable. A missing or an extra
# first-party file fails the test. The files that the dependency and license rules install under
# the top-level include/, lib/, lib64/, and licenses/ directories are listed, not checked.
# Run as
#   cmake -DBUILD_DIR=<build tree> -DSOURCE_DIR=<source tree> [-DCONFIG=<configuration>]
#         -DLAYOUT=<portable|fhs> -DFLATPAK=<ON|OFF> -DLAUNCHER=<ON|OFF> -DAPP_ID=<App_ID>
#         -DWORK_DIR=<scratch directory> -P InstallLayoutTest.cmake
# WORK_DIR is the install prefix. It is recreated at the start and removed when every check passes.
cmake_minimum_required(VERSION 3.22)

foreach(kyty_variable IN ITEMS BUILD_DIR SOURCE_DIR LAYOUT FLATPAK LAUNCHER APP_ID WORK_DIR)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "InstallLayoutTest: ${kyty_variable} is not set")
	endif()
endforeach()
if(NOT EXISTS "${BUILD_DIR}/cmake_install.cmake")
	message(FATAL_ERROR "InstallLayoutTest: ${BUILD_DIR} is not a configured build tree")
endif()
if(LAYOUT STREQUAL "fhs")
	set(kyty_bin "bin/")
	set(kyty_assets "share/kyty/assets")
elseif(LAYOUT STREQUAL "portable")
	if(FLATPAK)
		message(FATAL_ERROR "InstallLayoutTest: a Flatpak build cannot use the portable layout")
	endif()
	set(kyty_bin "")
	set(kyty_assets "assets")
else()
	message(FATAL_ERROR "InstallLayoutTest: LAYOUT must be portable or fhs (got '${LAYOUT}')")
endif()

# The expected first-party files: "<installed path>|<source file>", or "<installed path>|" for an
# executable.
set(kyty_expected
	"${kyty_bin}kyty_emulator|"
	"${kyty_assets}/sounds/trophy-unlock.wav|${SOURCE_DIR}/assets/sounds/trophy-unlock.wav")
file(GLOB_RECURSE kyty_cursors LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}/assets/cursors"
	"${SOURCE_DIR}/assets/cursors/*")
if(NOT kyty_cursors)
	message(FATAL_ERROR "InstallLayoutTest: ${SOURCE_DIR}/assets/cursors holds no file")
endif()
foreach(kyty_cursor IN LISTS kyty_cursors)
	list(APPEND kyty_expected "${kyty_assets}/cursors/${kyty_cursor}|${SOURCE_DIR}/assets/cursors/${kyty_cursor}")
endforeach()
if(LAUNCHER)
	list(APPEND kyty_expected "${kyty_bin}kyty_launcher|")
	if(NOT FLATPAK)
		list(APPEND kyty_expected "${kyty_bin}kyty_updater|")
	endif()
	if(LAYOUT STREQUAL "fhs")
		set(kyty_packaging "${SOURCE_DIR}/packaging/linux")
		list(APPEND kyty_expected
			"share/applications/${APP_ID}.desktop|${kyty_packaging}/${APP_ID}.desktop"
			"share/metainfo/${APP_ID}.metainfo.xml|${kyty_packaging}/${APP_ID}.metainfo.xml"
			"share/icons/hicolor/scalable/apps/${APP_ID}.svg|${kyty_packaging}/icons/${APP_ID}.svg"
			"share/icons/hicolor/256x256/apps/${APP_ID}.png|${kyty_packaging}/icons/${APP_ID}.png")
	endif()
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
# A DESTDIR from the caller's environment would move every file out of WORK_DIR.
unset(ENV{DESTDIR})
set(kyty_install_command "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${WORK_DIR}")
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
	list(APPEND kyty_install_command --config "${CONFIG}")
endif()
list(JOIN kyty_install_command " " kyty_install_text)
message(STATUS "InstallLayoutTest: ${LAYOUT} (Flatpak ${FLATPAK}, launcher ${LAUNCHER}): "
	"${kyty_install_text}")
execute_process(COMMAND ${kyty_install_command}
	RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_output)
if(NOT kyty_result STREQUAL "0")
	message(FATAL_ERROR "InstallLayoutTest: the install exited with '${kyty_result}' (build the all "
		"target first; the tree stays in ${WORK_DIR}):\n${kyty_output}")
endif()

file(GLOB_RECURSE kyty_installed LIST_DIRECTORIES false RELATIVE "${WORK_DIR}" "${WORK_DIR}/*")
list(SORT kyty_installed)
set(kyty_third_party "")
set(kyty_first_party "")
foreach(kyty_path IN LISTS kyty_installed)
	if(kyty_path MATCHES "^(include|lib|lib64|licenses)/")
		list(APPEND kyty_third_party "${kyty_path}")
	else()
		list(APPEND kyty_first_party "${kyty_path}")
	endif()
endforeach()

set(kyty_failures "")
set(kyty_expected_paths "")
foreach(kyty_entry IN LISTS kyty_expected)
	string(FIND "${kyty_entry}" "|" kyty_separator)
	string(SUBSTRING "${kyty_entry}" 0 ${kyty_separator} kyty_path)
	math(EXPR kyty_source_begin "${kyty_separator} + 1")
	string(SUBSTRING "${kyty_entry}" ${kyty_source_begin} -1 kyty_source)
	list(APPEND kyty_expected_paths "${kyty_path}")
	set(kyty_file "${WORK_DIR}/${kyty_path}")
	if(NOT EXISTS "${kyty_file}")
		string(APPEND kyty_failures "\n  missing: ${kyty_path}")
	elseif(kyty_source STREQUAL "")
		# if(IS_EXECUTABLE) needs CMake 3.29.
		execute_process(COMMAND test -x "${kyty_file}" RESULT_VARIABLE kyty_executable)
		if(NOT kyty_executable STREQUAL "0")
			string(APPEND kyty_failures "\n  not executable: ${kyty_path}")
		endif()
	else()
		file(SHA256 "${kyty_file}" kyty_installed_hash)
		file(SHA256 "${kyty_source}" kyty_source_hash)
		if(NOT kyty_installed_hash STREQUAL kyty_source_hash)
			string(APPEND kyty_failures "\n  ${kyty_path} differs from ${kyty_source}")
		endif()
	endif()
endforeach()
foreach(kyty_path IN LISTS kyty_first_party)
	if(NOT kyty_path IN_LIST kyty_expected_paths)
		string(APPEND kyty_failures "\n  unexpected: ${kyty_path}")
	endif()
endforeach()

list(LENGTH kyty_first_party kyty_first_party_count)
list(LENGTH kyty_third_party kyty_third_party_count)
list(JOIN kyty_first_party "\n  " kyty_first_party_text)
list(JOIN kyty_third_party "\n  " kyty_third_party_text)
message(STATUS "InstallLayoutTest: ${kyty_first_party_count} first-party file(s):\n  "
	"${kyty_first_party_text}")
message(STATUS "InstallLayoutTest: ${kyty_third_party_count} file(s) of the dependency and license "
	"rules (listed, not checked):\n  ${kyty_third_party_text}")
if(kyty_failures)
	message(FATAL_ERROR "InstallLayoutTest: FAILED (the tree stays in ${WORK_DIR}):"
		"${kyty_failures}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
list(LENGTH kyty_expected_paths kyty_expected_count)
message(STATUS "InstallLayoutTest: PASS (${LAYOUT}: the ${kyty_expected_count} first-party files, "
	"no other)")
