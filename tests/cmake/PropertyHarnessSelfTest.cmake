# property_harness_selftest (Requirements 17.8, 17.9): runs property 0 of EXE, which fails on purpose,
# twice.
#   Run 1, RC_PARAMS unset: must exit 1 and print "Using configuration: seed=<N>" and a
#   "Falsifiable after ..." report.
#   Run 2, RC_PARAMS="seed=<N>": must exit 1 and print the same seed line and an identical report,
#   that is, the same counterexample.
# A CTest WILL_FAIL property would invert a PASS_REGULAR_EXPRESSION, so this driver checks the
# non-zero exit status and the output itself.
#
#   cmake -DEXE=<property_harness_selftest_tests> -P tests/cmake/PropertyHarnessSelfTest.cmake
cmake_minimum_required(VERSION 3.19)

if(NOT EXE OR NOT EXISTS "${EXE}")
	message(FATAL_ERROR "PropertyHarnessSelfTest: -DEXE=<property test executable> is required")
endif()

# Sets <prefix>_result, <prefix>_seed, and <prefix>_report (the output from "Falsifiable after" to the
# end) from one run of property 0 with the environment changes in ARGN.
function(kyty_run_property prefix)
	execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ARGN} "${EXE}" 0
		RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_stdout ERROR_VARIABLE kyty_stderr
		TIMEOUT 50)
	set(kyty_output "${kyty_stdout}${kyty_stderr}")
	string(REGEX MATCH "Using configuration: seed=([0-9]+)" kyty_seed_line "${kyty_output}")
	set(kyty_seed "${CMAKE_MATCH_1}")
	set(kyty_report "")
	foreach(kyty_stream IN ITEMS kyty_stdout kyty_stderr)
		string(FIND "${${kyty_stream}}" "Falsifiable after" kyty_position)
		if(NOT kyty_position EQUAL -1)
			string(SUBSTRING "${${kyty_stream}}" ${kyty_position} -1 kyty_report)
			break()
		endif()
	endforeach()
	message(STATUS "PropertyHarnessSelfTest: ${prefix} (${ARGN}): exit ${kyty_result}\n"
		"--- stdout ---\n${kyty_stdout}--- stderr ---\n${kyty_stderr}--- end ---")
	set(${prefix}_result "${kyty_result}" PARENT_SCOPE)
	set(${prefix}_seed "${kyty_seed}" PARENT_SCOPE)
	set(${prefix}_report "${kyty_report}" PARENT_SCOPE)
endfunction()

set(kyty_failures "")
kyty_run_property(first --unset=RC_PARAMS)
if(NOT first_result STREQUAL "1")
	list(APPEND kyty_failures "run 1 exited with '${first_result}', expected 1 (the property fails)")
endif()
if(first_seed STREQUAL "")
	list(APPEND kyty_failures "run 1 printed no 'Using configuration: seed=<N>' line")
endif()
if(first_report STREQUAL "")
	list(APPEND kyty_failures "run 1 printed no 'Falsifiable after' report")
endif()

if(NOT kyty_failures)
	kyty_run_property(second "RC_PARAMS=seed=${first_seed}")
	if(NOT second_result STREQUAL "1")
		list(APPEND kyty_failures "run 2 exited with '${second_result}', expected 1")
	endif()
	if(NOT second_seed STREQUAL first_seed)
		list(APPEND kyty_failures "run 2 used seed '${second_seed}', expected ${first_seed}")
	endif()
	if(NOT second_report STREQUAL first_report)
		string(CONCAT kyty_difference "run 2 with seed ${first_seed} reported a different failure:\n"
			"--- run 1 ---\n${first_report}--- run 2 ---\n${second_report}")
		list(APPEND kyty_failures "${kyty_difference}")
	endif()
endif()

if(kyty_failures)
	list(JOIN kyty_failures "\n  " kyty_failures_text)
	message(FATAL_ERROR "PropertyHarnessSelfTest failed:\n  ${kyty_failures_text}")
endif()
message(STATUS "PropertyHarnessSelfTest: seed ${first_seed} reproduces the same counterexample; passed")
