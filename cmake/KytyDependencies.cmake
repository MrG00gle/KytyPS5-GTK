# Registry of the dependencies that configuration downloads (Requirements 14.3 and 14.13). The table at
# the end of this file is the only place their revisions and checksums are pinned.
#
#   kyty_declare_dependency(<name> GIT <url> COMMIT <40-hex sha> [PATCHES <file>...]
#                           [SUBMODULES <path>... | NO_SUBMODULES] [SOURCE_SUBDIR <dir>])
#   kyty_declare_dependency(<name> URL <url> SHA256 <64-hex sha> FILENAME <file>)
#       Adds a registry entry. PATCHES are relative to the top-level source directory. Online, a GIT
#       entry clones every submodule of the repository, only the SUBMODULES paths, or, with
#       NO_SUBMODULES, none (GIT_SUBMODULES "", which CMP0097 makes mean none).
#   kyty_check_prefetched_sources()
#       Call once, before any FetchContent call. With KYTY_PREFETCHED_SOURCES_DIR set, it fails once,
#       naming every dependency whose <dir>/<name> tree (or <dir>/<name>/<FILENAME> archive) or
#       SUBMODULES path is missing or holds only hidden entries (names starting with ".", such as
#       the .git of an uninitialized submodule), then sets FETCHCONTENT_FULLY_DISCONNECTED and
#       FETCHCONTENT_SOURCE_DIR_<NAME> in the caller's scope.
#   kyty_make_dependency_available(<name>)
#       GIT entries: FetchContent_Declare (GIT_TAG <sha>) and FetchContent_MakeAvailable, from the
#       network or from the prefetched tree. Entries with PATCHES are never added unpatched: the tree
#       is copied to ${CMAKE_BINARY_DIR}/_kyty_patched/<name> and patched there (kyty_patch_source_copy)
#       in both modes, <name>_SOURCE_DIR names the copy, and <copy>/<SOURCE_SUBDIR>, or <copy>
#       itself without SOURCE_SUBDIR, is added when it has a CMakeLists.txt. In prefetched mode,
#       the FETCHCONTENT_SOURCE_DIR_<NAME> cache entry that FetchContent makes from the registry's
#       value is removed again, so a later configure without KYTY_PREFETCHED_SOURCES_DIR downloads;
#       a value the user set is kept. Sets <name>_SOURCE_DIR, <name>_BINARY_DIR and
#       <name>_POPULATED.
#       URL entries: sets <name>_ARCHIVE (the prefetched archive; empty online, where the consumer
#       downloads it) and <name>_SHA256.
#   kyty_patch_source_copy(<name> SOURCE_DIR <dir> REVISION <text> PATCHES <file>... OUTPUT_VARIABLE <var>)
#       Copies <dir> to ${CMAKE_BINARY_DIR}/_kyty_patched/<name> and applies the patches there, with
#       git apply when <dir>/.git is a directory and patch -p1 otherwise (the copy then has no .git).
#       A patch for files with CRLF line endings is applied as a CRLF copy, so both tools make the
#       same bytes. The stamp _kyty_patched/<name>.stamp holds REVISION and the SHA-256 of every patch
#       and of the copy applied; the copy is reused while the stamp matches and redone otherwise.
#       Sets <var> to the copy.
#   kyty_dependency_set_source(<name> <path>)
#       Records the pristine source tree or archive of <name> used by this build.
#   kyty_dependency_prefetch_entries(<out>)
#       Sets <out> to one "<relative path>=<pristine source>" entry per dependency recorded in this
#       build, in table order: "<name>=<tree>" for GIT entries and "<name>/<FILENAME>=<archive>" for
#       URL entries. Creating <dir>/<relative path> for each entry gives a complete prefetched directory.
include_guard(GLOBAL)
include(FetchContent)

set(KYTY_PREFETCHED_SOURCES_DIR "" CACHE PATH
	"Directory holding <name> source trees and <name>/<file> archives for every configure-time dependency; when set, configuration makes no network requests")

function(_kyty_dependency_get name field out)
	get_property(kyty_dep_value GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_${field})
	set(${out} "${kyty_dep_value}" PARENT_SCOPE)
endfunction()

function(_kyty_dependency_require name context)
	get_property(kyty_dep_names GLOBAL PROPERTY KYTY_DEPENDENCIES)
	if(NOT name IN_LIST kyty_dep_names)
		message(FATAL_ERROR "${context}: ${name} is not declared in cmake/KytyDependencies.cmake")
	endif()
endfunction()

# Sets <out> to TRUE when <hex> is a lowercase hexadecimal string of <length> characters.
function(_kyty_dependency_is_hex hex length out)
	string(LENGTH "${hex}" kyty_dep_length)
	if(kyty_dep_length EQUAL length AND hex MATCHES "^[0-9a-f]+$")
		set(${out} TRUE PARENT_SCOPE)
	else()
		set(${out} FALSE PARENT_SCOPE)
	endif()
endfunction()

function(kyty_declare_dependency name)
	set(kyty_dep_context "kyty_declare_dependency(${name})")
	cmake_parse_arguments(PARSE_ARGV 1 kyty_dep "NO_SUBMODULES" "GIT;COMMIT;URL;SHA256;FILENAME;SOURCE_SUBDIR"
		"PATCHES;SUBMODULES")
	if(kyty_dep_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "${kyty_dep_context}: unexpected arguments: ${kyty_dep_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT name MATCHES "^[A-Za-z0-9_]+$")
		message(FATAL_ERROR "${kyty_dep_context}: the name may only contain letters, digits, and _")
	endif()
	get_property(kyty_dep_names GLOBAL PROPERTY KYTY_DEPENDENCIES)
	if(name IN_LIST kyty_dep_names)
		message(FATAL_ERROR "${kyty_dep_context}: declared twice")
	endif()
	if(kyty_dep_GIT AND NOT kyty_dep_URL)
		if(DEFINED kyty_dep_SHA256 OR DEFINED kyty_dep_FILENAME)
			message(FATAL_ERROR "${kyty_dep_context}: SHA256 and FILENAME belong to URL entries")
		endif()
		_kyty_dependency_is_hex("${kyty_dep_COMMIT}" 40 kyty_dep_valid)
		if(NOT kyty_dep_valid)
			message(FATAL_ERROR "${kyty_dep_context}: COMMIT '${kyty_dep_COMMIT}' is not a 40-character lowercase hexadecimal commit")
		endif()
		if(kyty_dep_NO_SUBMODULES AND kyty_dep_SUBMODULES)
			message(FATAL_ERROR "${kyty_dep_context}: give at most one of SUBMODULES and NO_SUBMODULES")
		endif()
		set(kyty_dep_patches "")
		foreach(kyty_dep_patch IN LISTS kyty_dep_PATCHES)
			get_filename_component(kyty_dep_patch "${kyty_dep_patch}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
			if(NOT EXISTS "${kyty_dep_patch}" OR IS_DIRECTORY "${kyty_dep_patch}")
				message(FATAL_ERROR "${kyty_dep_context}: patch ${kyty_dep_patch} does not exist")
			endif()
			list(APPEND kyty_dep_patches "${kyty_dep_patch}")
		endforeach()
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_KIND GIT)
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_URL "${kyty_dep_GIT}")
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_COMMIT "${kyty_dep_COMMIT}")
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_PATCHES "${kyty_dep_patches}")
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_SUBMODULES "${kyty_dep_SUBMODULES}")
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_NO_SUBMODULES "${kyty_dep_NO_SUBMODULES}")
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_SOURCE_SUBDIR "${kyty_dep_SOURCE_SUBDIR}")
	elseif(kyty_dep_URL AND NOT kyty_dep_GIT)
		if(DEFINED kyty_dep_COMMIT OR kyty_dep_PATCHES OR kyty_dep_SUBMODULES OR kyty_dep_NO_SUBMODULES
		   OR DEFINED kyty_dep_SOURCE_SUBDIR)
			message(FATAL_ERROR "${kyty_dep_context}: COMMIT, PATCHES, SUBMODULES, NO_SUBMODULES, and SOURCE_SUBDIR belong to GIT entries")
		endif()
		_kyty_dependency_is_hex("${kyty_dep_SHA256}" 64 kyty_dep_valid)
		if(NOT kyty_dep_valid)
			message(FATAL_ERROR "${kyty_dep_context}: SHA256 '${kyty_dep_SHA256}' is not a 64-character lowercase hexadecimal SHA-256")
		endif()
		if(NOT kyty_dep_FILENAME OR kyty_dep_FILENAME MATCHES "[/\\]")
			message(FATAL_ERROR "${kyty_dep_context}: FILENAME must be a plain file name")
		endif()
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_KIND URL)
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_URL "${kyty_dep_URL}")
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_SHA256 "${kyty_dep_SHA256}")
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_FILENAME "${kyty_dep_FILENAME}")
	else()
		message(FATAL_ERROR "${kyty_dep_context}: give exactly one of GIT <url> and URL <url>")
	endif()
	set_property(GLOBAL APPEND PROPERTY KYTY_DEPENDENCIES ${name})
endfunction()

# Sets <out> to TRUE when <dir> is a directory with at least one entry that is not hidden.
function(_kyty_dependency_dir_populated dir out)
	set(kyty_dep_populated FALSE)
	if(IS_DIRECTORY "${dir}")
		file(GLOB kyty_dep_children LIST_DIRECTORIES true "${dir}/*")
		# file(GLOB) also matches names that start with ".", so a lone .git would count as sources.
		list(FILTER kyty_dep_children EXCLUDE REGEX "/\\.[^/]*$")
		if(kyty_dep_children)
			set(kyty_dep_populated TRUE)
		endif()
	endif()
	set(${out} ${kyty_dep_populated} PARENT_SCOPE)
endfunction()

function(kyty_check_prefetched_sources)
	set_property(GLOBAL PROPERTY KYTY_DEPENDENCIES_CHECKED TRUE)
	get_property(kyty_dep_names GLOBAL PROPERTY KYTY_DEPENDENCIES)
	list(LENGTH kyty_dep_names kyty_dep_count)
	if(NOT KYTY_PREFETCHED_SOURCES_DIR)
		set_property(GLOBAL PROPERTY KYTY_DEPENDENCIES_PREFETCHED_DIR "")
		message(STATUS "KytyDependencies: ${kyty_dep_count} dependencies, downloaded at configure time")
		return()
	endif()
	get_filename_component(kyty_dep_dir "${KYTY_PREFETCHED_SOURCES_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	set(kyty_dep_missing "")
	set(kyty_dep_details "")
	foreach(kyty_dep_name IN LISTS kyty_dep_names)
		_kyty_dependency_get(${kyty_dep_name} KIND kyty_dep_kind)
		_kyty_dependency_get(${kyty_dep_name} URL kyty_dep_url)
		set(kyty_dep_path "${kyty_dep_dir}/${kyty_dep_name}")
		set(kyty_dep_problem "")
		if(kyty_dep_kind STREQUAL "URL")
			_kyty_dependency_get(${kyty_dep_name} FILENAME kyty_dep_filename)
			_kyty_dependency_get(${kyty_dep_name} SHA256 kyty_dep_sha256)
			if(NOT EXISTS "${kyty_dep_path}/${kyty_dep_filename}" OR IS_DIRECTORY "${kyty_dep_path}/${kyty_dep_filename}")
				set(kyty_dep_problem "${kyty_dep_path}/${kyty_dep_filename} (${kyty_dep_url}, SHA-256 ${kyty_dep_sha256})")
			endif()
		else()
			_kyty_dependency_get(${kyty_dep_name} COMMIT kyty_dep_commit)
			_kyty_dependency_dir_populated("${kyty_dep_path}" kyty_dep_populated)
			if(NOT kyty_dep_populated)
				set(kyty_dep_problem "${kyty_dep_path} (${kyty_dep_url} at ${kyty_dep_commit})")
			else()
				_kyty_dependency_get(${kyty_dep_name} SUBMODULES kyty_dep_submodules)
				foreach(kyty_dep_submodule IN LISTS kyty_dep_submodules)
					_kyty_dependency_dir_populated("${kyty_dep_path}/${kyty_dep_submodule}" kyty_dep_populated)
					if(NOT kyty_dep_populated)
						set(kyty_dep_problem "${kyty_dep_path}/${kyty_dep_submodule} (submodule ${kyty_dep_submodule} of ${kyty_dep_url} at ${kyty_dep_commit})")
						break()
					endif()
				endforeach()
			endif()
		endif()
		if(kyty_dep_problem)
			list(APPEND kyty_dep_missing ${kyty_dep_name})
			string(APPEND kyty_dep_details "\n  ${kyty_dep_name}: ${kyty_dep_problem}")
		endif()
	endforeach()
	if(kyty_dep_missing)
		list(LENGTH kyty_dep_missing kyty_dep_missing_count)
		list(JOIN kyty_dep_missing ", " kyty_dep_missing_text)
		message(FATAL_ERROR "KYTY_PREFETCHED_SOURCES_DIR=${kyty_dep_dir} lacks the sources of "
			"${kyty_dep_missing_count} of ${kyty_dep_count} dependencies: ${kyty_dep_missing_text}. "
			"Expected:${kyty_dep_details}\nPopulate them, or clear KYTY_PREFETCHED_SOURCES_DIR to download them.")
	endif()
	set_property(GLOBAL PROPERTY KYTY_DEPENDENCIES_PREFETCHED_DIR "${kyty_dep_dir}")
	set(FETCHCONTENT_FULLY_DISCONNECTED ON PARENT_SCOPE)
	foreach(kyty_dep_name IN LISTS kyty_dep_names)
		_kyty_dependency_get(${kyty_dep_name} KIND kyty_dep_kind)
		if(kyty_dep_kind STREQUAL "GIT")
			string(TOUPPER "${kyty_dep_name}" kyty_dep_upper)
			set(FETCHCONTENT_SOURCE_DIR_${kyty_dep_upper} "${kyty_dep_dir}/${kyty_dep_name}" PARENT_SCOPE)
		endif()
	endforeach()
	message(STATUS "KytyDependencies: ${kyty_dep_count} dependencies, prefetched from ${kyty_dep_dir} (no network access)")
endfunction()

function(kyty_dependency_set_source name path)
	_kyty_dependency_require(${name} "kyty_dependency_set_source")
	set_property(GLOBAL PROPERTY KYTY_DEPENDENCY_${name}_SOURCE "${path}")
endfunction()

function(kyty_dependency_prefetch_entries out)
	get_property(kyty_dep_names GLOBAL PROPERTY KYTY_DEPENDENCIES)
	set(kyty_dep_entries "")
	foreach(kyty_dep_name IN LISTS kyty_dep_names)
		_kyty_dependency_get(${kyty_dep_name} SOURCE kyty_dep_source)
		if(NOT kyty_dep_source)
			continue()
		endif()
		_kyty_dependency_get(${kyty_dep_name} KIND kyty_dep_kind)
		if(kyty_dep_kind STREQUAL "URL")
			_kyty_dependency_get(${kyty_dep_name} FILENAME kyty_dep_filename)
			list(APPEND kyty_dep_entries "${kyty_dep_name}/${kyty_dep_filename}=${kyty_dep_source}")
		else()
			list(APPEND kyty_dep_entries "${kyty_dep_name}=${kyty_dep_source}")
		endif()
	endforeach()
	set(${out} "${kyty_dep_entries}" PARENT_SCOPE)
endfunction()

# Sets <out> to <patch>, or, when every file the patch changes in the pristine <tree> has CRLF line
# endings, to a copy under <work_dir> whose context, removed, and added lines end in CRLF. git apply
# and patch -p1 then make the same bytes; patch -p1 rejects an LF patch on CRLF files. A removed line
# whose text starts with "-- " is taken for a file header and keeps LF, so such a hunk fails loudly.
function(_kyty_dependency_adapt_line_endings patch tree work_dir out)
	set(${out} "${patch}" PARENT_SCOPE)
	file(STRINGS "${patch}" kyty_dep_targets REGEX "^\\+\\+\\+ ")
	set(kyty_dep_crlf 0)
	set(kyty_dep_lf 0)
	foreach(kyty_dep_target IN LISTS kyty_dep_targets)
		string(REGEX REPLACE "^\\+\\+\\+ [^/\t]*/([^\t\r]*).*$" "\\1" kyty_dep_target "${kyty_dep_target}")
		if(NOT EXISTS "${tree}/${kyty_dep_target}" OR IS_DIRECTORY "${tree}/${kyty_dep_target}")
			continue()
		endif()
		# The first line ending decides. Read as hex: file(READ) drops carriage returns in text mode.
		file(READ "${tree}/${kyty_dep_target}" kyty_dep_head LIMIT 4096 HEX)
		string(REGEX REPLACE "(..)" "\\1;" kyty_dep_bytes "${kyty_dep_head}")
		list(FIND kyty_dep_bytes "0a" kyty_dep_newline)
		if(kyty_dep_newline GREATER 0)
			math(EXPR kyty_dep_before "${kyty_dep_newline} - 1")
			list(GET kyty_dep_bytes ${kyty_dep_before} kyty_dep_byte)
			if(kyty_dep_byte STREQUAL "0d")
				math(EXPR kyty_dep_crlf "${kyty_dep_crlf} + 1")
			else()
				math(EXPR kyty_dep_lf "${kyty_dep_lf} + 1")
			endif()
		elseif(kyty_dep_newline EQUAL 0)
			math(EXPR kyty_dep_lf "${kyty_dep_lf} + 1")
		endif()
	endforeach()
	if(kyty_dep_crlf EQUAL 0)
		return()
	endif()
	if(NOT kyty_dep_lf EQUAL 0)
		message(FATAL_ERROR "${patch} changes files with CRLF and with LF line endings; split it into one patch per line-ending style")
	endif()
	file(READ "${patch}" kyty_dep_text)
	string(REPLACE "\r\n" "\n" kyty_dep_text "\n${kyty_dep_text}")
	string(REPLACE "\n" "\r\n" kyty_dep_text "${kyty_dep_text}")
	# Header and other non-content lines keep LF.
	string(REGEX REPLACE "(\n[^ +\r\n-][^\r\n]*)\r" "\\1" kyty_dep_text "${kyty_dep_text}")
	string(REGEX REPLACE "(\n(--- |\\+\\+\\+ )[^\r\n]*)\r" "\\1" kyty_dep_text "${kyty_dep_text}")
	string(SUBSTRING "${kyty_dep_text}" 2 -1 kyty_dep_text)
	get_filename_component(kyty_dep_basename "${patch}" NAME)
	file(WRITE "${work_dir}/${kyty_dep_basename}" "${kyty_dep_text}")
	set(${out} "${work_dir}/${kyty_dep_basename}" PARENT_SCOPE)
endfunction()

function(kyty_patch_source_copy name)
	set(kyty_dep_context "kyty_patch_source_copy(${name})")
	cmake_parse_arguments(PARSE_ARGV 1 kyty_dep "" "SOURCE_DIR;REVISION;OUTPUT_VARIABLE" "PATCHES")
	if(kyty_dep_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "${kyty_dep_context}: unexpected arguments: ${kyty_dep_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT kyty_dep_SOURCE_DIR OR NOT kyty_dep_REVISION OR NOT kyty_dep_PATCHES OR NOT kyty_dep_OUTPUT_VARIABLE)
		message(FATAL_ERROR "${kyty_dep_context}: SOURCE_DIR, REVISION, PATCHES, and OUTPUT_VARIABLE are required")
	endif()
	if(NOT IS_DIRECTORY "${kyty_dep_SOURCE_DIR}")
		message(FATAL_ERROR "${kyty_dep_context}: SOURCE_DIR ${kyty_dep_SOURCE_DIR} is not a directory")
	endif()
	# A symlinked source would be copied as the link, and patching the copy would change the source.
	file(REAL_PATH "${kyty_dep_SOURCE_DIR}" kyty_dep_SOURCE_DIR)
	set(kyty_dep_root "${CMAKE_BINARY_DIR}/_kyty_patched")
	set(kyty_dep_copy "${kyty_dep_root}/${name}")
	set(kyty_dep_stamp "${kyty_dep_root}/${name}.stamp")
	set(kyty_dep_work "${kyty_dep_root}/${name}.patches")
	file(REMOVE_RECURSE "${kyty_dep_work}")
	file(MAKE_DIRECTORY "${kyty_dep_work}")
	set(kyty_dep_stamp_text "revision ${kyty_dep_REVISION}\n")
	set(kyty_dep_applied_patches "")
	foreach(kyty_dep_patch IN LISTS kyty_dep_PATCHES)
		_kyty_dependency_adapt_line_endings("${kyty_dep_patch}" "${kyty_dep_SOURCE_DIR}" "${kyty_dep_work}"
			kyty_dep_applied)
		list(APPEND kyty_dep_applied_patches "${kyty_dep_applied}")
		file(SHA256 "${kyty_dep_patch}" kyty_dep_patch_sha256)
		file(SHA256 "${kyty_dep_applied}" kyty_dep_applied_sha256)
		file(RELATIVE_PATH kyty_dep_patch_name "${CMAKE_SOURCE_DIR}" "${kyty_dep_patch}")
		string(APPEND kyty_dep_stamp_text
			"patch ${kyty_dep_patch_name} sha256 ${kyty_dep_patch_sha256} applied-sha256 ${kyty_dep_applied_sha256}\n")
	endforeach()
	set(${kyty_dep_OUTPUT_VARIABLE} "${kyty_dep_copy}" PARENT_SCOPE)
	if(IS_DIRECTORY "${kyty_dep_copy}" AND EXISTS "${kyty_dep_stamp}")
		file(READ "${kyty_dep_stamp}" kyty_dep_old_stamp)
		if(kyty_dep_old_stamp STREQUAL kyty_dep_stamp_text)
			message(STATUS "KytyDependencies: ${name}: stamp matches, reusing the patched copy ${kyty_dep_copy}")
			return()
		endif()
	endif()

	message(STATUS "KytyDependencies: ${name}: copying ${kyty_dep_SOURCE_DIR} to ${kyty_dep_copy}")
	file(REMOVE "${kyty_dep_stamp}")
	file(REMOVE_RECURSE "${kyty_dep_copy}")
	if(IS_DIRECTORY "${kyty_dep_SOURCE_DIR}/.git")
		set(kyty_dep_method "git apply")
		execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${kyty_dep_SOURCE_DIR}" "${kyty_dep_copy}"
			RESULT_VARIABLE kyty_dep_result)
		if(NOT kyty_dep_result EQUAL 0)
			message(FATAL_ERROR "${kyty_dep_context}: could not copy ${kyty_dep_SOURCE_DIR} to ${kyty_dep_copy}")
		endif()
		if(NOT GIT_EXECUTABLE)
			find_package(Git REQUIRED)
		endif()
	else()
		set(kyty_dep_method "patch -p1")
		file(COPY "${kyty_dep_SOURCE_DIR}/" DESTINATION "${kyty_dep_copy}" PATTERN ".git" EXCLUDE)
		find_program(KYTY_PATCH_EXECUTABLE NAMES patch)
		if(NOT KYTY_PATCH_EXECUTABLE)
			message(FATAL_ERROR "${kyty_dep_context}: patch was not found; it applies the repository patches "
				"to sources without a .git directory. Install GNU patch and reconfigure.")
		endif()
	endif()
	foreach(kyty_dep_patch kyty_dep_applied IN ZIP_LISTS kyty_dep_PATCHES kyty_dep_applied_patches)
		if(kyty_dep_method STREQUAL "git apply")
			execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${kyty_dep_copy}" apply --ignore-whitespace
				--whitespace=nowarn "${kyty_dep_applied}"
				RESULT_VARIABLE kyty_dep_result OUTPUT_VARIABLE kyty_dep_output ERROR_VARIABLE kyty_dep_output)
		else()
			# --forward: --batch alone would silently reverse a patch that looks already applied.
			execute_process(COMMAND "${KYTY_PATCH_EXECUTABLE}" -p1 --batch --forward --binary --ignore-whitespace
				--no-backup-if-mismatch -d "${kyty_dep_copy}" -i "${kyty_dep_applied}"
				RESULT_VARIABLE kyty_dep_result OUTPUT_VARIABLE kyty_dep_output ERROR_VARIABLE kyty_dep_output)
		endif()
		if(NOT kyty_dep_result EQUAL 0)
			file(REMOVE_RECURSE "${kyty_dep_copy}")
			message(FATAL_ERROR "${kyty_dep_context}: ${kyty_dep_method} could not apply ${kyty_dep_patch} "
				"to ${kyty_dep_copy}:\n${kyty_dep_output}")
		endif()
		message(STATUS "KytyDependencies: ${name}: applied ${kyty_dep_patch} with ${kyty_dep_method}")
	endforeach()
	file(WRITE "${kyty_dep_stamp}" "${kyty_dep_stamp_text}")
endfunction()

function(kyty_make_dependency_available name)
	set(kyty_dep_context "kyty_make_dependency_available(${name})")
	_kyty_dependency_require(${name} "${kyty_dep_context}")
	get_property(kyty_dep_checked GLOBAL PROPERTY KYTY_DEPENDENCIES_CHECKED)
	if(NOT kyty_dep_checked)
		message(FATAL_ERROR "${kyty_dep_context}: call kyty_check_prefetched_sources() first")
	endif()
	get_property(kyty_dep_prefetched GLOBAL PROPERTY KYTY_DEPENDENCIES_PREFETCHED_DIR)
	_kyty_dependency_get(${name} KIND kyty_dep_kind)
	_kyty_dependency_get(${name} URL kyty_dep_url)

	if(kyty_dep_kind STREQUAL "URL")
		_kyty_dependency_get(${name} SHA256 kyty_dep_sha256)
		set(${name}_SHA256 "${kyty_dep_sha256}" PARENT_SCOPE)
		if(kyty_dep_prefetched)
			_kyty_dependency_get(${name} FILENAME kyty_dep_filename)
			set(kyty_dep_archive "${kyty_dep_prefetched}/${name}/${kyty_dep_filename}")
			kyty_dependency_set_source(${name} "${kyty_dep_archive}")
			set(${name}_ARCHIVE "${kyty_dep_archive}" PARENT_SCOPE)
		else()
			set(${name}_ARCHIVE "" PARENT_SCOPE)
		endif()
		return()
	endif()

	_kyty_dependency_get(${name} COMMIT kyty_dep_commit)
	_kyty_dependency_get(${name} PATCHES kyty_dep_patches)
	_kyty_dependency_get(${name} SUBMODULES kyty_dep_submodules)
	_kyty_dependency_get(${name} NO_SUBMODULES kyty_dep_no_submodules)
	_kyty_dependency_get(${name} SOURCE_SUBDIR kyty_dep_subdir)
	set(kyty_dep_args GIT_REPOSITORY "${kyty_dep_url}" GIT_TAG ${kyty_dep_commit} GIT_SHALLOW FALSE)
	if(kyty_dep_submodules)
		list(APPEND kyty_dep_args GIT_SUBMODULES ${kyty_dep_submodules} GIT_SUBMODULES_RECURSE TRUE)
	endif()
	if(kyty_dep_patches)
		# A SOURCE_SUBDIR that does not exist, so FetchContent never adds the unpatched tree.
		list(APPEND kyty_dep_args SOURCE_SUBDIR kyty-registry-adds-the-patched-copy)
	elseif(kyty_dep_subdir)
		list(APPEND kyty_dep_args SOURCE_SUBDIR "${kyty_dep_subdir}")
	endif()
	if(kyty_dep_prefetched)
		# Also set here, so a call from another directory scope stays offline.
		string(TOUPPER "${name}" kyty_dep_upper)
		set(FETCHCONTENT_FULLY_DISCONNECTED ON)
		set(FETCHCONTENT_SOURCE_DIR_${kyty_dep_upper} "${kyty_dep_prefetched}/${name}")
	endif()
	if(kyty_dep_no_submodules)
		# Quoted here: an empty element of kyty_dep_args would vanish in the unquoted expansion.
		FetchContent_Declare(${name} ${kyty_dep_args} GIT_SUBMODULES "")
	else()
		FetchContent_Declare(${name} ${kyty_dep_args})
	endif()
	FetchContent_MakeAvailable(${name})
	kyty_dependency_set_source(${name} "${${name}_SOURCE_DIR}")
	# FetchContent caches the override it was given. Left there, it would point a later configure
	# without KYTY_PREFETCHED_SOURCES_DIR at the prefetched tree, which may be gone.
	if(kyty_dep_prefetched)
		set(kyty_dep_registry_dir "${kyty_dep_prefetched}/${name}")
		if("$CACHE{FETCHCONTENT_SOURCE_DIR_${kyty_dep_upper}}" STREQUAL "${kyty_dep_registry_dir}")
			unset(FETCHCONTENT_SOURCE_DIR_${kyty_dep_upper} CACHE)
		endif()
	endif()

	if(kyty_dep_patches)
		kyty_patch_source_copy(${name}
			SOURCE_DIR "${${name}_SOURCE_DIR}"
			REVISION "${kyty_dep_url} ${kyty_dep_commit}"
			PATCHES ${kyty_dep_patches}
			OUTPUT_VARIABLE ${name}_SOURCE_DIR)
		set(kyty_dep_added "${${name}_SOURCE_DIR}")
		if(kyty_dep_subdir)
			string(APPEND kyty_dep_added "/${kyty_dep_subdir}")
		endif()
		if(EXISTS "${kyty_dep_added}/CMakeLists.txt")
			add_subdirectory("${kyty_dep_added}" "${${name}_BINARY_DIR}")
		endif()
	endif()
	set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
	set(${name}_BINARY_DIR "${${name}_BINARY_DIR}" PARENT_SCOPE)
	set(${name}_POPULATED "${${name}_POPULATED}" PARENT_SCOPE)
endfunction()

# The registry. The FFmpeg tag 9ac4cfd195f1 is the 3rdparty/ffmpeg-core submodule commit.
kyty_declare_dependency(opus
	GIT https://github.com/xiph/opus.git
	COMMIT ddbe48383984d56acd9e1ab6a090c54ca6b735a6)
kyty_declare_dependency(openxr
	GIT https://github.com/KhronosGroup/OpenXR-SDK.git
	COMMIT f2448a8797c85814aa892efc1ab8707900fbcc78) # release-1.1.63
kyty_declare_dependency(xbyak
	GIT https://github.com/herumi/xbyak.git
	COMMIT 44a72f369268f7d552650891b296693e91db86bb)
kyty_declare_dependency(zydis
	GIT https://github.com/zyantific/zydis.git
	COMMIT 120e0e705f8e3b507dc49377ac2879979f0d545c
	SUBMODULES dependencies/zycore)
kyty_declare_dependency(zstd
	GIT https://github.com/facebook/zstd.git
	COMMIT f8745da6ff1ad1e7bab384bd1f9d742439278e99
	SOURCE_SUBDIR build/cmake)
kyty_declare_dependency(zarchive_source
	GIT https://github.com/Exzap/ZArchive.git
	COMMIT d2c717730092c7bf8cbb033b12fd4001b7c4d932
	PATCHES 3rdparty/patches/zarchive-reader.patch
	SOURCE_SUBDIR kyty-library-only)
kyty_declare_dependency(ffmpeg_core_prebuilt
	URL https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/9ac4cfd195f1/ffmpeg-linux-x64.zip
	SHA256 f4659ad09e6f2b99ecf0227ca56fe4bbb34572cdd8d85ff85384f74ba38f23ef
	FILENAME ffmpeg-linux-x64.zip)
# Property tests only; undeclared, so neither fetched nor checked, when BUILD_TESTING is OFF.
if(BUILD_TESTING)
	# ext/catch and ext/googletest are only used by RapidCheck's own tests (RC_ENABLE_TESTS is OFF).
	kyty_declare_dependency(rapidcheck
		GIT https://github.com/emil-e/rapidcheck.git
		COMMIT 2c3c4365aca21ef4e612768fdc95b1ce8b39a651
		NO_SUBMODULES)
endif()
