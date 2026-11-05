# gtk_pkgconfig_guard (task 23.2; Requirement 6.5; phase 7 plan D37): kyty_require_gtk_modules()
# (cmake/KytyGtk.cmake) stops with "kyty_launcher requires <module> >= <min> (found: <version or
# none>)" for a missing module and for a module older than its minimum, naming only that module,
# and accepts the three modules at their minimum versions. Each case runs a cmake -P driver that
# calls kyty_require_gtk_modules() with PKG_CONFIG_LIBDIR set to a directory of generated .pc files
# and PKG_CONFIG_PATH unset: the fakes replace pkg-config's whole search path, so a module that is
# installed on the host is missing when its fake is (PKG_CONFIG_PATH alone would only add to it).
# Run as
#   cmake -DPKG_CONFIG=<pkg-config> -DMODULE_DIR=<source>/cmake -DWORK_DIR=<scratch directory>
#         -P GtkPkgconfigGuardTest.cmake
# WORK_DIR is recreated at the start and removed when every check passes.
cmake_minimum_required(VERSION 3.22)

foreach(kyty_variable IN ITEMS PKG_CONFIG MODULE_DIR WORK_DIR)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "GtkPkgconfigGuardTest: ${kyty_variable} is not set")
	endif()
endforeach()
if(NOT EXISTS "${MODULE_DIR}/KytyGtk.cmake")
	message(FATAL_ERROR "GtkPkgconfigGuardTest: ${MODULE_DIR}/KytyGtk.cmake does not exist")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(kyty_driver "${WORK_DIR}/driver.cmake")
file(WRITE "${kyty_driver}"
	"list(APPEND CMAKE_MODULE_PATH [==[${MODULE_DIR}]==])\n"
	"include(KytyGtk)\n"
	"kyty_require_gtk_modules()\n")
unset(ENV{PKG_CONFIG_PATH})

set(kyty_failures "")

# Writes <case>/<module>.pc for each "<module>=<version>" argument after the case name.
function(kyty_fake_modules case)
	file(MAKE_DIRECTORY "${WORK_DIR}/${case}")
	foreach(kyty_module_entry IN LISTS ARGN)
		string(REPLACE "=" ";" kyty_module_fields "${kyty_module_entry}")
		list(GET kyty_module_fields 0 kyty_module)
		list(GET kyty_module_fields 1 kyty_version)
		file(WRITE "${WORK_DIR}/${case}/${kyty_module}.pc"
			"Name: ${kyty_module}\nDescription: gtk_pkgconfig_guard fake\n"
			"Version: ${kyty_version}\nLibs:\nCflags:\n")
	endforeach()
endfunction()

# Runs the driver against <case>/ and checks the exit status (0 or 1) and the one expected line.
function(kyty_guard_case case expected_exit expected_text)
	set(ENV{PKG_CONFIG_LIBDIR} "${WORK_DIR}/${case}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" "-DPKG_CONFIG_EXECUTABLE=${PKG_CONFIG}" -P "${kyty_driver}"
		WORKING_DIRECTORY "${WORK_DIR}"
		RESULT_VARIABLE kyty_result
		OUTPUT_VARIABLE kyty_output
		ERROR_VARIABLE kyty_output
		TIMEOUT 30)
	unset(ENV{PKG_CONFIG_LIBDIR})
	message(STATUS "GtkPkgconfigGuardTest: ${case}: exit '${kyty_result}'\n${kyty_output}")
	string(REGEX MATCHALL "kyty_launcher requires [^\n]*" kyty_lines "${kyty_output}")
	list(LENGTH kyty_lines kyty_line_count)
	string(FIND "${kyty_output}" "${expected_text}" kyty_position)
	if(NOT kyty_result STREQUAL expected_exit)
		string(APPEND kyty_failures "\n  ${case}: exit '${kyty_result}', expected ${expected_exit}")
	endif()
	if(kyty_position LESS 0)
		string(APPEND kyty_failures "\n  ${case}: the output lacks \"${expected_text}\"")
	endif()
	if(expected_exit STREQUAL "0")
		set(kyty_expected_lines 0)
	else()
		set(kyty_expected_lines 1)
	endif()
	if(NOT kyty_line_count EQUAL kyty_expected_lines)
		string(APPEND kyty_failures "\n  ${case}: ${kyty_line_count} \"kyty_launcher requires\" "
			"lines, expected ${kyty_expected_lines}")
	endif()
	set(kyty_failures "${kyty_failures}" PARENT_SCOPE)
endfunction()

kyty_fake_modules(missing-libadwaita gtk4=4.14.0 libsoup-3.0=3.4.0)
kyty_guard_case(missing-libadwaita 1 "kyty_launcher requires libadwaita-1 >= 1.5 (found: none)")
kyty_fake_modules(old-gtk4 gtk4=4.12.0 libadwaita-1=1.5.0 libsoup-3.0=3.4.0)
kyty_guard_case(old-gtk4 1 "kyty_launcher requires gtk4 >= 4.14 (found: 4.12.0)")
kyty_fake_modules(minimums gtk4=4.14.0 libadwaita-1=1.5.0 libsoup-3.0=3.4.0)
kyty_guard_case(minimums 0 "KytyGtk: gtk4 4.14.0, libadwaita-1 1.5.0, libsoup-3.0 3.4.0")

if(kyty_failures)
	message(FATAL_ERROR "GtkPkgconfigGuardTest: FAILED (the tree stays in ${WORK_DIR}):"
		"${kyty_failures}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "GtkPkgconfigGuardTest: PASS (a missing and a too old module stop the "
	"configure; the minimums pass)")
