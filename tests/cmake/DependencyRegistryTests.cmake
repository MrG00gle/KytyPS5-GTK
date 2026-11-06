# Dependency registry self-tests (cmake/KytyDependencies.cmake and cmake/KytyTargets.cmake), one
# CASE per CTest dependency_registry_<case>:
#   hidden_entries      In a prefetched directory, opus/ holds only .git/ and the zydis submodule
#                       dependencies/zycore only a .git file. Both count as missing: configure stops
#                       naming opus and zydis before any FetchContent call (no _deps directory).
#   patched_root        A patched entry without SOURCE_SUBDIR, fetched from a local repository: the
#                       top-level CMakeLists.txt of the patched copy is added, so its target exists,
#                       comes from the copy, and has the patch applied.
#   online_after_prefetched
#                       A configure from a prefetched directory, then the directory is deleted and
#                       the same build directory is configured with KYTY_PREFETCHED_SOURCES_DIR
#                       cleared: the entry is fetched from its local repository, and the cache no
#                       longer names the prefetched directory.
#   prefetched_not_first_party
#                       A prefetched directory inside the project, given as a relative path: the
#                       target it adds is not first-party (kyty_is_first_party_target), and a target
#                       of the project is.
# Each case writes a project (LANGUAGES NONE) to WORK_DIR/project that includes the two modules from
# MODULE_DIR, with a copy of PATCHES_DIR as 3rdparty/patches so that the registry's table validates,
# and configures it in WORK_DIR/build. Its sources come from local git repositories (file://, made
# with GIT_EXECUTABLE) or from prefetched directories with stand-in files for the table's entries.
# GIT_ALLOW_PROTOCOL=file makes every other git transport fail, so no case makes a network request.
# MODULE_DIR may name another copy of the modules, for example those of an older revision. WORK_DIR
# is deleted when the case passes and kept for inspection when it fails.
#
#   cmake -DCASE=<case> -DMODULE_DIR=<source>/cmake -DPATCHES_DIR=<source>/3rdparty/patches
#         -DGIT_EXECUTABLE=<git> -DGENERATOR=<generator> -DMAKE_PROGRAM=<program> -DWORK_DIR=<dir>
#         -P tests/cmake/DependencyRegistryTests.cmake
cmake_minimum_required(VERSION 3.22)

foreach(kyty_variable IN ITEMS CASE MODULE_DIR PATCHES_DIR GIT_EXECUTABLE GENERATOR MAKE_PROGRAM
		WORK_DIR)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "DependencyRegistryTests: -D${kyty_variable}=<value> is required")
	endif()
endforeach()
foreach(kyty_module IN ITEMS KytyDependencies KytyTargets)
	if(NOT EXISTS "${MODULE_DIR}/${kyty_module}.cmake")
		message(FATAL_ERROR "DependencyRegistryTests: ${MODULE_DIR}/${kyty_module}.cmake does not "
			"exist")
	endif()
endforeach()
if(NOT IS_DIRECTORY "${PATCHES_DIR}")
	message(FATAL_ERROR "DependencyRegistryTests: PATCHES_DIR ${PATCHES_DIR} is not a directory")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
set(kyty_project "${WORK_DIR}/project")
set(kyty_build "${WORK_DIR}/build")
set(kyty_repos "${WORK_DIR}/repos")
file(MAKE_DIRECTORY "${kyty_project}" "${kyty_repos}")
file(COPY "${PATCHES_DIR}/" DESTINATION "${kyty_project}/3rdparty/patches")
set(ENV{GIT_ALLOW_PROTOCOL} "file")
set(kyty_failures "")
set(kyty_logs "")

# Runs git with the remaining arguments in <dir> and sets kyty_git_output to its standard output;
# stops the test when git fails.
function(kyty_registry_git dir)
	execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" -c user.name=kyty
		-c user.email=kyty@localhost -c commit.gpgsign=false ${ARGN}
		RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_error)
	if(NOT kyty_result EQUAL 0)
		message(FATAL_ERROR "DependencyRegistryTests ${CASE}: git ${ARGN} in ${dir} failed "
			"(${kyty_result}):\n${kyty_output}${kyty_error}")
	endif()
	set(kyty_git_output "${kyty_output}" PARENT_SCOPE)
endfunction()

# Creates the repository kyty_repos/<name> with one commit holding CMakeLists.txt = <content>, and
# sets <out> to the commit.
function(kyty_registry_repo name content out)
	set(kyty_repo "${kyty_repos}/${name}")
	file(WRITE "${kyty_repo}/CMakeLists.txt" "${content}")
	kyty_registry_git("${kyty_repo}" init -q)
	kyty_registry_git("${kyty_repo}" add CMakeLists.txt)
	kyty_registry_git("${kyty_repo}" commit -q -m "${name}")
	kyty_registry_git("${kyty_repo}" rev-parse HEAD)
	string(STRIP "${kyty_git_output}" kyty_commit)
	set(${out} "${kyty_commit}" PARENT_SCOPE)
endfunction()

# Fills <farm> with a stand-in for every entry of the registry's table that is active with
# BUILD_TESTING unset: a README.md in each tree and in the zydis submodule, and the FFmpeg archive.
# No case adds these trees, so their contents do not matter.
function(kyty_registry_farm farm)
	foreach(kyty_entry IN ITEMS opus openxr xbyak zydis zydis/dependencies/zycore zstd
			zarchive_source)
		file(WRITE "${farm}/${kyty_entry}/README.md" "Stand-in for ${kyty_entry}.\n")
	endforeach()
	file(WRITE "${farm}/ffmpeg_core_prebuilt/ffmpeg-linux-x64.zip" "Stand-in archive.\n")
endfunction()

# Writes the project: the two modules, then <body> with its @variables@ replaced.
function(kyty_registry_project body)
	string(CONFIGURE "${body}" kyty_body @ONLY)
	file(WRITE "${kyty_project}/CMakeLists.txt"
		"cmake_minimum_required(VERSION 3.22...4.3)\n"
		"project(kyty_dependency_registry_${CASE} LANGUAGES NONE)\n"
		"list(APPEND CMAKE_MODULE_PATH [==[${MODULE_DIR}]==])\n"
		"include(KytyDependencies)\n"
		"include(KytyTargets)\n"
		"${kyty_body}")
endfunction()

# Configures the project in kyty_build with the remaining arguments, from WORK_DIR. Sets
# kyty_result, kyty_output, and kyty_normalized (every whitespace run made one space, because CMake
# wraps long error messages); the output is also written to WORK_DIR/<label>.log.
macro(kyty_registry_configure label)
	execute_process(COMMAND "${CMAKE_COMMAND}" -S "${kyty_project}" -B "${kyty_build}"
		-G "${GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
		"-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" ${ARGN}
		WORKING_DIRECTORY "${WORK_DIR}"
		RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_output)
	file(WRITE "${WORK_DIR}/${label}.log" "${kyty_output}")
	string(REGEX REPLACE "[ \t\r\n]+" " " kyty_normalized "${kyty_output}")
	string(APPEND kyty_logs "\n--- ${label} (exit ${kyty_result}) ---\n${kyty_output}")
	message(STATUS "DependencyRegistryTests ${CASE}: ${label}: exit '${kyty_result}'")
	string(REGEX MATCHALL "dependency_registry: [^\n]*" kyty_reports "${kyty_output}")
	string(REGEX MATCH "CMake Error[^\n]*(\n[^\n]*)(\n[^\n]*)(\n[^\n]*)(\n[^\n]*)" kyty_error
		"${kyty_output}")
	foreach(kyty_report IN LISTS kyty_reports)
		message(STATUS "DependencyRegistryTests ${CASE}: ${label}: ${kyty_report}")
	endforeach()
	if(kyty_error)
		message(STATUS "DependencyRegistryTests ${CASE}: ${label}:\n${kyty_error}")
	endif()
endmacro()

# <expected> is success or failure.
macro(kyty_expect_exit label expected)
	if("${expected}" STREQUAL "success" AND NOT kyty_result STREQUAL "0")
		list(APPEND kyty_failures "${label}: configure exited with '${kyty_result}', expected 0")
	elseif("${expected}" STREQUAL "failure"
	       AND (NOT kyty_result MATCHES "^[0-9]+$" OR kyty_result EQUAL 0))
		list(APPEND kyty_failures
			"${label}: configure exited with '${kyty_result}', expected a failure")
	endif()
endmacro()

macro(kyty_expect_text label text)
	string(FIND "${kyty_normalized}" "${text}" kyty_position)
	if(kyty_position EQUAL -1)
		list(APPEND kyty_failures "${label}: the output lacks the text: ${text}")
	endif()
endmacro()

# The project reports what it found as "dependency_registry: <key>=<value>" status lines.
macro(kyty_expect_report label key value)
	string(FIND "${kyty_output}" "-- dependency_registry: ${key}=${value}\n" kyty_position)
	if(kyty_position EQUAL -1)
		string(REGEX MATCH "dependency_registry: ${key}=[^\n]*" kyty_got "${kyty_output}")
		if(NOT kyty_got)
			set(kyty_got "no report")
		endif()
		list(APPEND kyty_failures
			"${label}: expected dependency_registry: ${key}=${value}, got: ${kyty_got}")
	endif()
endmacro()

if(CASE STREQUAL "hidden_entries")
	set(kyty_farm "${WORK_DIR}/prefetched")
	set(kyty_zycore "${kyty_farm}/zydis/dependencies/zycore")
	kyty_registry_farm("${kyty_farm}")
	# An uninitialized clone and an uninitialized submodule: nothing but their .git entry.
	file(REMOVE "${kyty_farm}/opus/README.md" "${kyty_zycore}/README.md")
	file(WRITE "${kyty_farm}/opus/.git/HEAD" "ref: refs/heads/main\n")
	file(WRITE "${kyty_zycore}/.git" "gitdir: ../../.git/modules/dependencies/zycore\n")
	kyty_registry_project([===[
kyty_check_prefetched_sources()
message(STATUS "dependency_registry: check=passed")
]===])
	kyty_registry_configure(configure "-DKYTY_PREFETCHED_SOURCES_DIR=${kyty_farm}")
	kyty_expect_exit(configure failure)
	string(CONCAT kyty_expected "KYTY_PREFETCHED_SOURCES_DIR=${kyty_farm} lacks the sources of "
		"2 of 7 dependencies: opus, zydis.")
	kyty_expect_text(configure "${kyty_expected}")
	kyty_expect_text(configure "opus: ${kyty_farm}/opus (")
	kyty_expect_text(configure "zydis: ${kyty_zycore} (submodule dependencies/zycore of")
	if(EXISTS "${kyty_build}/_deps")
		list(APPEND kyty_failures
			"configure: ${kyty_build}/_deps exists, expected no FetchContent call")
	endif()
elseif(CASE STREQUAL "patched_root")
	set(kyty_repo "${kyty_repos}/registry_patched_root")
	set(kyty_original [===[
cmake_minimum_required(VERSION 3.22...4.3)
project(registry_patched_root LANGUAGES NONE)
add_custom_target(registry_patched_root_target)
set_property(TARGET registry_patched_root_target PROPERTY KYTY_REGISTRY_PATCHED FALSE)
]===])
	kyty_registry_repo(registry_patched_root "${kyty_original}" kyty_commit)
	# The patch turns the marker on; git diff makes it from the edited file, which is then restored.
	string(REPLACE "KYTY_REGISTRY_PATCHED FALSE" "KYTY_REGISTRY_PATCHED TRUE" kyty_patched
		"${kyty_original}")
	file(WRITE "${kyty_repo}/CMakeLists.txt" "${kyty_patched}")
	kyty_registry_git("${kyty_repo}" diff)
	file(WRITE "${kyty_project}/patches/registry-patched-root.patch" "${kyty_git_output}")
	kyty_registry_git("${kyty_repo}" checkout -q -- CMakeLists.txt)
	kyty_registry_project([===[
kyty_declare_dependency(registry_patched_root
	GIT [==[file://@kyty_repo@]==]
	COMMIT @kyty_commit@
	PATCHES patches/registry-patched-root.patch)
kyty_check_prefetched_sources()
kyty_make_dependency_available(registry_patched_root)
if(TARGET registry_patched_root_target)
	get_target_property(kyty_marker registry_patched_root_target KYTY_REGISTRY_PATCHED)
	get_target_property(kyty_dir registry_patched_root_target SOURCE_DIR)
	message(STATUS "dependency_registry: target=TRUE")
	message(STATUS "dependency_registry: patched=${kyty_marker}")
	message(STATUS "dependency_registry: source=${kyty_dir}")
else()
	message(STATUS "dependency_registry: target=FALSE")
endif()
]===])
	kyty_registry_configure(configure)
	kyty_expect_exit(configure success)
	kyty_expect_report(configure target TRUE)
	kyty_expect_report(configure patched TRUE)
	kyty_expect_report(configure source "${kyty_build}/_kyty_patched/registry_patched_root")
elseif(CASE STREQUAL "online_after_prefetched")
	set(kyty_repo "${kyty_repos}/registry_online")
	kyty_registry_repo(registry_online [===[
cmake_minimum_required(VERSION 3.22...4.3)
project(registry_online LANGUAGES NONE)
add_custom_target(registry_online_target)
]===] kyty_commit)
	set(kyty_farm "${WORK_DIR}/prefetched")
	kyty_registry_farm("${kyty_farm}")
	file(COPY "${kyty_repo}/CMakeLists.txt" DESTINATION "${kyty_farm}/registry_online")
	kyty_registry_project([===[
kyty_declare_dependency(registry_online
	GIT [==[file://@kyty_repo@]==]
	COMMIT @kyty_commit@)
kyty_check_prefetched_sources()
kyty_make_dependency_available(registry_online)
if(TARGET registry_online_target)
	get_target_property(kyty_dir registry_online_target SOURCE_DIR)
	message(STATUS "dependency_registry: source=${kyty_dir}")
else()
	message(STATUS "dependency_registry: source=none")
endif()
]===])
	kyty_registry_configure(prefetched "-DKYTY_PREFETCHED_SOURCES_DIR=${kyty_farm}")
	kyty_expect_exit(prefetched success)
	kyty_expect_report(prefetched source "${kyty_farm}/registry_online")
	file(REMOVE_RECURSE "${kyty_farm}")
	kyty_registry_configure(online "-DKYTY_PREFETCHED_SOURCES_DIR=")
	kyty_expect_exit(online success)
	kyty_expect_report(online source "${kyty_build}/_deps/registry_online-src")
	if(EXISTS "${kyty_build}/CMakeCache.txt")
		file(STRINGS "${kyty_build}/CMakeCache.txt" kyty_cache_lines)
		set(kyty_stale "")
		foreach(kyty_line IN LISTS kyty_cache_lines)
			string(FIND "${kyty_line}" "${kyty_farm}" kyty_position)
			if(NOT kyty_position EQUAL -1)
				list(APPEND kyty_stale "${kyty_line}")
			endif()
		endforeach()
		if(kyty_stale)
			list(JOIN kyty_stale "; " kyty_stale)
			list(APPEND kyty_failures "online: the cache still names ${kyty_farm}: ${kyty_stale}")
		endif()
	else()
		list(APPEND kyty_failures "online: ${kyty_build}/CMakeCache.txt does not exist")
	endif()
elseif(CASE STREQUAL "prefetched_not_first_party")
	set(kyty_repo "${kyty_repos}/registry_farm")
	kyty_registry_repo(registry_farm [===[
cmake_minimum_required(VERSION 3.22...4.3)
project(registry_farm LANGUAGES NONE)
add_custom_target(registry_farm_target)
]===] kyty_commit)
	set(kyty_farm "${kyty_project}/_prefetched")
	kyty_registry_farm("${kyty_farm}")
	file(COPY "${kyty_repo}/CMakeLists.txt" DESTINATION "${kyty_farm}/registry_farm")
	kyty_registry_project([===[
add_custom_target(registry_project_target)
kyty_declare_dependency(registry_farm
	GIT [==[file://@kyty_repo@]==]
	COMMIT @kyty_commit@)
kyty_check_prefetched_sources()
kyty_make_dependency_available(registry_farm)
foreach(kyty_target IN ITEMS registry_farm_target registry_project_target)
	if(TARGET ${kyty_target})
		kyty_is_first_party_target(${kyty_target} kyty_first_party)
	else()
		set(kyty_first_party "no target")
	endif()
	message(STATUS "dependency_registry: ${kyty_target}=${kyty_first_party}")
endforeach()
]===])
	# Typed, so the relative value reaches the modules as given: an untyped PATH value given with
	# -D is made absolute against the working directory (WORK_DIR here).
	kyty_registry_configure(configure "-DKYTY_PREFETCHED_SOURCES_DIR:PATH=_prefetched")
	kyty_expect_exit(configure success)
	kyty_expect_text(configure "prefetched from ${kyty_farm} (no network access)")
	kyty_expect_report(configure registry_farm_target FALSE)
	kyty_expect_report(configure registry_project_target TRUE)
else()
	message(FATAL_ERROR "DependencyRegistryTests: unknown CASE '${CASE}' (hidden_entries, "
		"patched_root, online_after_prefetched, prefetched_not_first_party)")
endif()

if(kyty_failures)
	list(JOIN kyty_failures "\n  " kyty_failures_text)
	message(FATAL_ERROR "DependencyRegistryTests ${CASE} failed (work directory kept: ${WORK_DIR}):"
		"\n  ${kyty_failures_text}${kyty_logs}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "DependencyRegistryTests ${CASE}: passed")
