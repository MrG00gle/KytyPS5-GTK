# Build classification (Requirement 13.2; design.md "Updater interfaces").
#
#   kyty_classify_build(<out> [GITHUB_ACTIONS <v>] [GITHUB_REPOSITORY <v>] [GITHUB_EVENT_NAME <v>]
#                       [GITHUB_REF <v>] [RELEASE_TAG <v>] [BUILD_TYPE <v>] [FLATPAK_BUILD <v>]
#                       [REASON <reason-out>])
#       Sets <out> to ON only when every one of these holds, else to OFF:
#       - FLATPAK_BUILD is a CMake false value (OFF, 0, NO, FALSE, N, IGNORE, NOTFOUND, empty, or
#         omitted);
#       - GITHUB_ACTIONS is exactly "true";
#       - GITHUB_REPOSITORY is exactly "MrG00gle/KytyPS5-GTK";
#       - GITHUB_EVENT_NAME is exactly "push";
#       - GITHUB_REF is exactly "refs/heads/main";
#       - RELEASE_TAG is not empty;
#       - BUILD_TYPE is exactly "Release".
#       An omitted keyword counts as an empty value. REASON receives "" for an official build, else the
#       first condition of that list that fails (the Flatpak build first: it is never official,
#       whatever the CI says). Only script-mode commands, so it also runs under cmake -P
#       (tests/cmake/BuildClassificationTest.cmake).
#   kyty_apply_build_classification()
#       Classifies this configure from $ENV{GITHUB_ACTIONS}, $ENV{GITHUB_REPOSITORY},
#       $ENV{GITHUB_EVENT_NAME}, $ENV{GITHUB_REF}, KYTY_RELEASE_TAG, CMAKE_BUILD_TYPE, and
#       KYTY_FLATPAK_BUILD, sets KYTY_OFFICIAL_BUILD (ON or OFF) in the caller's scope, and prints one
#       STATUS line with the result and, when the build is not official, the reason.
#
# Builds made outside the CI_Pipeline are never official. Spoofing the CI variables on purpose is out
# of scope (design.md). The Updater also keeps the Qt launcher's "-dirty" check at run time
# (src/updater/core/updatePolicy.h, SelectUpdateUiMode).
include_guard(GLOBAL)

# A keyword followed by an empty string defines an empty value (CMake 3.31 and newer). The functions
# below record this setting, so script-mode callers with an older policy version get no warning.
# include() keeps it out of the including file's policy scope.
if(POLICY CMP0174)
	cmake_policy(SET CMP0174 NEW)
endif()

set(KYTY_OFFICIAL_REPOSITORY "MrG00gle/KytyPS5-GTK")

function(kyty_classify_build out)
	set(kyty_inputs GITHUB_ACTIONS GITHUB_REPOSITORY GITHUB_EVENT_NAME GITHUB_REF RELEASE_TAG BUILD_TYPE
		FLATPAK_BUILD)
	cmake_parse_arguments(PARSE_ARGV 1 kyty "" "${kyty_inputs};REASON" "")
	if(kyty_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "kyty_classify_build: unexpected arguments: ${kyty_UNPARSED_ARGUMENTS}")
	endif()
	# An omitted keyword, or one followed by an empty string, is an empty value.
	foreach(kyty_input IN LISTS kyty_inputs)
		if(NOT DEFINED kyty_${kyty_input})
			set(kyty_${kyty_input} "")
		endif()
	endforeach()

	set(kyty_reason "")
	if(kyty_FLATPAK_BUILD)
		set(kyty_reason "this is a Flatpak build (FLATPAK_BUILD is '${kyty_FLATPAK_BUILD}')")
	elseif(NOT kyty_GITHUB_ACTIONS STREQUAL "true")
		set(kyty_reason "GITHUB_ACTIONS is '${kyty_GITHUB_ACTIONS}', not 'true' (not a CI_Pipeline build)")
	elseif(NOT kyty_GITHUB_REPOSITORY STREQUAL KYTY_OFFICIAL_REPOSITORY)
		set(kyty_reason "GITHUB_REPOSITORY is '${kyty_GITHUB_REPOSITORY}', not '${KYTY_OFFICIAL_REPOSITORY}'")
	elseif(NOT kyty_GITHUB_EVENT_NAME STREQUAL "push")
		set(kyty_reason "GITHUB_EVENT_NAME is '${kyty_GITHUB_EVENT_NAME}', not 'push'")
	elseif(NOT kyty_GITHUB_REF STREQUAL "refs/heads/main")
		set(kyty_reason "GITHUB_REF is '${kyty_GITHUB_REF}', not 'refs/heads/main'")
	elseif(kyty_RELEASE_TAG STREQUAL "")
		set(kyty_reason "the release tag is empty")
	elseif(NOT kyty_BUILD_TYPE STREQUAL "Release")
		set(kyty_reason "the build type is '${kyty_BUILD_TYPE}', not 'Release'")
	endif()

	if(kyty_reason STREQUAL "")
		set(${out} ON PARENT_SCOPE)
	else()
		set(${out} OFF PARENT_SCOPE)
	endif()
	if(NOT "${kyty_REASON}" STREQUAL "")
		set(${kyty_REASON} "${kyty_reason}" PARENT_SCOPE)
	endif()
endfunction()

function(kyty_apply_build_classification)
	kyty_classify_build(kyty_official
		GITHUB_ACTIONS "$ENV{GITHUB_ACTIONS}"
		GITHUB_REPOSITORY "$ENV{GITHUB_REPOSITORY}"
		GITHUB_EVENT_NAME "$ENV{GITHUB_EVENT_NAME}"
		GITHUB_REF "$ENV{GITHUB_REF}"
		RELEASE_TAG "${KYTY_RELEASE_TAG}"
		BUILD_TYPE "${CMAKE_BUILD_TYPE}"
		FLATPAK_BUILD "${KYTY_FLATPAK_BUILD}"
		REASON kyty_reason)
	if(kyty_official)
		message(STATUS "KytyBuildInfo: official build of release ${KYTY_RELEASE_TAG}")
	else()
		message(STATUS "KytyBuildInfo: not an official build: ${kyty_reason}")
	endif()
	set(KYTY_OFFICIAL_BUILD ${kyty_official} PARENT_SCOPE)
endfunction()
