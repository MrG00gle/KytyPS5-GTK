# The file filter of the coverage reports (Requirement 17.3; cmake/KytyCoverageRun.cmake).
#
#   kyty_coverage_ignore_regex(<out> <build-dir> <source-dir>)
#       Sets <out> to the -ignore-filename-regex value of llvm-cov export and llvm-cov show. It leaves
#       out the bundled and fetched dependencies (3rdparty, _deps, _kyty_patched), the tests (/tests/),
#       and every file under <build-dir>: generated headers and sources, and the prebuilt FFmpeg
#       headers. <build-dir> is matched from the start of the path, with every metacharacter of POSIX
#       extended regular expressions (the syntax of llvm-cov) escaped and a trailing "/", so neither
#       a sibling directory whose name starts with the same text nor the same text later in a path
#       matches. When <source-dir> is <build-dir> or lies below it (an in-source build), the
#       <build-dir> part is left out, because it would match every source file. Both directories
#       must be absolute. Only script-mode commands, so it also runs under cmake -P
#       (tests/cmake/CoverageIgnoreRegexTest.cmake).
include_guard(GLOBAL)

set(KYTY_COVERAGE_IGNORE_PATTERN "(3rdparty|_deps|_kyty_patched|/tests/)")

function(kyty_coverage_ignore_regex out build_dir source_dir)
	foreach(kyty_dir IN ITEMS "${build_dir}" "${source_dir}")
		if(NOT IS_ABSOLUTE "${kyty_dir}")
			message(FATAL_ERROR "kyty_coverage_ignore_regex: '${kyty_dir}' is not an absolute path")
		endif()
	endforeach()
	string(REGEX REPLACE "/+$" "" kyty_build "${build_dir}")
	string(REGEX REPLACE "/+$" "" kyty_source "${source_dir}")
	set(kyty_regex "${KYTY_COVERAGE_IGNORE_PATTERN}")
	string(FIND "${kyty_source}/" "${kyty_build}/" kyty_position)
	if(NOT kyty_position EQUAL 0)
		# A backslash before each of \ ^ $ . [ ] | ( ) * + ? { } (in a CMake bracket expression a
		# backslash is an ordinary character, and a leading "]" is part of the set).
		string(REGEX REPLACE [=[([]\[^$.|()*+?{}])]=] [=[\\\1]=] kyty_escaped "${kyty_build}")
		string(APPEND kyty_regex "|^${kyty_escaped}/")
	endif()
	set(${out} "${kyty_regex}" PARENT_SCOPE)
endfunction()
