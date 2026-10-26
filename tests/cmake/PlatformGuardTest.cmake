# platform_guard (Requirements 5.1, 5.2): runs the platform guard in script mode,
#   cmake -DCMAKE_SYSTEM_NAME=<name> -DCMAKE_SYSTEM_PROCESSOR=<cpu> -DCMAKE_SIZEOF_VOID_P=<n>
#         -P cmake/KytyPlatform.cmake
# for each system name, processor, and pointer size below. A rejected combination must fail and name
# its value; Linux x86-64 must pass.
#
#   cmake -DPLATFORM_SCRIPT=<repository>/cmake/KytyPlatform.cmake -P tests/cmake/PlatformGuardTest.cmake
cmake_minimum_required(VERSION 3.19)

if(NOT PLATFORM_SCRIPT OR NOT EXISTS "${PLATFORM_SCRIPT}")
	message(FATAL_ERROR "PlatformGuardTest: -DPLATFORM_SCRIPT=<path of cmake/KytyPlatform.cmake> is required")
endif()

set(kyty_guard_text "supports only Linux x86-64")

# <expect> is "fail" or "pass"; a failing case must print <needle>.
function(kyty_platform_case name processor pointer_size expect needle)
	set(kyty_case "${name}/${processor}/${pointer_size}")
	execute_process(COMMAND "${CMAKE_COMMAND}" "-DCMAKE_SYSTEM_NAME=${name}"
		"-DCMAKE_SYSTEM_PROCESSOR=${processor}" "-DCMAKE_SIZEOF_VOID_P=${pointer_size}"
		-P "${PLATFORM_SCRIPT}"
		RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_output)
	# CMake wraps long error messages; compare with every whitespace run made one space.
	string(REGEX REPLACE "[ \t\r\n]+" " " kyty_normalized "${kyty_output}")
	string(FIND "${kyty_normalized}" "${kyty_guard_text}" kyty_guard_position)
	set(kyty_problem "")
	if(expect STREQUAL "fail")
		string(FIND "${kyty_normalized}" "${needle}" kyty_needle_position)
		if(NOT kyty_result MATCHES "^[0-9]+$" OR kyty_result EQUAL 0)
			set(kyty_problem "exited with '${kyty_result}', expected a non-zero status")
		elseif(kyty_guard_position EQUAL -1 OR kyty_needle_position EQUAL -1)
			set(kyty_problem "the message lacks '${kyty_guard_text}' or '${needle}'")
		endif()
	else()
		if(NOT kyty_result STREQUAL "0")
			set(kyty_problem "exited with '${kyty_result}', expected 0")
		elseif(NOT kyty_guard_position EQUAL -1)
			set(kyty_problem "printed the guard message")
		endif()
	endif()
	if(kyty_problem)
		set_property(GLOBAL APPEND PROPERTY KYTY_PLATFORM_FAILURES
			"${kyty_case}: ${kyty_problem}\n    output: ${kyty_normalized}")
	elseif(expect STREQUAL "fail")
		message(STATUS "PlatformGuardTest: ${kyty_case}: rejected as expected (exit ${kyty_result}, names ${needle})")
	else()
		message(STATUS "PlatformGuardTest: ${kyty_case}: accepted as expected (exit 0)")
	endif()
endfunction()

kyty_platform_case(FreeBSD x86_64 8 fail "CMAKE_SYSTEM_NAME='FreeBSD'")
kyty_platform_case(Linux aarch64 8 fail "CMAKE_SYSTEM_PROCESSOR='aarch64'")
kyty_platform_case(Linux x86_64 4 fail "CMAKE_SIZEOF_VOID_P='4'")
kyty_platform_case(Linux x86_64 8 pass "")
kyty_platform_case(Linux AMD64 8 pass "")

get_property(kyty_failures GLOBAL PROPERTY KYTY_PLATFORM_FAILURES)
if(kyty_failures)
	list(JOIN kyty_failures "\n  " kyty_failures_text)
	message(FATAL_ERROR "PlatformGuardTest failed:\n  ${kyty_failures_text}")
endif()
message(STATUS "PlatformGuardTest: passed")
