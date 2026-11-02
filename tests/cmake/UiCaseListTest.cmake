# launcher_ui_case_list (task 22.1; phase 7 plan D6): the cases that the UI test executable knows
# ("<EXE> --list", no display needed) are exactly those that the configure step registered tests
# for (CASES, written by kyty_add_ui_tests in cmake/KytyTesting.cmake), so a case cannot exist
# without its tests and a test cannot name a case that does not exist. Run as
#   cmake -DEXE=<launcher_ui_tests> -DCASES=<build>/tests/launcher_ui_cases.txt -P UiCaseListTest.cmake
foreach(kyty_variable IN ITEMS EXE CASES)
	if(NOT DEFINED ${kyty_variable})
		message(FATAL_ERROR "UiCaseListTest: ${kyty_variable} is not set")
	endif()
endforeach()
if(NOT EXISTS "${CASES}")
	message(FATAL_ERROR "UiCaseListTest: ${CASES} does not exist")
endif()

execute_process(
	COMMAND "${EXE}" --list
	RESULT_VARIABLE kyty_result
	OUTPUT_VARIABLE kyty_output
	ERROR_VARIABLE kyty_error
	TIMEOUT 30)
if(NOT kyty_result STREQUAL "0")
	message(FATAL_ERROR "UiCaseListTest: ${EXE} --list failed (${kyty_result}):\n${kyty_output}${kyty_error}")
endif()
string(REPLACE "\n" ";" kyty_listed "${kyty_output}")
list(FILTER kyty_listed EXCLUDE REGEX "^$")
file(STRINGS "${CASES}" kyty_registered)
list(FILTER kyty_registered EXCLUDE REGEX "^$")

set(kyty_missing ${kyty_registered})
if(kyty_listed)
	list(REMOVE_ITEM kyty_missing ${kyty_listed})
endif()
set(kyty_extra ${kyty_listed})
if(kyty_registered)
	list(REMOVE_ITEM kyty_extra ${kyty_registered})
endif()
list(LENGTH kyty_listed kyty_listed_count)
list(LENGTH kyty_registered kyty_registered_count)
if(kyty_missing OR kyty_extra OR NOT kyty_listed_count EQUAL kyty_registered_count)
	list(JOIN kyty_missing ", " kyty_missing_text)
	list(JOIN kyty_extra ", " kyty_extra_text)
	message(FATAL_ERROR "UiCaseListTest: the cases of ${EXE} differ from ${CASES} (configure again?): "
		"registered but unknown to --list: [${kyty_missing_text}]; listed but not registered: "
		"[${kyty_extra_text}]; ${kyty_listed_count} listed, ${kyty_registered_count} registered")
endif()
message(STATUS "UiCaseListTest: ${kyty_listed_count} cases, the same in --list and ${CASES}")
