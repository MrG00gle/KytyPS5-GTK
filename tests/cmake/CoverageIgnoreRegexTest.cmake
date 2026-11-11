# coverage_ignore_regex (Requirement 17.3): checks kyty_coverage_ignore_regex() from
# cmake/KytyCoverageFilter.cmake in script mode:
# - the exact value for a plain build directory, and the dependency and test filters it keeps;
# - for build directories with +, ., (, ), [, ], spaces, and every other metacharacter of POSIX
#   extended regular expressions: the escaped part equals a character-by-character escape of the
#   directory (so no ordinary character, digits included, gets a backslash), files under the
#   directory match, and a prefix-sibling directory, the directory later in a path, a source file,
#   and a path with each "." replaced do not;
# - the value survives the argument list that cmake/KytyCoverageRun.cmake passes to llvm-cov;
# - a trailing "/" changes nothing; an in-source build, and a source directory below the build
#   directory, get no build directory part; a relative directory stops with an error.
#
#   cmake -DFILTER_SCRIPT=<repository>/cmake/KytyCoverageFilter.cmake -P tests/cmake/CoverageIgnoreRegexTest.cmake
cmake_minimum_required(VERSION 3.19)

if(NOT FILTER_SCRIPT OR NOT EXISTS "${FILTER_SCRIPT}")
	message(FATAL_ERROR "CoverageIgnoreRegexTest: -DFILTER_SCRIPT=<path of cmake/KytyCoverageFilter.cmake> is required")
endif()
include("${FILTER_SCRIPT}")

# Child run for the relative-directory case: the call must stop with FATAL_ERROR.
if(DEFINED RELATIVE_CASE)
	kyty_coverage_ignore_regex(kyty_regex "${RELATIVE_CASE}" /s/repo)
	message(STATUS "CoverageIgnoreRegexTest: no error for '${RELATIVE_CASE}'")
	return()
endif()

set_property(GLOBAL PROPERTY KYTY_IGNORE_REGEX_CASES 0)

function(kyty_ignore_regex_problem text)
	set_property(GLOBAL APPEND PROPERTY KYTY_IGNORE_REGEX_FAILURES "${text}")
endfunction()

function(kyty_ignore_regex_count)
	get_property(kyty_count GLOBAL PROPERTY KYTY_IGNORE_REGEX_CASES)
	math(EXPR kyty_count "${kyty_count} + 1")
	set_property(GLOBAL PROPERTY KYTY_IGNORE_REGEX_CASES ${kyty_count})
endfunction()

# <regex> must match <path> when <expected> is TRUE, and must not when it is FALSE.
function(kyty_expect_match regex path expected)
	string(REGEX MATCH "${regex}" kyty_match "${path}")
	if(expected AND kyty_match STREQUAL "")
		kyty_ignore_regex_problem("'${regex}' does not match '${path}'")
	elseif(NOT expected AND NOT kyty_match STREQUAL "")
		kyty_ignore_regex_problem("'${regex}' matches '${path}' (matched '${kyty_match}')")
	endif()
	kyty_ignore_regex_count()
endfunction()

# The value for <build> and <source> must equal <expected>.
function(kyty_expect_value build source expected)
	kyty_coverage_ignore_regex(kyty_regex "${build}" "${source}")
	if(NOT kyty_regex STREQUAL expected)
		kyty_ignore_regex_problem("build '${build}', source '${source}': '${kyty_regex}', expected '${expected}'")
	endif()
	kyty_ignore_regex_count()
endfunction()

# The reference escape, one character at a time.
function(kyty_reference_escape text out)
	set(kyty_metacharacters [=[\^$.[]|()*+?{}]=])
	set(kyty_result "")
	string(LENGTH "${text}" kyty_length)
	if(kyty_length GREATER 0)
		math(EXPR kyty_last "${kyty_length} - 1")
		foreach(kyty_index RANGE 0 ${kyty_last})
			string(SUBSTRING "${text}" ${kyty_index} 1 kyty_character)
			string(FIND "${kyty_metacharacters}" "${kyty_character}" kyty_position)
			if(NOT kyty_position EQUAL -1)
				string(APPEND kyty_result "\\")
			endif()
			string(APPEND kyty_result "${kyty_character}")
		endforeach()
	endif()
	set(${out} "${kyty_result}" PARENT_SCOPE)
endfunction()

set(kyty_pattern "(3rdparty|_deps|_kyty_patched|/tests/)")

# 1. A plain build directory: the exact value, and the filters of the dependencies and the tests.
kyty_expect_value(/b/build /s/repo "${kyty_pattern}|^/b/build/")
kyty_expect_value(/b/build/ /s/repo "${kyty_pattern}|^/b/build/")
kyty_expect_value(/b/build// /s/repo/ "${kyty_pattern}|^/b/build/")
kyty_coverage_ignore_regex(kyty_plain /b/build /s/repo)
foreach(kyty_path IN ITEMS /s/repo/3rdparty/vma/vk_mem_alloc.h /b/build/_deps/fmt-src/include/fmt/core.h
		/x/_kyty_patched/a.cpp /s/repo/tests/launcher_core/AtomicFileTests.cpp /b/build/cmake_config.h)
	kyty_expect_match("${kyty_plain}" "${kyty_path}" TRUE)
endforeach()
foreach(kyty_path IN ITEMS /s/repo/src/launcher/core/atomicFile.cpp /b/build-other/a.h /b/buildx/a.h
		/b/build /x/b/build/a.h /s/repo/src/emulator/testsuite.cpp)
	kyty_expect_match("${kyty_plain}" "${kyty_path}" FALSE)
endforeach()

# 2. Build directories with metacharacters and spaces, outside and inside the checkout.
set(kyty_source /home/user/Projects/KytyPS5-GTK)
set(kyty_build_dirs
	"/home/user/Projects/KytyPS5-GTK/_Build/phase3-debug"
	"/tmp/a+b/build"
	"/tmp/v1.2/build.dir"
	"/tmp/x(1)/build"
	"/tmp/[ci]/build"
	"/tmp/my builds/debug 2"
	"/tmp/c++ (x86-64) [v1.0]/build"
	[=[/tmp/all \^$.|()*+?{}[]/build]=]
	[=[/tmp/b{2}/a{1,3}/x|y/^z$]=])
foreach(kyty_build IN LISTS kyty_build_dirs)
	kyty_coverage_ignore_regex(kyty_regex "${kyty_build}" "${kyty_source}")
	kyty_reference_escape("${kyty_build}" kyty_escaped)
	set(kyty_expected "${kyty_pattern}|^${kyty_escaped}/")
	if(NOT kyty_regex STREQUAL kyty_expected)
		kyty_ignore_regex_problem("build '${kyty_build}': '${kyty_regex}', expected '${kyty_expected}'")
	endif()
	kyty_ignore_regex_count()
	foreach(kyty_file IN ITEMS cmake_config.h common/localizationData.h launcher/gtk/kyty-launcher-resources.c
			externals/ffmpeg-9ac4cfd195f1/include/libavutil/avutil.h)
		kyty_expect_match("${kyty_regex}" "${kyty_build}/${kyty_file}" TRUE)
	endforeach()
	kyty_expect_match("${kyty_regex}" "${kyty_build}x/cmake_config.h" FALSE)
	kyty_expect_match("${kyty_regex}" "${kyty_build}-other/cmake_config.h" FALSE)
	kyty_expect_match("${kyty_regex}" "/elsewhere${kyty_build}/cmake_config.h" FALSE)
	kyty_expect_match("${kyty_regex}" "${kyty_source}/src/launcher/core/atomicFile.cpp" FALSE)
	if(kyty_build MATCHES "[.]")
		string(REPLACE "." "x" kyty_decoy "${kyty_build}")
		kyty_expect_match("${kyty_regex}" "${kyty_decoy}/cmake_config.h" FALSE)
	endif()
endforeach()

# 3. The argument list of cmake/KytyCoverageRun.cmake keeps the value as one argument.
set(kyty_build [=[/tmp/c++ (x86-64) [v1.0]/all \^$.|()*+?{}[]/build]=])
kyty_coverage_ignore_regex(kyty_regex "${kyty_build}" "${kyty_source}")
set(kyty_arguments "-instr-profile=${kyty_build}/coverage/coverage.profdata"
	"-ignore-filename-regex=${kyty_regex}" "-num-threads=2" "${kyty_build}/a" "-object=${kyty_build}/b")
set(kyty_seen "")
foreach(kyty_argument IN LISTS kyty_arguments)
	list(APPEND kyty_seen "<${kyty_argument}>")
endforeach()
list(LENGTH kyty_seen kyty_seen_count)
list(GET kyty_seen 1 kyty_seen_regex)
if(NOT kyty_seen_count EQUAL 5 OR NOT kyty_seen_regex STREQUAL "<-ignore-filename-regex=${kyty_regex}>")
	kyty_ignore_regex_problem("the argument list gives ${kyty_seen_count} arguments: ${kyty_seen}")
endif()
kyty_ignore_regex_count()

# 4. In-source builds and a source directory below the build directory: no build directory part.
kyty_expect_value(/s/repo /s/repo "${kyty_pattern}")
kyty_expect_value(/s/repo/ /s/repo "${kyty_pattern}")
kyty_expect_value(/s /s/repo "${kyty_pattern}")
kyty_expect_value(/ /s/repo "${kyty_pattern}")
kyty_expect_value(/s/repo /s/repo-src "${kyty_pattern}|^/s/repo/")
kyty_expect_value(/s/repo-build /s/repo "${kyty_pattern}|^/s/repo-build/")
kyty_expect_value(/s/repo/_Build/debug /s/repo "${kyty_pattern}|^/s/repo/_Build/debug/")

# 5. A relative build directory stops with an error that names it.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DFILTER_SCRIPT=${FILTER_SCRIPT}" -DRELATIVE_CASE=_Build/debug
	-P "${CMAKE_CURRENT_LIST_FILE}"
	RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_output)
string(REGEX REPLACE "[ \t\r\n]+" " " kyty_output "${kyty_output}")
if(kyty_result STREQUAL "0" OR NOT kyty_output MATCHES "'_Build/debug' is not an absolute path")
	kyty_ignore_regex_problem("relative build directory: exit '${kyty_result}', output: ${kyty_output}")
endif()
kyty_ignore_regex_count()

get_property(kyty_cases GLOBAL PROPERTY KYTY_IGNORE_REGEX_CASES)
get_property(kyty_failures GLOBAL PROPERTY KYTY_IGNORE_REGEX_FAILURES)
if(kyty_failures)
	list(LENGTH kyty_failures kyty_failure_count)
	list(JOIN kyty_failures "\n  " kyty_failures_text)
	message(FATAL_ERROR "CoverageIgnoreRegexTest: ${kyty_failure_count} of ${kyty_cases} checks failed:\n"
		"  ${kyty_failures_text}")
endif()
message(STATUS "CoverageIgnoreRegexTest: passed ${kyty_cases} checks")
