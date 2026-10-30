# build_classification (task 17.6, Requirement 13.2): runs kyty_classify_build() from
# cmake/KytyBuildInfo.cmake in script mode for
# - every one of the 2^7 combinations of one matching and one non-matching value per input;
# - each extra non-matching value alone, the other six inputs matching (omitted and empty values,
#   other repositories, events, refs, and build types, true values for FLATPAK_BUILD);
# - each extra matching value alone (false values for FLATPAK_BUILD, other release tags);
# - kyty_apply_build_classification() on the CI environment variables.
# A build must be official exactly when every input matches, and the reason of a build that is not
# official must name the first condition that fails (FLATPAK_BUILD first, then the order of the
# kyty_classify_build() arguments).
#
#   cmake -DBUILD_INFO_SCRIPT=<repository>/cmake/KytyBuildInfo.cmake -P tests/cmake/BuildClassificationTest.cmake
cmake_minimum_required(VERSION 3.19)

if(NOT BUILD_INFO_SCRIPT OR NOT EXISTS "${BUILD_INFO_SCRIPT}")
	message(FATAL_ERROR "BuildClassificationTest: -DBUILD_INFO_SCRIPT=<path of cmake/KytyBuildInfo.cmake> is required")
endif()
include("${BUILD_INFO_SCRIPT}")

# The inputs in the order kyty_classify_build() checks them.
set(kyty_order FLATPAK_BUILD GITHUB_ACTIONS GITHUB_REPOSITORY GITHUB_EVENT_NAME GITHUB_REF RELEASE_TAG BUILD_TYPE)

set(kyty_match_FLATPAK_BUILD "OFF")
set(kyty_match_GITHUB_ACTIONS "true")
set(kyty_match_GITHUB_REPOSITORY "MrG00gle/KytyPS5-GTK")
set(kyty_match_GITHUB_EVENT_NAME "push")
set(kyty_match_GITHUB_REF "refs/heads/main")
set(kyty_match_RELEASE_TAG "2026-10-30-8d525f7")
set(kyty_match_BUILD_TYPE "Release")

set(kyty_miss_FLATPAK_BUILD "ON")
set(kyty_miss_GITHUB_ACTIONS "false")
set(kyty_miss_GITHUB_REPOSITORY "KytyPS5/KytyPS5")
set(kyty_miss_GITHUB_EVENT_NAME "pull_request")
set(kyty_miss_GITHUB_REF "refs/heads/dev")
set(kyty_miss_RELEASE_TAG "")
set(kyty_miss_BUILD_TYPE "Debug")

# The text that the reason must contain when this input is the first failing condition.
set(kyty_needle_FLATPAK_BUILD "Flatpak build")
set(kyty_needle_GITHUB_ACTIONS "GITHUB_ACTIONS")
set(kyty_needle_GITHUB_REPOSITORY "GITHUB_REPOSITORY")
set(kyty_needle_GITHUB_EVENT_NAME "GITHUB_EVENT_NAME")
set(kyty_needle_GITHUB_REF "GITHUB_REF is")
set(kyty_needle_RELEASE_TAG "release tag")
set(kyty_needle_BUILD_TYPE "build type")

set_property(GLOBAL PROPERTY KYTY_CLASSIFICATION_CASES 0)

# Classifies the values kyty_v_<input> (inputs listed in kyty_omit are left out of the call) and
# checks the result: official exactly when <first_failing> is empty, else a reason naming it.
function(kyty_classification_case label first_failing)
	# Bracket arguments keep empty values as arguments.
	set(kyty_code "kyty_classify_build(kyty_result")
	foreach(kyty_input IN LISTS kyty_order)
		if(NOT kyty_input IN_LIST kyty_omit)
			string(APPEND kyty_code " ${kyty_input} [==[${kyty_v_${kyty_input}}]==]")
		endif()
	endforeach()
	string(APPEND kyty_code " REASON kyty_reason)")
	cmake_language(EVAL CODE "${kyty_code}")

	set(kyty_problem "")
	if(first_failing STREQUAL "")
		if(NOT kyty_result STREQUAL "ON")
			set(kyty_problem "not official (${kyty_reason}), expected official")
		elseif(NOT kyty_reason STREQUAL "")
			set(kyty_problem "official, but the reason is '${kyty_reason}'")
		endif()
	else()
		string(FIND "${kyty_reason}" "${kyty_needle_${first_failing}}" kyty_position)
		if(NOT kyty_result STREQUAL "OFF")
			set(kyty_problem "'${kyty_result}', expected OFF because of ${first_failing}")
		elseif(kyty_position EQUAL -1)
			set(kyty_problem "the reason '${kyty_reason}' does not name ${first_failing}")
		endif()
	endif()
	if(kyty_problem)
		set_property(GLOBAL APPEND PROPERTY KYTY_CLASSIFICATION_FAILURES "${label}: ${kyty_problem}\n    call: ${kyty_code}")
	endif()
	get_property(kyty_count GLOBAL PROPERTY KYTY_CLASSIFICATION_CASES)
	math(EXPR kyty_count "${kyty_count} + 1")
	set_property(GLOBAL PROPERTY KYTY_CLASSIFICATION_CASES ${kyty_count})
endfunction()

# Every input matching except <input>, which gets <value> (or is omitted when <value> is
# "<omitted>"); <first_failing> is <input> for a non-matching value and empty for a matching one.
function(kyty_variant_case input value first_failing)
	foreach(kyty_input IN LISTS kyty_order)
		set(kyty_v_${kyty_input} "${kyty_match_${kyty_input}}")
	endforeach()
	set(kyty_omit "")
	if(value STREQUAL "<omitted>")
		set(kyty_omit ${input})
	else()
		set(kyty_v_${input} "${value}")
	endif()
	kyty_classification_case("${input}='${value}'" "${first_failing}")
endfunction()

# 1. Every combination: bit i of the mask set means input i of kyty_order matches.
set(kyty_combinations 0)
set(kyty_official_combinations 0)
foreach(kyty_mask RANGE 0 127)
	set(kyty_first_failing "")
	set(kyty_label "mask ${kyty_mask}:")
	set(kyty_bit 0)
	foreach(kyty_input IN LISTS kyty_order)
		math(EXPR kyty_matches "(${kyty_mask} >> ${kyty_bit}) & 1")
		if(kyty_matches)
			set(kyty_v_${kyty_input} "${kyty_match_${kyty_input}}")
		else()
			set(kyty_v_${kyty_input} "${kyty_miss_${kyty_input}}")
			if(kyty_first_failing STREQUAL "")
				set(kyty_first_failing ${kyty_input})
			endif()
		endif()
		string(APPEND kyty_label " ${kyty_input}='${kyty_v_${kyty_input}}'")
		math(EXPR kyty_bit "${kyty_bit} + 1")
	endforeach()
	set(kyty_omit "")
	kyty_classification_case("${kyty_label}" "${kyty_first_failing}")
	math(EXPR kyty_combinations "${kyty_combinations} + 1")
	if(kyty_first_failing STREQUAL "")
		math(EXPR kyty_official_combinations "${kyty_official_combinations} + 1")
	endif()
endforeach()

# 2. Extra non-matching values, one input at a time.
foreach(kyty_value IN ITEMS "<omitted>" "" "false" "True" "TRUE" "1" "yes" " true" "true ")
	kyty_variant_case(GITHUB_ACTIONS "${kyty_value}" GITHUB_ACTIONS)
endforeach()
foreach(kyty_value IN ITEMS "<omitted>" "" "KytyPS5/KytyPS5" "mrg00gle/kytyps5-gtk" "MrG00gle/KytyPS5"
		"MrG00gle/KytyPS5-GTK-fork" "someone/KytyPS5-GTK" "MrG00gle/KytyPS5-GTK/" " MrG00gle/KytyPS5-GTK")
	kyty_variant_case(GITHUB_REPOSITORY "${kyty_value}" GITHUB_REPOSITORY)
endforeach()
foreach(kyty_value IN ITEMS "<omitted>" "" "pull_request" "workflow_dispatch" "pull_request_target" "schedule"
		"release" "Push" "push ")
	kyty_variant_case(GITHUB_EVENT_NAME "${kyty_value}" GITHUB_EVENT_NAME)
endforeach()
foreach(kyty_value IN ITEMS "<omitted>" "" "refs/heads/dev" "refs/pull/1/merge" "refs/heads/main2" "refs/tags/v1.0"
		"main" "refs/heads/main/" "refs/heads/Main" "refs/remotes/origin/main")
	kyty_variant_case(GITHUB_REF "${kyty_value}" GITHUB_REF)
endforeach()
foreach(kyty_value IN ITEMS "<omitted>" "")
	kyty_variant_case(RELEASE_TAG "${kyty_value}" RELEASE_TAG)
endforeach()
foreach(kyty_value IN ITEMS "<omitted>" "" "Debug" "RelWithDebInfo" "MinSizeRel" "release" "RELEASE" "Release ")
	kyty_variant_case(BUILD_TYPE "${kyty_value}" BUILD_TYPE)
endforeach()
foreach(kyty_value IN ITEMS "ON" "on" "TRUE" "1" "YES" "Y" "2" "flatpak")
	kyty_variant_case(FLATPAK_BUILD "${kyty_value}" FLATPAK_BUILD)
endforeach()

# 3. Extra matching values: false values for FLATPAK_BUILD and other non-empty release tags.
foreach(kyty_value IN ITEMS "<omitted>" "" "OFF" "off" "0" "FALSE" "NO" "N" "IGNORE" "NOTFOUND" "KYTY-NOTFOUND")
	kyty_variant_case(FLATPAK_BUILD "${kyty_value}" "")
endforeach()
foreach(kyty_value IN ITEMS "v1" "1.0.0" "2026-01-01-abcdef0")
	kyty_variant_case(RELEASE_TAG "${kyty_value}" "")
endforeach()

# 4. kyty_apply_build_classification(): the CI environment, KYTY_RELEASE_TAG, CMAKE_BUILD_TYPE, and
#    KYTY_FLATPAK_BUILD. <change> names one variable to unset or replace.
set(kyty_ci_variables GITHUB_ACTIONS GITHUB_REPOSITORY GITHUB_EVENT_NAME GITHUB_REF)
function(kyty_apply_case label change value expected)
	foreach(kyty_variable IN LISTS kyty_ci_variables)
		set(ENV{${kyty_variable}} "${kyty_match_${kyty_variable}}")
	endforeach()
	set(KYTY_RELEASE_TAG "${kyty_match_RELEASE_TAG}")
	set(CMAKE_BUILD_TYPE "${kyty_match_BUILD_TYPE}")
	set(KYTY_FLATPAK_BUILD "${kyty_match_FLATPAK_BUILD}")
	if(change IN_LIST kyty_ci_variables)
		if(value STREQUAL "<unset>")
			unset(ENV{${change}})
		else()
			set(ENV{${change}} "${value}")
		endif()
	elseif(NOT change STREQUAL "")
		set(${change} "${value}")
	endif()
	set(KYTY_OFFICIAL_BUILD "unchanged")
	kyty_apply_build_classification()
	if(NOT KYTY_OFFICIAL_BUILD STREQUAL expected)
		set_property(GLOBAL APPEND PROPERTY KYTY_CLASSIFICATION_FAILURES
			"kyty_apply_build_classification() ${label}: KYTY_OFFICIAL_BUILD is '${KYTY_OFFICIAL_BUILD}', expected ${expected}")
	endif()
	get_property(kyty_count GLOBAL PROPERTY KYTY_CLASSIFICATION_CASES)
	math(EXPR kyty_count "${kyty_count} + 1")
	set_property(GLOBAL PROPERTY KYTY_CLASSIFICATION_CASES ${kyty_count})
endfunction()

kyty_apply_case("with every input matching" "" "" ON)
foreach(kyty_variable IN LISTS kyty_ci_variables)
	kyty_apply_case("without ${kyty_variable}" ${kyty_variable} "<unset>" OFF)
	kyty_apply_case("with ${kyty_variable} not matching" ${kyty_variable} "${kyty_miss_${kyty_variable}}" OFF)
endforeach()
kyty_apply_case("with an empty KYTY_RELEASE_TAG" KYTY_RELEASE_TAG "" OFF)
kyty_apply_case("with CMAKE_BUILD_TYPE Debug" CMAKE_BUILD_TYPE "Debug" OFF)
kyty_apply_case("with KYTY_FLATPAK_BUILD ON" KYTY_FLATPAK_BUILD "ON" OFF)
foreach(kyty_variable IN LISTS kyty_ci_variables)
	unset(ENV{${kyty_variable}})
endforeach()

get_property(kyty_cases GLOBAL PROPERTY KYTY_CLASSIFICATION_CASES)
get_property(kyty_failures GLOBAL PROPERTY KYTY_CLASSIFICATION_FAILURES)
if(kyty_failures)
	list(LENGTH kyty_failures kyty_failure_count)
	list(JOIN kyty_failures "\n  " kyty_failures_text)
	message(FATAL_ERROR "BuildClassificationTest: ${kyty_failure_count} of ${kyty_cases} cases failed:\n  ${kyty_failures_text}")
endif()
if(NOT kyty_combinations EQUAL 128 OR NOT kyty_official_combinations EQUAL 1)
	message(FATAL_ERROR "BuildClassificationTest: ran ${kyty_combinations} combinations with "
		"${kyty_official_combinations} official, expected 128 and 1")
endif()
message(STATUS "BuildClassificationTest: passed ${kyty_cases} cases (128 combinations, 1 of them official)")
