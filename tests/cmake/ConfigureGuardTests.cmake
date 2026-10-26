# Configure-time guard self-tests, one CASE per CTest:
#   non_linux           configure_rejects_non_linux (Requirement 5.1): a toolchain file sets
#                       CMAKE_SYSTEM_NAME to FreeBSD.
#   sdl_backend         sdl_backend_guard (Requirement 5.8): SDL3 cannot find ALSA
#                       (CMAKE_DISABLE_FIND_PACKAGE_ALSA), so it cannot enable SDL_ALSA.
#   prefetched_missing  prefetched_missing_sources (Requirement 14.13): KYTY_PREFETCHED_SOURCES_DIR
#                       is an empty directory.
#   coverage_requires_clang
#                       coverage_requires_clang (Requirement 17.12): GCC and G++ as the compilers
#                       with KYTY_ENABLE_COVERAGE=ON.
# Each case configures SOURCE_DIR in a fresh WORK_DIR/build and expects the configure step to fail
# with the guard's message before any build file is generated. WORK_DIR is deleted when the case
# passes and kept for inspection when it fails. No case makes a network request: non_linux and
# coverage_requires_clang stop before any dependency is declared, and the other two set
# KYTY_PREFETCHED_SOURCES_DIR.
#
#   cmake -DCASE=<case> -DSOURCE_DIR=<repository> -DWORK_DIR=<dir> -DGENERATOR=<generator>
#         -DMAKE_PROGRAM=<program> -DC_COMPILER=<cc> -DCXX_COMPILER=<c++>
#         [-DPREFETCH_ENTRIES=<file>]      sdl_backend: "<relative path>=<source>" lines
#                                          (kyty_dependency_prefetch_entries of this build)
#         [-DDEPENDENCIES=<name>,<name>...] prefetched_missing: every active dependency, in table
#                                          order
#         [-DGCC=<gcc> -DGXX=<g++>]        coverage_requires_clang: the GNU compilers
#         -P tests/cmake/ConfigureGuardTests.cmake
cmake_minimum_required(VERSION 3.19)

foreach(kyty_variable IN ITEMS CASE SOURCE_DIR WORK_DIR GENERATOR MAKE_PROGRAM C_COMPILER CXX_COMPILER)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "ConfigureGuardTests: -D${kyty_variable}=<value> is required")
	endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(kyty_build_dir "${WORK_DIR}/build")
set(kyty_arguments
	-S "${SOURCE_DIR}" -B "${kyty_build_dir}" -G "${GENERATOR}"
	"-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
	"-DCMAKE_C_COMPILER=${C_COMPILER}"
	"-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
	"-DCMAKE_C_COMPILER_LAUNCHER="
	"-DCMAKE_CXX_COMPILER_LAUNCHER="
	-DCMAKE_BUILD_TYPE=Release)
set(kyty_expected_text "")
set(kyty_expected_regex "")
set(kyty_absent_paths "${kyty_build_dir}/build.ninja" "${kyty_build_dir}/Makefile")

if(CASE STREQUAL "non_linux")
	set(kyty_toolchain "${WORK_DIR}/freebsd-toolchain.cmake")
	file(WRITE "${kyty_toolchain}" "set(CMAKE_SYSTEM_NAME FreeBSD)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n")
	list(APPEND kyty_arguments "-DCMAKE_TOOLCHAIN_FILE=${kyty_toolchain}")
	set(kyty_expected_text "supports only Linux x86-64; detected CMAKE_SYSTEM_NAME='FreeBSD'")
elseif(CASE STREQUAL "sdl_backend")
	if(NOT PREFETCH_ENTRIES OR NOT EXISTS "${PREFETCH_ENTRIES}")
		message(FATAL_ERROR "ConfigureGuardTests: sdl_backend needs -DPREFETCH_ENTRIES=<file>")
	endif()
	# A symlink farm over this build's pristine dependency sources (Requirement 14.13 layout).
	set(kyty_farm "${WORK_DIR}/prefetched")
	file(MAKE_DIRECTORY "${kyty_farm}")
	file(STRINGS "${PREFETCH_ENTRIES}" kyty_entries)
	foreach(kyty_entry IN LISTS kyty_entries)
		string(FIND "${kyty_entry}" "=" kyty_separator)
		if(kyty_separator LESS 1)
			message(FATAL_ERROR "ConfigureGuardTests: malformed entry '${kyty_entry}' in ${PREFETCH_ENTRIES}")
		endif()
		string(SUBSTRING "${kyty_entry}" 0 ${kyty_separator} kyty_relative)
		math(EXPR kyty_source_begin "${kyty_separator} + 1")
		string(SUBSTRING "${kyty_entry}" ${kyty_source_begin} -1 kyty_source)
		if(NOT EXISTS "${kyty_source}")
			message(FATAL_ERROR "ConfigureGuardTests: the pristine source ${kyty_source} of ${kyty_relative} "
				"does not exist; sdl_backend_guard needs every dependency source of this build")
		endif()
		get_filename_component(kyty_parent "${kyty_farm}/${kyty_relative}" DIRECTORY)
		file(MAKE_DIRECTORY "${kyty_parent}")
		file(CREATE_LINK "${kyty_source}" "${kyty_farm}/${kyty_relative}" SYMBOLIC)
	endforeach()
	list(APPEND kyty_arguments "-DKYTY_PREFETCHED_SOURCES_DIR=${kyty_farm}" -DBUILD_TESTING=OFF
		-DCMAKE_DISABLE_FIND_PACKAGE_ALSA=ON)
	set(kyty_expected_regex "SDL3 could not enable the required backends: [A-Z0-9_, ]*SDL_ALSA")
elseif(CASE STREQUAL "prefetched_missing")
	if(NOT DEPENDENCIES)
		message(FATAL_ERROR "ConfigureGuardTests: prefetched_missing needs -DDEPENDENCIES=<name>,<name>...")
	endif()
	set(kyty_prefetched "${WORK_DIR}/prefetched")
	file(MAKE_DIRECTORY "${kyty_prefetched}")
	list(APPEND kyty_arguments "-DKYTY_PREFETCHED_SOURCES_DIR=${kyty_prefetched}")
	string(REPLACE "," ";" kyty_dependencies "${DEPENDENCIES}")
	list(LENGTH kyty_dependencies kyty_dependency_count)
	list(JOIN kyty_dependencies ", " kyty_dependencies_text)
	string(CONCAT kyty_expected_text "KYTY_PREFETCHED_SOURCES_DIR=${kyty_prefetched} lacks the sources of "
		"${kyty_dependency_count} of ${kyty_dependency_count} dependencies: ${kyty_dependencies_text}.")
	# No FetchContent call ran.
	list(APPEND kyty_absent_paths "${kyty_build_dir}/_deps")
elseif(CASE STREQUAL "coverage_requires_clang")
	foreach(kyty_compiler IN ITEMS GCC GXX)
		if(NOT ${kyty_compiler} OR NOT EXISTS "${${kyty_compiler}}")
			message(FATAL_ERROR "ConfigureGuardTests: coverage_requires_clang needs gcc and g++, but "
				"${kyty_compiler}='${${kyty_compiler}}' was not found; install gcc and gcc-c++ and reconfigure")
		endif()
	endforeach()
	list(REMOVE_ITEM kyty_arguments "-DCMAKE_C_COMPILER=${C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
	list(APPEND kyty_arguments "-DCMAKE_C_COMPILER=${GCC}" "-DCMAKE_CXX_COMPILER=${GXX}"
		-DKYTY_ENABLE_COVERAGE=ON)
	set(kyty_expected_text "coverage instrumentation requires Clang (found C: GNU, C++: GNU)")
	# The check runs before any dependency is declared.
	list(APPEND kyty_absent_paths "${kyty_build_dir}/_deps")
else()
	message(FATAL_ERROR "ConfigureGuardTests: unknown CASE '${CASE}' (non_linux, sdl_backend, prefetched_missing, "
		"coverage_requires_clang)")
endif()

list(JOIN kyty_arguments " " kyty_arguments_text)
message(STATUS "ConfigureGuardTests ${CASE}: cmake ${kyty_arguments_text}")
execute_process(COMMAND "${CMAKE_COMMAND}" ${kyty_arguments}
	RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_output)
file(WRITE "${WORK_DIR}/configure.log" "${kyty_output}")

# CMake wraps long error messages, so the texts are compared with every whitespace run made one space.
string(REGEX REPLACE "[ \t\r\n]+" " " kyty_normalized "${kyty_output}")
set(kyty_failures "")
if(NOT kyty_result MATCHES "^[0-9]+$" OR kyty_result EQUAL 0)
	list(APPEND kyty_failures "the configure step exited with '${kyty_result}', expected a non-zero status")
endif()
if(kyty_expected_text)
	string(FIND "${kyty_normalized}" "${kyty_expected_text}" kyty_position)
	if(kyty_position EQUAL -1)
		list(APPEND kyty_failures "the output lacks the text: ${kyty_expected_text}")
	endif()
endif()
if(kyty_expected_regex AND NOT kyty_normalized MATCHES "${kyty_expected_regex}")
	list(APPEND kyty_failures "the output does not match the regular expression: ${kyty_expected_regex}")
endif()
foreach(kyty_path IN LISTS kyty_absent_paths)
	if(EXISTS "${kyty_path}")
		list(APPEND kyty_failures "${kyty_path} exists")
	endif()
endforeach()

if(kyty_failures)
	list(JOIN kyty_failures "\n  " kyty_failures_text)
	message(FATAL_ERROR "ConfigureGuardTests ${CASE} failed (work directory kept: ${WORK_DIR}):\n  "
		"${kyty_failures_text}\n--- configure output (exit ${kyty_result}) ---\n${kyty_output}")
endif()
string(REGEX MATCH "CMake Error[^\n]*(\n[^\n]*)(\n[^\n]*)(\n[^\n]*)(\n[^\n]*)" kyty_error "${kyty_output}")
message(STATUS "ConfigureGuardTests ${CASE}: configure exited ${kyty_result} as expected:\n${kyty_error}")
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "ConfigureGuardTests ${CASE}: passed")
