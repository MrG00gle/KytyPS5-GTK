# The coverage target's script (cmake/KytyCoverage.cmake; Requirements 17.2, 17.13). Runs the whole
# test suite of an instrumented build, then writes the reports from the data of that run only:
#
#   cmake -DCOVERAGE_DIR=<build>/coverage -DBUILD_DIR=<build> -DSOURCE_DIR=<repository>
#         -DCTEST_COMMAND=<ctest> -DLLVM_PROFDATA=<llvm-profdata> -DLLVM_COV=<llvm-cov>
#         -DPYTHON=<python3> -DOBJECTS_FILE=<file with one instrumented executable per line>
#         -DTHREADS=<n, 0 = tool default> -P cmake/KytyCoverageRun.cmake
#
# Writes to COVERAGE_DIR: profraw/ (the raw profiles), ctest.xml (CTest JUnit report),
# coverage.profdata, export.json (llvm-cov export -format=text -summary-only), coverage.lcov,
# html/files/ (llvm-cov show -format=html), and summary.json and html/index.html (report.py). The
# llvm-cov outputs leave out the dependencies, the tests, and every file under BUILD_DIR
# (cmake/KytyCoverageFilter.cmake). When a test fails, the reports are still written and the script
# then fails.
cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/KytyCoverageFilter.cmake")

foreach(kyty_variable IN ITEMS COVERAGE_DIR BUILD_DIR SOURCE_DIR CTEST_COMMAND LLVM_PROFDATA LLVM_COV PYTHON
		OBJECTS_FILE THREADS)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "KytyCoverageRun: -D${kyty_variable}=<value> is required")
	endif()
endforeach()
if(NOT THREADS MATCHES "^[0-9]+$")
	message(FATAL_ERROR "KytyCoverageRun: THREADS must be a non-negative integer; found '${THREADS}'")
endif()

# Runs COMMAND and fails naming <description> when it exits non-zero. OUTPUT_FILE receives stdout.
function(_kyty_coverage_step description)
	cmake_parse_arguments(PARSE_ARGV 1 kyty "" "OUTPUT_FILE" "COMMAND")
	set(kyty_output_arguments "")
	if(kyty_OUTPUT_FILE)
		set(kyty_output_arguments OUTPUT_FILE "${kyty_OUTPUT_FILE}")
	endif()
	message(STATUS "KytyCoverageRun: ${description}")
	execute_process(COMMAND ${kyty_COMMAND} ${kyty_output_arguments} RESULT_VARIABLE kyty_result)
	if(NOT kyty_result EQUAL 0)
		message(FATAL_ERROR "KytyCoverageRun: ${description} failed (exit ${kyty_result}; ctest exit "
			"${kyty_ctest_result})")
	endif()
endfunction()

# 1. Only data recorded by this run goes into the reports.
file(REMOVE_RECURSE
	"${COVERAGE_DIR}/profraw"
	"${COVERAGE_DIR}/coverage.profdata"
	"${COVERAGE_DIR}/export.json"
	"${COVERAGE_DIR}/coverage.lcov"
	"${COVERAGE_DIR}/ctest.xml"
	"${COVERAGE_DIR}/html"
	"${COVERAGE_DIR}/summary.json")
file(MAKE_DIRECTORY "${COVERAGE_DIR}/profraw")

# 2. Every test, no filters. kyty_add_test sets the same LLVM_PROFILE_FILE on each test; setting it
# here also covers processes that a test's own environment does not reach.
set(ENV{LLVM_PROFILE_FILE} "${COVERAGE_DIR}/profraw/%m-%p.profraw")
message(STATUS "KytyCoverageRun: running the test suite in ${BUILD_DIR}")
execute_process(
	COMMAND "${CTEST_COMMAND}" --test-dir "${BUILD_DIR}" --output-on-failure
	        --output-junit "${COVERAGE_DIR}/ctest.xml"
	RESULT_VARIABLE kyty_ctest_result)
unset(ENV{LLVM_PROFILE_FILE})
if(kyty_ctest_result EQUAL 0)
	message(STATUS "KytyCoverageRun: every test passed")
else()
	message(STATUS "KytyCoverageRun: ctest exited ${kyty_ctest_result}; writing the reports anyway")
endif()

set(kyty_threads "")
if(NOT THREADS EQUAL 0)
	set(kyty_threads "-num-threads=${THREADS}")
endif()

# 3. Merge the raw profiles.
file(GLOB kyty_profiles "${COVERAGE_DIR}/profraw/*.profraw")
if(NOT kyty_profiles)
	message(FATAL_ERROR "KytyCoverageRun: no raw profile was written to ${COVERAGE_DIR}/profraw (ctest exit "
		"${kyty_ctest_result}); is ${BUILD_DIR} configured with KYTY_ENABLE_COVERAGE=ON?")
endif()
list(LENGTH kyty_profiles kyty_profile_count)
list(JOIN kyty_profiles "\n" kyty_profiles_text)
set(kyty_profile_list "${COVERAGE_DIR}/profraw/inputs.txt")
file(WRITE "${kyty_profile_list}" "${kyty_profiles_text}\n")
_kyty_coverage_step("llvm-profdata merge of ${kyty_profile_count} raw profiles"
	COMMAND "${LLVM_PROFDATA}" merge -sparse ${kyty_threads} "--input-files=${kyty_profile_list}"
	        -o "${COVERAGE_DIR}/coverage.profdata")

# 4. and 5. Exports and the per-file HTML pages, over every instrumented executable.
file(STRINGS "${OBJECTS_FILE}" kyty_objects)
if(NOT kyty_objects)
	message(FATAL_ERROR "KytyCoverageRun: ${OBJECTS_FILE} lists no executable")
endif()
set(kyty_object_arguments "")
foreach(kyty_object IN LISTS kyty_objects)
	if(NOT EXISTS "${kyty_object}")
		message(FATAL_ERROR "KytyCoverageRun: ${kyty_object} (listed in ${OBJECTS_FILE}) does not exist")
	endif()
	if(kyty_object_arguments)
		list(APPEND kyty_object_arguments "-object=${kyty_object}")
	else()
		list(APPEND kyty_object_arguments "${kyty_object}")
	endif()
endforeach()
# Dependencies, tests, and the build tree (generated files, prebuilt headers) stay out of the reports.
kyty_coverage_ignore_regex(kyty_ignore_regex "${BUILD_DIR}" "${SOURCE_DIR}")
set(kyty_cov_arguments
	"-instr-profile=${COVERAGE_DIR}/coverage.profdata"
	"-ignore-filename-regex=${kyty_ignore_regex}"
	${kyty_threads}
	${kyty_object_arguments})
_kyty_coverage_step("llvm-cov export -format=text -summary-only"
	COMMAND "${LLVM_COV}" export -format=text -summary-only -show-branch-summary ${kyty_cov_arguments}
	OUTPUT_FILE "${COVERAGE_DIR}/export.json")
_kyty_coverage_step("llvm-cov export -format=lcov"
	COMMAND "${LLVM_COV}" export -format=lcov ${kyty_cov_arguments}
	OUTPUT_FILE "${COVERAGE_DIR}/coverage.lcov")
_kyty_coverage_step("llvm-cov show -format=html"
	COMMAND "${LLVM_COV}" show -format=html -show-branches=count "-output-dir=${COVERAGE_DIR}/html/files"
	        ${kyty_cov_arguments})

# 6. summary.json and html/index.html.
set(ENV{PYTHONDONTWRITEBYTECODE} 1)
_kyty_coverage_step("tools/coverage/report.py"
	COMMAND "${PYTHON}" -B "${SOURCE_DIR}/tools/coverage/report.py"
	        --export "${COVERAGE_DIR}/export.json"
	        --source-dir "${SOURCE_DIR}"
	        --groups "${SOURCE_DIR}/tools/coverage/groups.json"
	        --output-dir "${COVERAGE_DIR}")

# 7. A failed test fails the coverage target, after the reports are written (Requirement 17.13).
if(NOT kyty_ctest_result EQUAL 0)
	message(FATAL_ERROR "ctest failed (exit ${kyty_ctest_result}); reports written to ${COVERAGE_DIR}")
endif()
message(STATUS "KytyCoverageRun: reports written to ${COVERAGE_DIR}")
