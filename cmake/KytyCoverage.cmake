# Clang source-based coverage for KytyPS5-GTK (Requirements 17.1, 17.2, 17.12, 17.13). Included from the
# top-level CMakeLists.txt right after kyty_require_lld(), before any dependency is downloaded.
#
#   KYTY_ENABLE_COVERAGE (BOOL, default OFF)
#       Instruments every first-party target that compiles sources, tests included, and adds the
#       coverage target. Needs Clang for C and C++, and llvm-profdata and llvm-cov of Clang's major
#       version (cache entries KYTY_LLVM_PROFDATA and KYTY_LLVM_COV).
#   KYTY_COVERAGE_THREADS (STRING, default 0 = tool default)
#       Passed as -num-threads=<n> to llvm-profdata merge and llvm-cov when not 0. Lower it to cut the
#       memory use of the report steps.
#   kyty_enable_coverage(<target>)
#       Adds -fprofile-instr-generate -fcoverage-mapping to the compile and link options of <target>.
#       Does nothing when KYTY_ENABLE_COVERAGE is OFF.
#   coverage (target)
#       Builds kyty_tests and every first-party executable, then runs cmake/KytyCoverageRun.cmake: the
#       whole test suite with LLVM_PROFILE_FILE set, and the reports in ${CMAKE_BINARY_DIR}/coverage.
#
# At the end of configuration a walker calls kyty_enable_coverage() on every first-party target (see
# kyty_is_first_party_target), so new targets are covered without extra code, and a check fails
# naming every target that compiles a file under src/ without the instrumentation flags. Third-party
# targets are never instrumented.
include_guard(GLOBAL)
include(KytyTargets)

option(KYTY_ENABLE_COVERAGE "Instrument first-party targets with Clang source-based coverage" OFF)
set(KYTY_COVERAGE_THREADS 0 CACHE STRING
	"Threads for llvm-profdata merge and llvm-cov in the coverage target (0 = tool default)")

set(KYTY_COVERAGE_FLAGS -fprofile-instr-generate -fcoverage-mapping)
set(KYTY_COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")
# Every test process writes its raw profile here (kyty_add_test sets it when coverage is ON).
set(KYTY_COVERAGE_PROFILE_FILE "${KYTY_COVERAGE_DIR}/profraw/%m-%p.profraw")

function(kyty_enable_coverage target)
	if(NOT KYTY_ENABLE_COVERAGE)
		return()
	endif()
	get_target_property(kyty_compile_options ${target} COMPILE_OPTIONS)
	if(kyty_compile_options AND "-fcoverage-mapping" IN_LIST kyty_compile_options)
		return()
	endif()
	target_compile_options(${target} PRIVATE ${KYTY_COVERAGE_FLAGS})
	target_link_options(${target} PRIVATE ${KYTY_COVERAGE_FLAGS})
endfunction()

# Sets the cache entry <variable> to <tool> of Clang's LLVM major version. Candidates, in order: the
# cached value when set; otherwise <tool> and <tool>-<major> in the C++ compiler's directory, then
# <tool>-<major> and <tool> on the PATH. A candidate must print "LLVM version <major>." for --version.
function(_kyty_coverage_find_tool tool variable)
	string(REGEX MATCH "^[0-9]+" kyty_major "${CMAKE_CXX_COMPILER_VERSION}")
	set(kyty_candidates "")
	if(${variable})
		list(APPEND kyty_candidates "${${variable}}")
	else()
		get_filename_component(kyty_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
		list(APPEND kyty_candidates "${kyty_compiler_dir}/${tool}" "${kyty_compiler_dir}/${tool}-${kyty_major}")
		foreach(kyty_name IN ITEMS ${tool}-${kyty_major} ${tool})
			unset(_KYTY_COVERAGE_CANDIDATE)
			unset(_KYTY_COVERAGE_CANDIDATE CACHE)
			find_program(_KYTY_COVERAGE_CANDIDATE NAMES ${kyty_name})
			if(_KYTY_COVERAGE_CANDIDATE)
				list(APPEND kyty_candidates "${_KYTY_COVERAGE_CANDIDATE}")
			endif()
			unset(_KYTY_COVERAGE_CANDIDATE)
			unset(_KYTY_COVERAGE_CANDIDATE CACHE)
		endforeach()
		list(REMOVE_DUPLICATES kyty_candidates)
	endif()

	set(kyty_tried "")
	foreach(kyty_candidate IN LISTS kyty_candidates)
		if(NOT EXISTS "${kyty_candidate}" OR IS_DIRECTORY "${kyty_candidate}")
			list(APPEND kyty_tried "${kyty_candidate} (not found)")
			continue()
		endif()
		execute_process(COMMAND "${kyty_candidate}" --version
			RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_output)
		set(kyty_version "")
		if(kyty_result EQUAL 0 AND kyty_output MATCHES "LLVM version ([0-9]+)\\.")
			set(kyty_version "${CMAKE_MATCH_1}")
		endif()
		if(kyty_version STREQUAL kyty_major)
			set(${variable} "${kyty_candidate}" CACHE FILEPATH "${tool} of Clang's LLVM version (coverage target)" FORCE)
			message(STATUS "KytyCoverage: ${tool}: ${kyty_candidate} (LLVM ${kyty_version})")
			return()
		elseif(kyty_version STREQUAL "")
			list(APPEND kyty_tried "${kyty_candidate} (no LLVM version in its --version output)")
		else()
			list(APPEND kyty_tried "${kyty_candidate} (LLVM ${kyty_version})")
		endif()
	endforeach()
	list(JOIN kyty_tried "; " kyty_tried_text)
	message(FATAL_ERROR "KYTY_ENABLE_COVERAGE=ON needs ${tool} from LLVM ${kyty_major} to match Clang "
		"${CMAKE_CXX_COMPILER_VERSION}; tried: ${kyty_tried_text}. Install LLVM ${kyty_major} (Fedora: "
		"dnf install llvm), or set ${variable} to a matching ${tool} (an outdated cache entry can be "
		"cleared with cmake -U${variable} <build-dir>).")
endfunction()

# Appends to <out> the absolute paths of the C and C++ sources that <target> compiles (generator
# expressions are skipped).
function(_kyty_coverage_compiled_sources target out)
	get_target_property(kyty_sources ${target} SOURCES)
	get_target_property(kyty_source_dir ${target} SOURCE_DIR)
	set(kyty_result "")
	foreach(kyty_source IN LISTS kyty_sources)
		if(kyty_source MATCHES "\\$<" OR NOT kyty_source MATCHES "\\.(c|cc|cpp|cxx)$")
			continue()
		endif()
		get_filename_component(kyty_absolute "${kyty_source}" ABSOLUTE BASE_DIR "${kyty_source_dir}")
		list(APPEND kyty_result "${kyty_absolute}")
	endforeach()
	set(${out} "${kyty_result}" PARENT_SCOPE)
endfunction()

# Deferred to the end of the top-level directory: instruments, checks, and adds the coverage target.
function(_kyty_coverage_finalize)
	set(kyty_compiling_types "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
	kyty_collect_targets(kyty_targets)

	set(kyty_instrumented "")
	set(kyty_executables "")
	foreach(kyty_target IN LISTS kyty_targets)
		get_target_property(kyty_type ${kyty_target} TYPE)
		if(NOT kyty_type MATCHES "${kyty_compiling_types}")
			continue()
		endif()
		kyty_is_first_party_target(${kyty_target} kyty_first_party)
		if(NOT kyty_first_party)
			continue()
		endif()
		kyty_enable_coverage(${kyty_target})
		list(APPEND kyty_instrumented ${kyty_target})
		if(kyty_type STREQUAL "EXECUTABLE")
			list(APPEND kyty_executables ${kyty_target})
		endif()
	endforeach()

	# Every target, first-party or not, that compiles a file under src/ must be instrumented
	# (Requirement 17.1).
	set(kyty_missing "")
	foreach(kyty_target IN LISTS kyty_targets)
		get_target_property(kyty_type ${kyty_target} TYPE)
		if(NOT kyty_type MATCHES "${kyty_compiling_types}")
			continue()
		endif()
		_kyty_coverage_compiled_sources(${kyty_target} kyty_sources)
		set(kyty_compiles_src FALSE)
		foreach(kyty_source IN LISTS kyty_sources)
			string(FIND "${kyty_source}" "${CMAKE_SOURCE_DIR}/src/" kyty_position)
			if(kyty_position EQUAL 0)
				set(kyty_compiles_src TRUE)
				break()
			endif()
		endforeach()
		if(NOT kyty_compiles_src)
			continue()
		endif()
		get_target_property(kyty_compile_options ${kyty_target} COMPILE_OPTIONS)
		get_target_property(kyty_link_options ${kyty_target} LINK_OPTIONS)
		foreach(kyty_flag IN LISTS KYTY_COVERAGE_FLAGS)
			if(NOT kyty_compile_options OR NOT kyty_flag IN_LIST kyty_compile_options OR
			   NOT kyty_link_options OR NOT kyty_flag IN_LIST kyty_link_options)
				list(APPEND kyty_missing ${kyty_target})
				break()
			endif()
		endforeach()
	endforeach()
	if(kyty_missing)
		list(JOIN kyty_missing ", " kyty_missing_text)
		list(JOIN KYTY_COVERAGE_FLAGS " " kyty_flags_text)
		message(FATAL_ERROR "KYTY_ENABLE_COVERAGE=ON: these targets compile files under src/ without coverage "
			"instrumentation (${kyty_flags_text}): ${kyty_missing_text}. Call kyty_enable_coverage(<target>) "
			"for each one.")
	endif()

	# The executables that llvm-cov reads, one path per line.
	set(kyty_objects_file "${CMAKE_BINARY_DIR}/kyty_coverage_objects.txt")
	set(kyty_objects "")
	foreach(kyty_executable IN LISTS kyty_executables)
		string(APPEND kyty_objects "$<TARGET_FILE:${kyty_executable}>\n")
	endforeach()
	file(GENERATE OUTPUT "${kyty_objects_file}" CONTENT "${kyty_objects}")

	add_custom_target(coverage
		COMMAND "${CMAKE_COMMAND}"
			"-DCOVERAGE_DIR=${KYTY_COVERAGE_DIR}"
			"-DBUILD_DIR=${CMAKE_BINARY_DIR}"
			"-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
			"-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
			"-DLLVM_PROFDATA=${KYTY_LLVM_PROFDATA}"
			"-DLLVM_COV=${KYTY_LLVM_COV}"
			"-DPYTHON=${Python3_EXECUTABLE}"
			"-DOBJECTS_FILE=${kyty_objects_file}"
			"-DTHREADS=${KYTY_COVERAGE_THREADS}"
			-P "${CMAKE_SOURCE_DIR}/cmake/KytyCoverageRun.cmake"
		COMMENT "Running the test suite with coverage and writing ${KYTY_COVERAGE_DIR}"
		USES_TERMINAL
		VERBATIM)
	if(TARGET kyty_tests)
		add_dependencies(coverage kyty_tests)
	endif()
	if(kyty_executables)
		add_dependencies(coverage ${kyty_executables})
	endif()

	list(LENGTH kyty_instrumented kyty_instrumented_count)
	list(LENGTH kyty_executables kyty_executable_count)
	message(STATUS "KytyCoverage: ${kyty_instrumented_count} first-party targets instrumented "
		"(${kyty_executable_count} executables); build the coverage target for the reports")
endfunction()

if(KYTY_ENABLE_COVERAGE)
	if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		message(FATAL_ERROR "coverage instrumentation requires Clang (found C: ${CMAKE_C_COMPILER_ID}, "
			"C++: ${CMAKE_CXX_COMPILER_ID}); configure with -DCMAKE_C_COMPILER=clang "
			"-DCMAKE_CXX_COMPILER=clang++ or set KYTY_ENABLE_COVERAGE=OFF.")
	endif()
	# The coverage target runs ctest --output-junit.
	if(CMAKE_VERSION VERSION_LESS 3.21)
		message(FATAL_ERROR "KYTY_ENABLE_COVERAGE=ON needs CMake 3.21 or later; found ${CMAKE_VERSION}")
	endif()
	if(NOT KYTY_COVERAGE_THREADS MATCHES "^[0-9]+$")
		message(FATAL_ERROR "KYTY_COVERAGE_THREADS must be a non-negative integer; found '${KYTY_COVERAGE_THREADS}'")
	endif()
	_kyty_coverage_find_tool(llvm-profdata KYTY_LLVM_PROFDATA)
	_kyty_coverage_find_tool(llvm-cov KYTY_LLVM_COV)
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
		message(WARNING "KYTY_ENABLE_COVERAGE=ON with CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}': use Debug. "
			"Release links kyty_emulator with --strip-all, which drops its coverage mapping.")
	endif()
	# report.py runs in the coverage target.
	find_package(Python3 COMPONENTS Interpreter REQUIRED)
	cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _kyty_coverage_finalize)
endif()
