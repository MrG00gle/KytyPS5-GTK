# Test registration for KytyPS5-GTK (Requirements 17.4-17.7).
#
#   kyty_add_test_executable(<target> SOURCES <src>... [LINK <item>...])
#       The only way to create a test executable: EXCLUDE_FROM_ALL, linked with lld, written to
#       ${CMAKE_BINARY_DIR}, and built by the kyty_tests target (which must already exist).
#   kyty_add_test(NAME <name> COMMAND <arg>... [LABELS gpu|ui|property|source-check ...]
#                 [TIMEOUT <1-600, default 120>] [SKIP_RETURN_CODE <0-255>] [ENVIRONMENT <var=value>...]
#                 [ENVIRONMENT_MODIFICATION <var=op:value>...] [WILL_FAIL])
#       The only way to register a CTest test. Rejects bad arguments immediately. Tests run in
#       ${CMAKE_BINARY_DIR}. With KYTY_ENABLE_COVERAGE ON, LLVM_PROFILE_FILE is added to the test's
#       environment (cmake/KytyCoverage.cmake). ENVIRONMENT_MODIFICATION sets the CTest property of
#       that name (for example "DISPLAY=unset:", which ENVIRONMENT cannot express); it needs CMake 3.22
#       or newer, and an older CMake stops the configure instead of ignoring it.
#   kyty_add_test_directory(<dir> [OPTIONAL] LINK <target>... [LABELS ...] [TIMEOUT <s>]
#                           [ENVIRONMENT ...] [ENVIRONMENT_MODIFICATION ...])
#       One executable and one test per <dir>/*Tests.cpp (globbed with CONFIGURE_DEPENDS). <Name>Tests.cpp
#       in tests/<d> becomes test <d>_<snake_case(Name)> (no doubled <d>_ prefix) and executable
#       <test>_tests. Skips with a status message when <dir> has no sources or a LINK item is not a
#       target yet. OPTIONAL marks a directory whose LINK targets exist only in some configurations
#       (tests/launcher_gtk needs kyty_launcher_gtk, which KYTY_BUILD_LAUNCHER=OFF does not define):
#       its skip line ends with "(optional)", and its sources are then exempt from the
#       "not compiled into a registered test executable" check below. Pass it only in the
#       configurations without the targets, so that a missing target elsewhere still fails.
#   kyty_add_property_executable(<target> SOURCES <src>... [LINK <item>...])
#       A RapidCheck property test executable (Requirements 17.8, 17.9): kyty_add_test_executable() of
#       SOURCES plus tests/support/propertyMain.cpp, compiled with -fexceptions -frtti, with tests/ on
#       the include path, and linked with rapidcheck and LINK.
#   kyty_add_property_group(<group> LINK <target>... [ENVIRONMENT_MODIFICATION ...])
#       One property executable <group> from tests/properties/<group>/P<NN>_*.cpp (globbed with
#       CONFIGURE_DEPENDS), and one test <group>_p<NN> with the property label per file, which runs
#       "<group> <NN>". Skips with a status message when the group has no sources or a LINK item is
#       not a target yet.
#   kyty_add_ui_tests(<dir> [OPTIONAL] EXECUTABLE <prefix>_tests PORTAL <target> WRAPPER <script>
#                     BACKENDS <wayland|x11>... [TIMEOUT <s>] [PORTAL_LINK <item>...] LINK <target>...)
#       UI tests (Requirements 16.6, 17.5, 17.10): one executable EXECUTABLE from <dir>/*Tests.cpp and
#       <dir>/support/*.cpp linked with LINK, and one executable PORTAL from <dir>/portal/*.cpp
#       linked with PORTAL_LINK (default: LINK). The cases are the lines that start with
#       KYTY_UI_CASE(<name>) in <dir>/*Tests.cpp (<name>: lowercase letters, digits, and "_", starting
#       with a letter; a malformed or duplicate name stops the configure). Their sorted list goes to
#       ${CMAKE_BINARY_DIR}/tests/<prefix>_cases.txt. Each case gets one test <prefix>_<case>_<backend>
#       per backend, with the ui label, SKIP_RETURN_CODE 77, and TIMEOUT (default 120), which runs
#       "bash WRAPPER --portal <PORTAL> <backend> -- <EXECUTABLE> --case <case>"; the unlabeled test
#       <prefix>_case_list (tests/cmake/UiCaseListTest.cmake) checks that "<EXECUTABLE> --list" names
#       exactly the listed cases, so a case cannot exist without its tests. Skips with a status
#       message when <dir> has no *Tests.cpp or a LINK or PORTAL_LINK item is not a target yet;
#       OPTIONAL as for kyty_add_test_directory (every .cpp below <dir> is then exempt).
#
# End-of-configure checks fail the configure step, listing every violation at once, when a test has no
# TIMEOUT or one outside 1-600, a gpu or ui test has no SKIP_RETURN_CODE, a test was not registered
# with kyty_add_test(), a test executable is not a kyty_tests dependency or is not run by any
# kyty_add_test() COMMAND (Requirement 17.4), a .cpp file under tests/ is not compiled into a
# registered test executable (an executable named by $<TARGET_FILE:...> in a kyty_add_test() COMMAND,
# directly or through a library that only such executables or other tests-only libraries link; the
# sources of a skipped OPTIONAL test directory are exempt), or no executable that compiles it is a
# kyty_tests dependency. An ALIAS counts as the target it names, both in LINK_LIBRARIES and in
# $<TARGET_FILE:...>.
include_guard(GLOBAL)
include(KytyLinker)
include(KytyTargets)

set(KYTY_TEST_LABELS gpu ui property source-check)
set(KYTY_TEST_DEFAULT_TIMEOUT 120)
set(KYTY_TEST_MAX_TIMEOUT 600)

function(kyty_add_test_executable target)
	cmake_parse_arguments(PARSE_ARGV 1 kyty "" "" "SOURCES;LINK")
	if(kyty_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "kyty_add_test_executable(${target}): unexpected arguments: ${kyty_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT kyty_SOURCES)
		message(FATAL_ERROR "kyty_add_test_executable(${target}): SOURCES is required")
	endif()
	if(NOT TARGET kyty_tests)
		message(FATAL_ERROR "kyty_add_test_executable(${target}): the kyty_tests target does not exist yet")
	endif()
	add_executable(${target} EXCLUDE_FROM_ALL ${kyty_SOURCES})
	if(kyty_LINK)
		target_link_libraries(${target} ${kyty_LINK})
	endif()
	kyty_link_with_lld(${target})
	set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
	add_dependencies(kyty_tests ${target})
	set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_EXECUTABLES ${target})
endfunction()

function(kyty_add_test)
	cmake_parse_arguments(PARSE_ARGV 0 kyty "WILL_FAIL" "NAME;TIMEOUT;SKIP_RETURN_CODE"
		"COMMAND;LABELS;ENVIRONMENT;ENVIRONMENT_MODIFICATION")
	if(NOT kyty_NAME)
		message(FATAL_ERROR "kyty_add_test: NAME is required")
	endif()
	set(kyty_context "kyty_add_test(NAME ${kyty_NAME})")
	if(kyty_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "${kyty_context}: unexpected arguments: ${kyty_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT kyty_COMMAND)
		message(FATAL_ERROR "${kyty_context}: COMMAND is required")
	endif()

	if(NOT DEFINED kyty_TIMEOUT)
		set(kyty_TIMEOUT ${KYTY_TEST_DEFAULT_TIMEOUT})
	endif()
	if(NOT kyty_TIMEOUT MATCHES "^[0-9]+$" OR kyty_TIMEOUT LESS 1 OR kyty_TIMEOUT GREATER KYTY_TEST_MAX_TIMEOUT)
		message(FATAL_ERROR "${kyty_context}: TIMEOUT ${kyty_TIMEOUT} is not an integer from 1 to "
			"${KYTY_TEST_MAX_TIMEOUT} seconds")
	endif()
	foreach(kyty_label IN LISTS kyty_LABELS)
		if(NOT kyty_label IN_LIST KYTY_TEST_LABELS)
			list(JOIN KYTY_TEST_LABELS ", " kyty_known_labels)
			message(FATAL_ERROR "${kyty_context}: unknown label ${kyty_label} (known labels: ${kyty_known_labels})")
		endif()
	endforeach()
	if(DEFINED kyty_SKIP_RETURN_CODE)
		if(NOT kyty_SKIP_RETURN_CODE MATCHES "^[0-9]+$" OR kyty_SKIP_RETURN_CODE GREATER 255)
			message(FATAL_ERROR "${kyty_context}: SKIP_RETURN_CODE ${kyty_SKIP_RETURN_CODE} is not an integer "
				"from 0 to 255")
		endif()
	endif()
	if(kyty_ENVIRONMENT_MODIFICATION)
		if(CMAKE_VERSION VERSION_LESS 3.22)
			message(FATAL_ERROR "${kyty_context}: ENVIRONMENT_MODIFICATION needs CMake 3.22 or newer "
				"(this is CMake ${CMAKE_VERSION})")
		endif()
		foreach(kyty_modification IN LISTS kyty_ENVIRONMENT_MODIFICATION)
			if(NOT kyty_modification MATCHES "^[A-Za-z_][A-Za-z0-9_]*=(reset|set|unset|string_append|string_prepend|path_list_append|path_list_prepend|cmake_list_append|cmake_list_prepend):")
				message(FATAL_ERROR "${kyty_context}: ENVIRONMENT_MODIFICATION entry '${kyty_modification}' "
					"is not <variable>=<operation>:<value>")
			endif()
		endforeach()
	endif()

	add_test(NAME ${kyty_NAME} COMMAND ${kyty_COMMAND} WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
	set_tests_properties(${kyty_NAME} PROPERTIES TIMEOUT ${kyty_TIMEOUT})
	if(kyty_LABELS)
		set_tests_properties(${kyty_NAME} PROPERTIES LABELS "${kyty_LABELS}")
	endif()
	if(DEFINED kyty_SKIP_RETURN_CODE)
		set_tests_properties(${kyty_NAME} PROPERTIES SKIP_RETURN_CODE ${kyty_SKIP_RETURN_CODE})
	endif()
	# Coverage builds: every test process writes its raw profile for the coverage target.
	if(KYTY_ENABLE_COVERAGE)
		list(APPEND kyty_ENVIRONMENT "LLVM_PROFILE_FILE=${KYTY_COVERAGE_PROFILE_FILE}")
	endif()
	if(kyty_ENVIRONMENT)
		set_tests_properties(${kyty_NAME} PROPERTIES ENVIRONMENT "${kyty_ENVIRONMENT}")
	endif()
	if(kyty_ENVIRONMENT_MODIFICATION)
		set_tests_properties(${kyty_NAME} PROPERTIES ENVIRONMENT_MODIFICATION "${kyty_ENVIRONMENT_MODIFICATION}")
	endif()
	if(kyty_WILL_FAIL)
		set_tests_properties(${kyty_NAME} PROPERTIES WILL_FAIL TRUE)
	endif()

	# Record the test and the targets its command runs ($<TARGET_FILE:...>) for the final checks.
	set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_TESTS "${CMAKE_CURRENT_SOURCE_DIR}|${kyty_NAME}")
	foreach(kyty_argument IN LISTS kyty_COMMAND)
		string(REGEX MATCHALL "\\$<TARGET_FILE:[^>]+>" kyty_references "${kyty_argument}")
		foreach(kyty_reference IN LISTS kyty_references)
			string(REGEX REPLACE "^\\$<TARGET_FILE:([^>]+)>$" "\\1" kyty_referenced_target "${kyty_reference}")
			set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_COMMAND_TARGETS ${kyty_referenced_target})
		endforeach()
	endforeach()

	# Re-check the properties of this directory's tests once it is fully processed, so a later
	# set_tests_properties() cannot bypass the rules. The top-level directory is checked by
	# _kyty_testing_final_checks.
	if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
		get_property(kyty_scheduled DIRECTORY PROPERTY KYTY_TESTING_DIRECTORY_CHECK_SCHEDULED)
		if(NOT kyty_scheduled)
			set_property(DIRECTORY PROPERTY KYTY_TESTING_DIRECTORY_CHECK_SCHEDULED TRUE)
			cmake_language(DEFER CALL _kyty_testing_directory_check)
		endif()
	endif()
endfunction()

# Converts CamelCase to snake_case: "_" goes between a lowercase letter or digit and an uppercase
# letter, and inside an uppercase run before an uppercase letter that a lowercase one follows.
function(_kyty_testing_snake_case value out)
	string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" kyty_value "${value}")
	string(REGEX REPLACE "([A-Z])([A-Z][a-z])" "\\1_\\2" kyty_value "${kyty_value}")
	string(TOLOWER "${kyty_value}" kyty_value)
	set(${out} "${kyty_value}" PARENT_SCOPE)
endfunction()

function(kyty_add_test_directory dir)
	cmake_parse_arguments(PARSE_ARGV 1 kyty "OPTIONAL" "TIMEOUT" "LINK;LABELS;ENVIRONMENT;ENVIRONMENT_MODIFICATION")
	if(kyty_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "kyty_add_test_directory(${dir}): unexpected arguments: ${kyty_UNPARSED_ARGUMENTS}")
	endif()
	get_filename_component(kyty_dir "${dir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	file(GLOB kyty_sources CONFIGURE_DEPENDS "${kyty_dir}/*Tests.cpp")
	file(RELATIVE_PATH kyty_dir_text "${CMAKE_SOURCE_DIR}" "${kyty_dir}")
	if(NOT kyty_sources)
		message(STATUS "KytyTesting: skipping ${kyty_dir_text}: no *Tests.cpp sources")
		return()
	endif()
	foreach(kyty_item IN LISTS kyty_LINK)
		if(NOT TARGET ${kyty_item})
			if(kyty_OPTIONAL)
				# Every .cpp of the directory, not only the *Tests.cpp ones, is exempt.
				file(GLOB_RECURSE kyty_skipped_sources CONFIGURE_DEPENDS "${kyty_dir}/*.cpp")
				set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_OPTIONAL_SKIPPED_SOURCES ${kyty_skipped_sources})
				message(STATUS "KytyTesting: skipping ${kyty_dir_text}: link target ${kyty_item} does not exist (optional)")
			else()
				message(STATUS "KytyTesting: skipping ${kyty_dir_text}: link target ${kyty_item} does not exist yet")
			endif()
			return()
		endif()
	endforeach()

	set(kyty_test_arguments "")
	if(kyty_LABELS)
		list(APPEND kyty_test_arguments LABELS ${kyty_LABELS})
	endif()
	if(DEFINED kyty_TIMEOUT)
		list(APPEND kyty_test_arguments TIMEOUT ${kyty_TIMEOUT})
	endif()
	if(kyty_ENVIRONMENT)
		list(APPEND kyty_test_arguments ENVIRONMENT ${kyty_ENVIRONMENT})
	endif()
	if(kyty_ENVIRONMENT_MODIFICATION)
		list(APPEND kyty_test_arguments ENVIRONMENT_MODIFICATION ${kyty_ENVIRONMENT_MODIFICATION})
	endif()

	get_filename_component(kyty_prefix "${kyty_dir}" NAME)
	list(SORT kyty_sources)
	foreach(kyty_source IN LISTS kyty_sources)
		get_filename_component(kyty_file_name "${kyty_source}" NAME)
		string(REGEX REPLACE "Tests\\.cpp$" "" kyty_base_name "${kyty_file_name}")
		_kyty_testing_snake_case("${kyty_base_name}" kyty_snake)
		string(FIND "${kyty_snake}" "${kyty_prefix}_" kyty_prefix_position)
		if(kyty_snake STREQUAL kyty_prefix OR kyty_prefix_position EQUAL 0)
			set(kyty_test_name "${kyty_snake}")
		else()
			set(kyty_test_name "${kyty_prefix}_${kyty_snake}")
		endif()
		kyty_add_test_executable(${kyty_test_name}_tests SOURCES "${kyty_source}" LINK ${kyty_LINK})
		kyty_add_test(NAME ${kyty_test_name} COMMAND $<TARGET_FILE:${kyty_test_name}_tests> ${kyty_test_arguments})
	endforeach()
endfunction()

function(kyty_add_property_executable target)
	cmake_parse_arguments(PARSE_ARGV 1 kyty "" "" "SOURCES;LINK")
	if(kyty_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "kyty_add_property_executable(${target}): unexpected arguments: ${kyty_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT kyty_SOURCES)
		message(FATAL_ERROR "kyty_add_property_executable(${target}): SOURCES is required")
	endif()
	if(NOT TARGET rapidcheck)
		message(FATAL_ERROR "kyty_add_property_executable(${target}): the rapidcheck target does not exist "
			"(3rdparty/CMakeLists.txt adds it when BUILD_TESTING is ON)")
	endif()
	kyty_add_test_executable(${target}
		SOURCES ${kyty_SOURCES} "${CMAKE_SOURCE_DIR}/tests/support/propertyMain.cpp"
		LINK rapidcheck ${kyty_LINK})
	target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/tests")
	target_compile_options(${target} PRIVATE -fexceptions -frtti)
endfunction()

function(kyty_add_property_group group)
	cmake_parse_arguments(PARSE_ARGV 1 kyty "" "" "LINK;ENVIRONMENT_MODIFICATION")
	if(kyty_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "kyty_add_property_group(${group}): unexpected arguments: ${kyty_UNPARSED_ARGUMENTS}")
	endif()
	set(kyty_dir_text "tests/properties/${group}")
	file(GLOB kyty_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${kyty_dir_text}/P[0-9][0-9]_*.cpp")
	if(NOT kyty_sources)
		message(STATUS "KytyTesting: skipping property group ${group}: no ${kyty_dir_text}/P<NN>_*.cpp sources")
		return()
	endif()
	foreach(kyty_item IN LISTS kyty_LINK)
		if(NOT TARGET ${kyty_item})
			message(STATUS "KytyTesting: skipping property group ${group}: link target ${kyty_item} does not exist yet")
			return()
		endif()
	endforeach()

	set(kyty_test_arguments "")
	if(kyty_ENVIRONMENT_MODIFICATION)
		list(APPEND kyty_test_arguments ENVIRONMENT_MODIFICATION ${kyty_ENVIRONMENT_MODIFICATION})
	endif()

	list(SORT kyty_sources)
	kyty_add_property_executable(${group} SOURCES ${kyty_sources} LINK ${kyty_LINK})
	foreach(kyty_source IN LISTS kyty_sources)
		get_filename_component(kyty_file_name "${kyty_source}" NAME)
		string(REGEX REPLACE "^P([0-9][0-9])_.*$" "\\1" kyty_number "${kyty_file_name}")
		kyty_add_test(NAME ${group}_p${kyty_number} COMMAND $<TARGET_FILE:${group}> ${kyty_number}
			LABELS property ${kyty_test_arguments})
	endforeach()
endfunction()

function(kyty_add_ui_tests dir)
	cmake_parse_arguments(PARSE_ARGV 1 kyty "OPTIONAL" "EXECUTABLE;PORTAL;WRAPPER;TIMEOUT"
		"BACKENDS;PORTAL_LINK;LINK")
	set(kyty_context "kyty_add_ui_tests(${dir})")
	if(kyty_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "${kyty_context}: unexpected arguments: ${kyty_UNPARSED_ARGUMENTS}")
	endif()
	foreach(kyty_required IN ITEMS EXECUTABLE PORTAL WRAPPER BACKENDS LINK)
		if(NOT kyty_${kyty_required})
			message(FATAL_ERROR "${kyty_context}: ${kyty_required} is required")
		endif()
	endforeach()
	if(NOT kyty_EXECUTABLE MATCHES "^([a-z][a-z0-9_]*)_tests$")
		message(FATAL_ERROR "${kyty_context}: EXECUTABLE ${kyty_EXECUTABLE} does not end with _tests")
	endif()
	set(kyty_prefix "${CMAKE_MATCH_1}")
	foreach(kyty_backend IN LISTS kyty_BACKENDS)
		if(NOT kyty_backend MATCHES "^(wayland|x11)$")
			message(FATAL_ERROR "${kyty_context}: unknown backend ${kyty_backend} (known: wayland, x11)")
		endif()
	endforeach()
	if(NOT DEFINED kyty_TIMEOUT)
		set(kyty_TIMEOUT ${KYTY_TEST_DEFAULT_TIMEOUT})
	endif()
	if(NOT DEFINED kyty_PORTAL_LINK)
		set(kyty_PORTAL_LINK ${kyty_LINK})
	endif()
	get_filename_component(kyty_wrapper "${kyty_WRAPPER}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	if(NOT EXISTS "${kyty_wrapper}")
		message(FATAL_ERROR "${kyty_context}: WRAPPER ${kyty_wrapper} does not exist")
	endif()

	get_filename_component(kyty_dir "${dir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	file(RELATIVE_PATH kyty_dir_text "${CMAKE_SOURCE_DIR}" "${kyty_dir}")
	file(GLOB kyty_case_sources CONFIGURE_DEPENDS "${kyty_dir}/*Tests.cpp")
	file(GLOB kyty_support_sources CONFIGURE_DEPENDS "${kyty_dir}/support/*.cpp")
	file(GLOB kyty_portal_sources CONFIGURE_DEPENDS "${kyty_dir}/portal/*.cpp")
	if(NOT kyty_case_sources)
		message(STATUS "KytyTesting: skipping ${kyty_dir_text}: no *Tests.cpp sources")
		return()
	endif()
	if(NOT kyty_portal_sources)
		message(FATAL_ERROR "${kyty_context}: no ${kyty_dir_text}/portal/*.cpp sources for ${kyty_PORTAL}")
	endif()
	foreach(kyty_item IN LISTS kyty_LINK kyty_PORTAL_LINK)
		if(NOT TARGET ${kyty_item})
			if(kyty_OPTIONAL)
				file(GLOB_RECURSE kyty_skipped_sources CONFIGURE_DEPENDS "${kyty_dir}/*.cpp")
				set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_OPTIONAL_SKIPPED_SOURCES ${kyty_skipped_sources})
				message(STATUS "KytyTesting: skipping ${kyty_dir_text}: link target ${kyty_item} does not exist (optional)")
			else()
				message(STATUS "KytyTesting: skipping ${kyty_dir_text}: link target ${kyty_item} does not exist yet")
			endif()
			return()
		endif()
	endforeach()
	find_program(KYTY_UI_TESTS_BASH bash REQUIRED)

	# The cases, from the KYTY_UI_CASE(<name>) lines.
	list(SORT kyty_case_sources)
	set(kyty_cases "")
	foreach(kyty_source IN LISTS kyty_case_sources)
		file(RELATIVE_PATH kyty_source_text "${CMAKE_SOURCE_DIR}" "${kyty_source}")
		file(STRINGS "${kyty_source}" kyty_case_lines REGEX "^KYTY_UI_CASE")
		foreach(kyty_line IN LISTS kyty_case_lines)
			if(NOT kyty_line MATCHES "^KYTY_UI_CASE\\(([a-z][a-z0-9_]*)\\)")
				message(FATAL_ERROR "${kyty_context}: ${kyty_source_text}: malformed case line '${kyty_line}' "
					"(expected KYTY_UI_CASE(<name>) with a name of lowercase letters, digits, and _)")
			endif()
			if(CMAKE_MATCH_1 IN_LIST kyty_cases)
				message(FATAL_ERROR "${kyty_context}: ${kyty_source_text}: duplicate case ${CMAKE_MATCH_1}")
			endif()
			list(APPEND kyty_cases "${CMAKE_MATCH_1}")
		endforeach()
	endforeach()
	list(SORT kyty_cases)
	list(JOIN kyty_cases "\n" kyty_cases_text)
	if(kyty_cases)
		string(APPEND kyty_cases_text "\n")
	endif()
	set(kyty_cases_file "${CMAKE_BINARY_DIR}/tests/${kyty_prefix}_cases.txt")
	file(CONFIGURE OUTPUT "${kyty_cases_file}" CONTENT "${kyty_cases_text}" @ONLY)

	kyty_add_test_executable(${kyty_EXECUTABLE} SOURCES ${kyty_case_sources} ${kyty_support_sources}
		LINK ${kyty_LINK})
	kyty_add_test_executable(${kyty_PORTAL} SOURCES ${kyty_portal_sources} LINK ${kyty_PORTAL_LINK})
	foreach(kyty_case IN LISTS kyty_cases)
		foreach(kyty_backend IN LISTS kyty_BACKENDS)
			kyty_add_test(NAME ${kyty_prefix}_${kyty_case}_${kyty_backend}
				COMMAND "${KYTY_UI_TESTS_BASH}" "${kyty_wrapper}" --portal $<TARGET_FILE:${kyty_PORTAL}>
				        ${kyty_backend} -- $<TARGET_FILE:${kyty_EXECUTABLE}> --case ${kyty_case}
				LABELS ui
				SKIP_RETURN_CODE 77
				TIMEOUT ${kyty_TIMEOUT})
		endforeach()
	endforeach()
	# --list needs no display.
	kyty_add_test(NAME ${kyty_prefix}_case_list
		COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:${kyty_EXECUTABLE}>" "-DCASES=${kyty_cases_file}"
		        -P "${CMAKE_SOURCE_DIR}/tests/cmake/UiCaseListTest.cmake"
		TIMEOUT 60
		ENVIRONMENT_MODIFICATION WAYLAND_DISPLAY=unset: DISPLAY=unset:)
	list(LENGTH kyty_cases kyty_case_count)
	list(LENGTH kyty_BACKENDS kyty_backend_count)
	math(EXPR kyty_ui_test_count "${kyty_case_count} * ${kyty_backend_count}")
	list(JOIN kyty_BACKENDS ", " kyty_backends_text)
	message(STATUS "KytyTesting: ${kyty_dir_text}: ${kyty_case_count} UI cases, ${kyty_ui_test_count} ui tests "
		"(${kyty_backends_text})")
endfunction()

# Appends a violation for every test of the current directory with a bad TIMEOUT or a gpu or ui label
# without SKIP_RETURN_CODE.
function(_kyty_testing_check_current_directory_tests)
	file(RELATIVE_PATH kyty_dir_text "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
	if(kyty_dir_text STREQUAL "")
		set(kyty_dir_text ".")
	endif()
	get_property(kyty_tests DIRECTORY PROPERTY TESTS)
	foreach(kyty_test IN LISTS kyty_tests)
		set(kyty_context "test ${kyty_test} (${kyty_dir_text})")
		get_test_property(${kyty_test} TIMEOUT kyty_timeout)
		if(NOT kyty_timeout)
			set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS "${kyty_context}: no TIMEOUT")
		elseif(NOT kyty_timeout MATCHES "^[0-9]+$" OR kyty_timeout LESS 1 OR kyty_timeout GREATER KYTY_TEST_MAX_TIMEOUT)
			set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS
				"${kyty_context}: TIMEOUT ${kyty_timeout} is not an integer from 1 to ${KYTY_TEST_MAX_TIMEOUT} seconds")
		endif()
		get_test_property(${kyty_test} LABELS kyty_labels)
		get_test_property(${kyty_test} SKIP_RETURN_CODE kyty_skip_return_code)
		foreach(kyty_label IN ITEMS gpu ui)
			if(kyty_labels AND kyty_label IN_LIST kyty_labels AND
			   (NOT DEFINED kyty_skip_return_code OR kyty_skip_return_code STREQUAL "NOTFOUND"))
				set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS
					"${kyty_context}: label ${kyty_label} requires SKIP_RETURN_CODE")
			endif()
		endforeach()
	endforeach()
endfunction()

function(_kyty_testing_directory_check)
	_kyty_testing_check_current_directory_tests()
endfunction()

# Sets <out> to the absolute paths of the sources that <target> compiles (generator expressions are
# skipped).
function(_kyty_testing_target_sources target out)
	get_target_property(kyty_sources ${target} SOURCES)
	get_target_property(kyty_source_dir ${target} SOURCE_DIR)
	set(kyty_result "")
	foreach(kyty_source IN LISTS kyty_sources)
		if(kyty_source MATCHES "\\$<")
			continue()
		endif()
		get_filename_component(kyty_absolute "${kyty_source}" ABSOLUTE BASE_DIR "${kyty_source_dir}")
		list(APPEND kyty_result "${kyty_absolute}")
	endforeach()
	set(${out} "${kyty_result}" PARENT_SCOPE)
endfunction()

# Sets <out> to the target that an ALIAS <item> names, else to <item>.
function(_kyty_testing_real_target item out)
	set(kyty_real "${item}")
	if(TARGET "${item}")
		get_target_property(kyty_aliased "${item}" ALIASED_TARGET)
		if(kyty_aliased)
			set(kyty_real "${kyty_aliased}")
		endif()
	endif()
	set(${out} "${kyty_real}" PARENT_SCOPE)
endfunction()

function(_kyty_testing_final_checks)
	_kyty_testing_check_current_directory_tests()

	get_property(kyty_registered GLOBAL PROPERTY KYTY_TESTING_TESTS)
	get_property(kyty_executables GLOBAL PROPERTY KYTY_TESTING_EXECUTABLES)
	get_property(kyty_command_target_items GLOBAL PROPERTY KYTY_TESTING_COMMAND_TARGETS)
	set(kyty_command_targets "")
	foreach(kyty_item IN LISTS kyty_command_target_items)
		_kyty_testing_real_target("${kyty_item}" kyty_real)
		list(APPEND kyty_command_targets "${kyty_real}")
	endforeach()
	if(kyty_command_targets)
		list(REMOVE_DUPLICATES kyty_command_targets)
	endif()

	# Every test of every directory must come from kyty_add_test().
	kyty_collect_directories(kyty_directories)
	set(kyty_test_count 0)
	foreach(kyty_directory IN LISTS kyty_directories)
		get_property(kyty_tests DIRECTORY "${kyty_directory}" PROPERTY TESTS)
		file(RELATIVE_PATH kyty_dir_text "${CMAKE_SOURCE_DIR}" "${kyty_directory}")
		if(kyty_dir_text STREQUAL "")
			set(kyty_dir_text ".")
		endif()
		foreach(kyty_test IN LISTS kyty_tests)
			math(EXPR kyty_test_count "${kyty_test_count} + 1")
			if(NOT "${kyty_directory}|${kyty_test}" IN_LIST kyty_registered)
				set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS
					"test ${kyty_test} (${kyty_dir_text}): not registered with kyty_add_test()")
			endif()
		endforeach()
	endforeach()

	# Test executables created by kyty_add_test_executable() are kyty_tests dependencies by
	# construction; check it anyway.
	set(kyty_tests_dependencies "")
	if(TARGET kyty_tests)
		get_target_property(kyty_tests_dependencies kyty_tests MANUALLY_ADDED_DEPENDENCIES)
		if(NOT kyty_tests_dependencies)
			set(kyty_tests_dependencies "")
		endif()
	endif()
	foreach(kyty_executable IN LISTS kyty_executables)
		if(NOT kyty_executable IN_LIST kyty_tests_dependencies)
			set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS
				"test executable ${kyty_executable}: not a kyty_tests dependency")
		endif()
		if(NOT kyty_executable IN_LIST kyty_command_targets)
			set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS
				"test executable ${kyty_executable}: not run by any kyty_add_test() COMMAND (Requirement 17.4)")
		endif()
	endforeach()

	set(kyty_cpp_note "")
	if(BUILD_TESTING)
		# Registered test executables: targets that a kyty_add_test() COMMAND runs.
		set(kyty_registered_executables "")
		foreach(kyty_target IN LISTS kyty_command_targets)
			if(TARGET ${kyty_target})
				get_target_property(kyty_type ${kyty_target} TYPE)
				if(kyty_type STREQUAL "EXECUTABLE")
					list(APPEND kyty_registered_executables ${kyty_target})
				endif()
			endif()
		endforeach()

		# Who links each first-party library.
		kyty_collect_targets(kyty_targets)
		set(kyty_libraries "")
		foreach(kyty_target IN LISTS kyty_targets)
			kyty_is_first_party_target(${kyty_target} kyty_first_party)
			if(NOT kyty_first_party)
				continue()
			endif()
			get_target_property(kyty_type ${kyty_target} TYPE)
			if(kyty_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
				list(APPEND kyty_libraries ${kyty_target})
			endif()
			if(NOT kyty_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
				continue()
			endif()
			get_target_property(kyty_links ${kyty_target} LINK_LIBRARIES)
			if(NOT kyty_links)
				continue()
			endif()
			foreach(kyty_link_item IN LISTS kyty_links)
				_kyty_testing_real_target("${kyty_link_item}" kyty_link)
				if(TARGET "${kyty_link}")
					string(MD5 kyty_key "${kyty_link}")
					list(APPEND kyty_consumers_${kyty_key} ${kyty_target})
				endif()
			endforeach()
		endforeach()

		# Tests-only libraries: linked only by registered test executables or other tests-only
		# libraries.
		set(kyty_tests_only "")
		set(kyty_changed TRUE)
		while(kyty_changed)
			set(kyty_changed FALSE)
			foreach(kyty_library IN LISTS kyty_libraries)
				if(kyty_library IN_LIST kyty_tests_only)
					continue()
				endif()
				string(MD5 kyty_key "${kyty_library}")
				if(NOT kyty_consumers_${kyty_key})
					continue()
				endif()
				set(kyty_only_tests TRUE)
				foreach(kyty_consumer IN LISTS kyty_consumers_${kyty_key})
					if(NOT kyty_consumer IN_LIST kyty_registered_executables AND NOT kyty_consumer IN_LIST kyty_tests_only)
						set(kyty_only_tests FALSE)
						break()
					endif()
				endforeach()
				if(kyty_only_tests)
					list(APPEND kyty_tests_only ${kyty_library})
					set(kyty_changed TRUE)
				endif()
			endforeach()
		endwhile()

		# For every source, the registered test executables that compile it.
		foreach(kyty_executable IN LISTS kyty_registered_executables)
			set(kyty_compiled_by_executable "")
			_kyty_testing_target_sources(${kyty_executable} kyty_sources)
			list(APPEND kyty_compiled_by_executable ${kyty_sources})
			get_target_property(kyty_pending ${kyty_executable} LINK_LIBRARIES)
			if(NOT kyty_pending)
				set(kyty_pending "")
			endif()
			set(kyty_visited "")
			while(kyty_pending)
				list(POP_FRONT kyty_pending kyty_library_item)
				_kyty_testing_real_target("${kyty_library_item}" kyty_library)
				if(NOT kyty_library IN_LIST kyty_tests_only OR kyty_library IN_LIST kyty_visited)
					continue()
				endif()
				list(APPEND kyty_visited ${kyty_library})
				_kyty_testing_target_sources(${kyty_library} kyty_sources)
				list(APPEND kyty_compiled_by_executable ${kyty_sources})
				get_target_property(kyty_library_links ${kyty_library} LINK_LIBRARIES)
				if(kyty_library_links)
					list(APPEND kyty_pending ${kyty_library_links})
				endif()
			endwhile()
			foreach(kyty_source IN LISTS kyty_compiled_by_executable)
				string(MD5 kyty_key "${kyty_source}")
				list(APPEND kyty_compiled_by_${kyty_key} ${kyty_executable})
			endforeach()
		endforeach()

		file(GLOB_RECURSE kyty_test_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*.cpp")
		get_property(kyty_optional_skipped GLOBAL PROPERTY KYTY_TESTING_OPTIONAL_SKIPPED_SOURCES)
		if(kyty_optional_skipped)
			list(REMOVE_ITEM kyty_test_sources ${kyty_optional_skipped})
		endif()
		list(SORT kyty_test_sources)
		foreach(kyty_source IN LISTS kyty_test_sources)
			file(RELATIVE_PATH kyty_source_text "${CMAKE_SOURCE_DIR}" "${kyty_source}")
			string(MD5 kyty_key "${kyty_source}")
			if(NOT kyty_compiled_by_${kyty_key})
				set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS
					"${kyty_source_text}: not compiled into a registered test executable")
				continue()
			endif()
			set(kyty_built_by_kyty_tests FALSE)
			foreach(kyty_executable IN LISTS kyty_compiled_by_${kyty_key})
				if(kyty_executable IN_LIST kyty_tests_dependencies)
					set(kyty_built_by_kyty_tests TRUE)
				endif()
			endforeach()
			if(NOT kyty_built_by_kyty_tests)
				list(REMOVE_DUPLICATES kyty_compiled_by_${kyty_key})
				list(JOIN kyty_compiled_by_${kyty_key} ", " kyty_executables_text)
				set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_VIOLATIONS
					"${kyty_source_text}: compiled only into ${kyty_executables_text}, which kyty_tests does not build")
			endif()
		endforeach()
		set(kyty_cpp_note ", every tests/*.cpp compiled")
	endif()

	get_property(kyty_violations GLOBAL PROPERTY KYTY_TESTING_VIOLATIONS)
	if(kyty_violations)
		list(LENGTH kyty_violations kyty_violation_count)
		string(REPLACE ";" "\n  " kyty_violations_text "${kyty_violations}")
		message(FATAL_ERROR "KytyTesting: ${kyty_violation_count} test registration violation(s):\n  "
			"${kyty_violations_text}")
	endif()
	list(LENGTH kyty_executables kyty_executable_count)
	if(BUILD_TESTING)
		message(STATUS "KytyTesting: ${kyty_test_count} tests, ${kyty_executable_count} test executables${kyty_cpp_note}")
	else()
		message(STATUS "KytyTesting: BUILD_TESTING is OFF; ${kyty_test_count} tests registered")
	endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _kyty_testing_final_checks)
