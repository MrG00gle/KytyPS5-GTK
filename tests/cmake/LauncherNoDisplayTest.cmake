# launcher_no_display (task 22.6; Requirement 6.15; phase 7 plan D38): kyty_launcher started with no
# reachable display server exits with 1 within 5 seconds, prints "kyty_launcher: cannot open
# display: <reason>" on stderr, and leaves the Settings_Store unchanged. Three runs, each through
# "cmake -E env" with WAYLAND_DISPLAY, DISPLAY, DBUS_SESSION_BUS_ADDRESS, and GDK_BACKEND unset, a
# private empty 0700 XDG_RUNTIME_DIR (no desktop's wayland-0 socket), and XDG_CONFIG_HOME holding a
# prepared Kyty/Kyty.ini: both variables unset, an empty WAYLAND_DISPLAY, and an unreachable
# WAYLAND_DISPLAY and DISPLAY. Afterwards Kyty.ini has the same SHA-256, the private tree lists the
# same files, and the directory of the launcher gained no Kyty.ini (or kept its bytes). Run as
#   cmake -DLAUNCHER=<kyty_launcher> -DWORK_DIR=<scratch directory> -P LauncherNoDisplayTest.cmake
# WORK_DIR is recreated at the start and removed when every check passes.
cmake_minimum_required(VERSION 3.22)

foreach(kyty_variable IN ITEMS LAUNCHER WORK_DIR)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "LauncherNoDisplayTest: ${kyty_variable} is not set")
	endif()
endforeach()
if(NOT EXISTS "${LAUNCHER}")
	message(FATAL_ERROR "LauncherNoDisplayTest: ${LAUNCHER} does not exist")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
foreach(kyty_directory IN ITEMS runtime config/Kyty data cache home cwd)
	file(MAKE_DIRECTORY "${WORK_DIR}/${kyty_directory}")
endforeach()
file(CHMOD "${WORK_DIR}/runtime" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(kyty_settings "${WORK_DIR}/config/Kyty/Kyty.ini")
file(WRITE "${kyty_settings}"
	"[Launcher]\ngame_dirs=/games\nview_mode=grid\n\n[LauncherWindow]\nwidth=1200\nheight=800\n")

# Every file and directory of the private tree, sorted (the runs' output goes to variables).
function(kyty_tree_listing out)
	file(GLOB_RECURSE kyty_entries LIST_DIRECTORIES true RELATIVE "${WORK_DIR}" "${WORK_DIR}/*")
	list(SORT kyty_entries)
	set(${out} "${kyty_entries}" PARENT_SCOPE)
endfunction()

# A portable Kyty.ini next to the launcher would be its Settings_Store: it must not appear, and one
# that exists must keep its bytes.
get_filename_component(kyty_launcher_directory "${LAUNCHER}" DIRECTORY)
set(kyty_portable "${kyty_launcher_directory}/Kyty.ini")
function(kyty_portable_state out)
	if(EXISTS "${kyty_portable}")
		file(SHA256 "${kyty_portable}" kyty_hash)
		set(${out} "present ${kyty_hash}" PARENT_SCOPE)
	else()
		set(${out} "absent" PARENT_SCOPE)
	endif()
endfunction()

# Microseconds since the epoch ("%f" needs CMake 3.23; whole seconds before).
function(kyty_now_us out)
	if(CMAKE_VERSION VERSION_LESS 3.23)
		string(TIMESTAMP kyty_seconds "%s" UTC)
		set(${out} "${kyty_seconds}000000" PARENT_SCOPE)
	else()
		string(TIMESTAMP kyty_now "%s%f" UTC)
		set(${out} "${kyty_now}" PARENT_SCOPE)
	endif()
endfunction()

file(SHA256 "${kyty_settings}" kyty_hash_before)
kyty_tree_listing(kyty_listing_before)
kyty_portable_state(kyty_portable_before)
message(STATUS "LauncherNoDisplayTest: Kyty.ini sha256 before ${kyty_hash_before}")
message(STATUS "LauncherNoDisplayTest: Kyty.ini next to the launcher before: "
	"${kyty_portable_before}")

set(kyty_failures "")
set(kyty_common_environment
	--unset=WAYLAND_DISPLAY --unset=DISPLAY --unset=DBUS_SESSION_BUS_ADDRESS --unset=GDK_BACKEND
	--unset=LANGUAGE "LC_ALL=C.UTF-8" "LANG=C.UTF-8" "G_DEBUG=fatal-criticals"
	"XDG_RUNTIME_DIR=${WORK_DIR}/runtime" "XDG_CONFIG_HOME=${WORK_DIR}/config"
	"XDG_DATA_HOME=${WORK_DIR}/data" "XDG_CACHE_HOME=${WORK_DIR}/cache" "HOME=${WORK_DIR}/home")
foreach(kyty_case IN ITEMS unset empty-wayland unreachable)
	if(kyty_case STREQUAL "unset")
		set(kyty_case_environment "")
	elseif(kyty_case STREQUAL "empty-wayland")
		set(kyty_case_environment "WAYLAND_DISPLAY=")
	else()
		set(kyty_case_environment "WAYLAND_DISPLAY=kyty-none" "DISPLAY=:96")
	endif()
	kyty_now_us(kyty_start)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env ${kyty_common_environment} ${kyty_case_environment}
		        "${LAUNCHER}"
		WORKING_DIRECTORY "${WORK_DIR}/cwd"
		INPUT_FILE /dev/null
		RESULT_VARIABLE kyty_result
		OUTPUT_VARIABLE kyty_stdout
		ERROR_VARIABLE kyty_stderr
		TIMEOUT 5)
	kyty_now_us(kyty_end)
	math(EXPR kyty_elapsed_ms "(${kyty_end} - ${kyty_start}) / 1000")
	message(STATUS "LauncherNoDisplayTest: ${kyty_case}: exit '${kyty_result}' after "
		"${kyty_elapsed_ms} ms; stderr '${kyty_stderr}'; stdout '${kyty_stdout}'")
	if(NOT kyty_result STREQUAL "1")
		string(APPEND kyty_failures "\n  ${kyty_case}: exit '${kyty_result}', expected 1")
	endif()
	if(NOT kyty_elapsed_ms LESS 5000)
		string(APPEND kyty_failures "\n  ${kyty_case}: ${kyty_elapsed_ms} ms, expected under 5000")
	endif()
	if(NOT kyty_stderr MATCHES "(^|\n)kyty_launcher: cannot open display: [^\n]")
		string(APPEND kyty_failures "\n  ${kyty_case}: stderr lacks a line "
			"\"kyty_launcher: cannot open display: <reason>\"")
	endif()
endforeach()

file(SHA256 "${kyty_settings}" kyty_hash_after)
kyty_tree_listing(kyty_listing_after)
kyty_portable_state(kyty_portable_after)
message(STATUS "LauncherNoDisplayTest: Kyty.ini sha256 after  ${kyty_hash_after}")
message(STATUS "LauncherNoDisplayTest: Kyty.ini next to the launcher after: "
	"${kyty_portable_after}")
list(JOIN kyty_listing_after ", " kyty_listing_text)
message(STATUS "LauncherNoDisplayTest: the private tree after: ${kyty_listing_text}")
if(NOT kyty_hash_after STREQUAL kyty_hash_before)
	string(APPEND kyty_failures
		"\n  Kyty.ini changed (sha256 ${kyty_hash_before} -> ${kyty_hash_after})")
endif()
if(NOT kyty_listing_after STREQUAL kyty_listing_before)
	list(JOIN kyty_listing_before ", " kyty_before_text)
	string(APPEND kyty_failures "\n  the private tree changed: before [${kyty_before_text}], "
		"after [${kyty_listing_text}]")
endif()
if(NOT kyty_portable_after STREQUAL kyty_portable_before)
	string(APPEND kyty_failures
		"\n  ${kyty_portable}: ${kyty_portable_before} before, ${kyty_portable_after} after")
endif()
if(kyty_failures)
	message(FATAL_ERROR "LauncherNoDisplayTest: FAILED (the tree stays in ${WORK_DIR}):"
		"${kyty_failures}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "LauncherNoDisplayTest: PASS (3 runs: exit 1 within 5 s, the message, "
	"Kyty.ini unchanged)")
