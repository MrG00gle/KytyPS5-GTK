# License files of the Kyty sources and of every third-party component (Requirements 21.1, 21.4,
# 21.7). Included from the top-level CMakeLists.txt after add_subdirectory(3rdparty), when the source
# directory of every component is known.
#
#   kyty_register_license(<component> SOURCE_DIR <dir> [NOTICE_FROM <file> LINES <a>-<b>])
#       Registers the license files of one third-party component: every file at the root of <dir>
#       whose name starts with LICENSE, LICENCE, COPYING, or NOTICE, installed byte-identical as
#       <component>/<name>. With NOTICE_FROM, lines <a> to <b> of <file>, byte for byte, are written
#       to ${CMAKE_BINARY_DIR}/kyty_licenses/<component>/NOTICE and installed as <component>/NOTICE:
#       for a component whose notice is in a header or in a file with another name. A component that
#       yields no file stops the configure, and so does a line range outside <file>.
#   kyty_register_third_party_licenses()
#       Registers the components that the build uses (the table at the end of this file), from the
#       directories the build uses them from.
#   kyty_install_licenses()
#       Call once, after the last kyty_register_license. Writes ${CMAKE_BINARY_DIR}/licenses-manifest.txt,
#       one path per line relative to the licenses directory: LICENSE and Kyty-MIT.txt (the
#       repository's LICENSE and LICENSES/Kyty-MIT.txt) first, then every component file, sorted.
#       Beside it, licenses-sources.txt has one line per manifest path for the tests:
#       "<path>|<component source dir>|<installed from>", plus "|<file>|<a>-<b>" for an extracted
#       NOTICE. In the fhs layout it installs every listed file under ${KYTY_INSTALL_LICENSEDIR} and
#       the manifest as ${KYTY_INSTALL_DATADIR}/kyty/licenses-manifest.txt; the portable layout
#       installs neither (cmake/KytyInstallLayout.cmake).
include_guard(GLOBAL)
include(FetchContent)

# Reads the line count of <file>: its newlines, plus a last line without one.
function(_kyty_license_line_count file out)
	# Text mode drops a carriage return before each newline; only the newlines are counted here.
	file(READ "${file}" kyty_lic_text)
	string(REGEX REPLACE "[^\n]" "" kyty_lic_newlines "${kyty_lic_text}")
	string(LENGTH "${kyty_lic_newlines}" kyty_lic_count)
	if(NOT kyty_lic_text STREQUAL "" AND NOT kyty_lic_text MATCHES "\n$")
		math(EXPR kyty_lic_count "${kyty_lic_count} + 1")
	endif()
	set(${out} ${kyty_lic_count} PARENT_SCOPE)
endfunction()

# Writes lines <first> to <last> of <from> to <notice>, byte for byte (sed keeps the line endings,
# which file(READ) would not). <notice> is rewritten only when its content changes.
function(_kyty_license_extract_notice context from first last notice)
	_kyty_license_line_count("${from}" kyty_lic_count)
	if(last GREATER kyty_lic_count)
		message(FATAL_ERROR "${context}: LINES ${first}-${last}: ${from} has ${kyty_lic_count} lines")
	endif()
	if(NOT KYTY_SED_EXECUTABLE)
		find_program(KYTY_SED_EXECUTABLE NAMES sed)
		if(NOT KYTY_SED_EXECUTABLE)
			message(FATAL_ERROR "${context}: sed was not found; it copies license notices out of "
				"their source files. Install sed and reconfigure.")
		endif()
	endif()
	get_filename_component(kyty_lic_dir "${notice}" DIRECTORY)
	file(MAKE_DIRECTORY "${kyty_lic_dir}")
	set(kyty_lic_new "${notice}.new")
	execute_process(COMMAND "${KYTY_SED_EXECUTABLE}" -n "${first},${last}p" "${from}"
		OUTPUT_FILE "${kyty_lic_new}" RESULT_VARIABLE kyty_lic_result ERROR_VARIABLE kyty_lic_error)
	if(NOT kyty_lic_result STREQUAL "0")
		file(REMOVE "${kyty_lic_new}")
		message(FATAL_ERROR "${context}: sed could not read lines ${first}-${last} of ${from}: "
			"${kyty_lic_result} ${kyty_lic_error}")
	endif()
	file(READ "${kyty_lic_new}" kyty_lic_text)
	string(STRIP "${kyty_lic_text}" kyty_lic_text)
	if(kyty_lic_text STREQUAL "")
		file(REMOVE "${kyty_lic_new}")
		message(FATAL_ERROR "${context}: lines ${first}-${last} of ${from} hold no text")
	endif()
	file(COPY_FILE "${kyty_lic_new}" "${notice}" ONLY_IF_DIFFERENT)
	file(REMOVE "${kyty_lic_new}")
	# A changed notice source configures again, so the installed NOTICE follows it.
	set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${from}")
endfunction()

function(kyty_register_license component)
	set(kyty_lic_context "kyty_register_license(${component})")
	cmake_parse_arguments(PARSE_ARGV 1 kyty_lic "" "SOURCE_DIR;NOTICE_FROM;LINES" "")
	if(kyty_lic_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "${kyty_lic_context}: unexpected arguments: ${kyty_lic_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT component MATCHES "^[A-Za-z0-9][A-Za-z0-9_.+-]*$")
		message(FATAL_ERROR "${kyty_lic_context}: the name may only contain letters, digits, and . _ + -")
	endif()
	get_property(kyty_lic_components GLOBAL PROPERTY KYTY_LICENSE_COMPONENTS)
	if(component IN_LIST kyty_lic_components)
		message(FATAL_ERROR "${kyty_lic_context}: registered twice")
	endif()
	get_property(kyty_lic_installed GLOBAL PROPERTY KYTY_LICENSES_INSTALLED)
	if(kyty_lic_installed)
		message(FATAL_ERROR "${kyty_lic_context}: called after kyty_install_licenses()")
	endif()
	if(NOT kyty_lic_SOURCE_DIR OR NOT IS_DIRECTORY "${kyty_lic_SOURCE_DIR}")
		message(FATAL_ERROR "${kyty_lic_context}: SOURCE_DIR '${kyty_lic_SOURCE_DIR}' is not a directory")
	endif()
	if(DEFINED kyty_lic_NOTICE_FROM AND NOT DEFINED kyty_lic_LINES
	   OR NOT DEFINED kyty_lic_NOTICE_FROM AND DEFINED kyty_lic_LINES)
		message(FATAL_ERROR "${kyty_lic_context}: NOTICE_FROM and LINES go together")
	endif()

	# CONFIGURE_DEPENDS: a license file added to or removed from <dir> configures again.
	file(GLOB kyty_lic_files LIST_DIRECTORIES false CONFIGURE_DEPENDS
		"${kyty_lic_SOURCE_DIR}/LICENSE*" "${kyty_lic_SOURCE_DIR}/LICENCE*"
		"${kyty_lic_SOURCE_DIR}/COPYING*" "${kyty_lic_SOURCE_DIR}/NOTICE*")
	list(SORT kyty_lic_files)
	set(kyty_lic_entries "")
	set(kyty_lic_names "")
	foreach(kyty_lic_file IN LISTS kyty_lic_files)
		get_filename_component(kyty_lic_name "${kyty_lic_file}" NAME)
		list(APPEND kyty_lic_names "${kyty_lic_name}")
		list(APPEND kyty_lic_entries "${component}/${kyty_lic_name}|${kyty_lic_SOURCE_DIR}|${kyty_lic_file}")
	endforeach()

	if(DEFINED kyty_lic_NOTICE_FROM)
		if(NOT kyty_lic_LINES MATCHES "^([1-9][0-9]*)-([1-9][0-9]*)$")
			message(FATAL_ERROR "${kyty_lic_context}: LINES '${kyty_lic_LINES}' is not <first>-<last>")
		endif()
		set(kyty_lic_first ${CMAKE_MATCH_1})
		set(kyty_lic_last ${CMAKE_MATCH_2})
		if(kyty_lic_first GREATER kyty_lic_last)
			message(FATAL_ERROR "${kyty_lic_context}: LINES ${kyty_lic_LINES} ends before it starts")
		endif()
		if(NOT EXISTS "${kyty_lic_NOTICE_FROM}" OR IS_DIRECTORY "${kyty_lic_NOTICE_FROM}")
			message(FATAL_ERROR "${kyty_lic_context}: NOTICE_FROM ${kyty_lic_NOTICE_FROM} is not a file")
		endif()
		if("NOTICE" IN_LIST kyty_lic_names)
			message(FATAL_ERROR "${kyty_lic_context}: ${kyty_lic_SOURCE_DIR} has a NOTICE file already; "
				"drop NOTICE_FROM")
		endif()
		set(kyty_lic_notice "${CMAKE_BINARY_DIR}/kyty_licenses/${component}/NOTICE")
		_kyty_license_extract_notice("${kyty_lic_context}" "${kyty_lic_NOTICE_FROM}" ${kyty_lic_first}
			${kyty_lic_last} "${kyty_lic_notice}")
		set(kyty_lic_entry "${component}/NOTICE|${kyty_lic_SOURCE_DIR}|${kyty_lic_notice}")
		string(APPEND kyty_lic_entry "|${kyty_lic_NOTICE_FROM}|${kyty_lic_first}-${kyty_lic_last}")
		list(APPEND kyty_lic_entries "${kyty_lic_entry}")
	endif()

	if(NOT kyty_lic_entries)
		message(FATAL_ERROR "${kyty_lic_context}: ${kyty_lic_SOURCE_DIR} has no file whose name starts "
			"with LICENSE, LICENCE, COPYING, or NOTICE at its root. Give NOTICE_FROM <file> LINES <a>-<b> "
			"to copy the license notice out of the component's source.")
	endif()
	set_property(GLOBAL APPEND PROPERTY KYTY_LICENSE_COMPONENTS ${component})
	set_property(GLOBAL APPEND PROPERTY KYTY_LICENSE_ENTRIES ${kyty_lic_entries})
endfunction()

function(kyty_install_licenses)
	get_property(kyty_lic_installed GLOBAL PROPERTY KYTY_LICENSES_INSTALLED)
	if(kyty_lic_installed)
		message(FATAL_ERROR "kyty_install_licenses: called twice")
	endif()
	set_property(GLOBAL PROPERTY KYTY_LICENSES_INSTALLED TRUE)
	get_property(kyty_lic_components GLOBAL PROPERTY KYTY_LICENSE_COMPONENTS)
	get_property(kyty_lic_entries GLOBAL PROPERTY KYTY_LICENSE_ENTRIES)

	# The repository's own license files, at the root of the licenses directory.
	set(kyty_lic_root_entries
		"LICENSE|${CMAKE_SOURCE_DIR}|${CMAKE_SOURCE_DIR}/LICENSE"
		"Kyty-MIT.txt|${CMAKE_SOURCE_DIR}|${CMAKE_SOURCE_DIR}/LICENSES/Kyty-MIT.txt")
	foreach(kyty_lic_file IN ITEMS "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/LICENSES/Kyty-MIT.txt")
		if(NOT EXISTS "${kyty_lic_file}" OR IS_DIRECTORY "${kyty_lic_file}")
			message(FATAL_ERROR "kyty_install_licenses: ${kyty_lic_file} is missing")
		endif()
	endforeach()

	# Sorted by path alone: the "|" after each path would sort "COPYING.txt" before "COPYING".
	set(kyty_lic_paths "")
	foreach(kyty_lic_entry IN LISTS kyty_lic_entries)
		string(FIND "${kyty_lic_entry}" "|" kyty_lic_separator)
		string(SUBSTRING "${kyty_lic_entry}" 0 ${kyty_lic_separator} kyty_lic_path)
		list(APPEND kyty_lic_paths "${kyty_lic_path}")
	endforeach()
	list(SORT kyty_lic_paths)
	set(kyty_lic_sorted_entries "${kyty_lic_root_entries}")
	foreach(kyty_lic_path IN LISTS kyty_lic_paths)
		foreach(kyty_lic_entry IN LISTS kyty_lic_entries)
			if(kyty_lic_entry MATCHES "^([^|]*)\\|" AND CMAKE_MATCH_1 STREQUAL kyty_lic_path)
				list(APPEND kyty_lic_sorted_entries "${kyty_lic_entry}")
				break()
			endif()
		endforeach()
	endforeach()

	set(kyty_lic_manifest_text "")
	set(kyty_lic_sources_text "")
	foreach(kyty_lic_entry IN LISTS kyty_lic_sorted_entries)
		string(REPLACE "|" ";" kyty_lic_fields "${kyty_lic_entry}")
		list(GET kyty_lic_fields 0 kyty_lic_path)
		list(GET kyty_lic_fields 2 kyty_lic_from)
		string(APPEND kyty_lic_manifest_text "${kyty_lic_path}\n")
		string(APPEND kyty_lic_sources_text "${kyty_lic_entry}\n")
		if(KYTY_INSTALL_LAYOUT STREQUAL "fhs")
			get_filename_component(kyty_lic_subdir "${kyty_lic_path}" DIRECTORY)
			set(kyty_lic_destination "${KYTY_INSTALL_LICENSEDIR}")
			if(kyty_lic_subdir)
				string(APPEND kyty_lic_destination "/${kyty_lic_subdir}")
			endif()
			install(FILES "${kyty_lic_from}" DESTINATION "${kyty_lic_destination}")
		endif()
	endforeach()

	# Rewritten only when the content changes.
	foreach(kyty_lic_output IN ITEMS manifest sources)
		set(kyty_lic_file "${CMAKE_BINARY_DIR}/licenses-${kyty_lic_output}.txt")
		file(WRITE "${kyty_lic_file}.new" "${kyty_lic_${kyty_lic_output}_text}")
		file(COPY_FILE "${kyty_lic_file}.new" "${kyty_lic_file}" ONLY_IF_DIFFERENT)
		file(REMOVE "${kyty_lic_file}.new")
	endforeach()

	list(LENGTH kyty_lic_components kyty_lic_component_count)
	list(LENGTH kyty_lic_sorted_entries kyty_lic_file_count)
	if(KYTY_INSTALL_LAYOUT STREQUAL "fhs")
		install(FILES "${CMAKE_BINARY_DIR}/licenses-manifest.txt" DESTINATION "${KYTY_INSTALL_DATADIR}/kyty")
		set(kyty_lic_where "installed under ${KYTY_INSTALL_LICENSEDIR}")
	else()
		set(kyty_lic_where "not installed in the ${KYTY_INSTALL_LAYOUT} layout")
	endif()
	message(STATUS "KytyLicenses: ${kyty_lic_component_count} components, ${kyty_lic_file_count} files "
		"in licenses-manifest.txt, ${kyty_lic_where}")
endfunction()

# Sets <out> to the source directory of the registry entry <name> (cmake/KytyDependencies.cmake):
# the downloaded or prefetched tree.
function(_kyty_license_dependency_dir name out)
	FetchContent_GetProperties(${name} SOURCE_DIR kyty_lic_dir POPULATED kyty_lic_populated)
	if(NOT kyty_lic_populated OR NOT IS_DIRECTORY "${kyty_lic_dir}")
		message(FATAL_ERROR "KytyLicenses: the dependency ${name} is not available, so its license "
			"files cannot be registered")
	endif()
	set(${out} "${kyty_lic_dir}" PARENT_SCOPE)
endfunction()

# The components of design.md "License installation", in its order, and the FFmpeg prebuilt archive.
function(kyty_register_third_party_licenses)
	set(kyty_lic_3rdparty "${KYTY_THIRD_PARTY_DIR}")
	foreach(kyty_lic_component IN ITEMS SDL3 fmt magic_enum nlohmann_json spdlog tracy imgui LibAtrac9 stb)
		kyty_register_license(${kyty_lic_component} SOURCE_DIR "${kyty_lic_3rdparty}/${kyty_lic_component}")
	endforeach()
	# renderdoc_app.h is the only file; its license is the comment block at the top (the text lines
	# between the two border lines).
	kyty_register_license(renderdoc SOURCE_DIR "${kyty_lic_3rdparty}/renderdoc"
		NOTICE_FROM "${kyty_lic_3rdparty}/renderdoc/renderdoc_app.h" LINES 2-22)
	kyty_register_license(cpuinfo SOURCE_DIR "${kyty_lic_3rdparty}/cpuinfo")
	kyty_register_license(clog SOURCE_DIR "${kyty_lic_3rdparty}/cpuinfo/deps/clog")
	foreach(kyty_lic_component IN ITEMS xxHash Vulkan-Headers VulkanMemoryAllocator SPIRV-Tools SPIRV-Headers)
		kyty_register_license(${kyty_lic_component} SOURCE_DIR "${kyty_lic_3rdparty}/${kyty_lic_component}")
	endforeach()
	# ffmpeg-core keeps the LGPL 2.1 text in a file named copyright.
	kyty_register_license(ffmpeg-core SOURCE_DIR "${kyty_lic_3rdparty}/ffmpeg-core"
		NOTICE_FROM "${kyty_lic_3rdparty}/ffmpeg-core/copyright" LINES 1-502)

	_kyty_license_dependency_dir(opus kyty_lic_opus)
	kyty_register_license(opus SOURCE_DIR "${kyty_lic_opus}")
	_kyty_license_dependency_dir(openxr kyty_lic_openxr)
	kyty_register_license(openxr SOURCE_DIR "${kyty_lic_openxr}")
	# The OpenXR loader is built with the jsoncpp copy bundled in OpenXR-SDK.
	kyty_register_license(jsoncpp SOURCE_DIR "${kyty_lic_openxr}/src/external/jsoncpp")
	# xbyak's notice file is named COPYRIGHT; its first line is empty.
	_kyty_license_dependency_dir(xbyak kyty_lic_xbyak)
	kyty_register_license(xbyak SOURCE_DIR "${kyty_lic_xbyak}"
		NOTICE_FROM "${kyty_lic_xbyak}/COPYRIGHT" LINES 2-27)
	_kyty_license_dependency_dir(zydis kyty_lic_zydis)
	kyty_register_license(zydis SOURCE_DIR "${kyty_lic_zydis}")
	kyty_register_license(zycore SOURCE_DIR "${kyty_lic_zydis}/dependencies/zycore")
	_kyty_license_dependency_dir(zstd kyty_lic_zstd)
	kyty_register_license(zstd SOURCE_DIR "${kyty_lic_zstd}")
	_kyty_license_dependency_dir(zarchive_source kyty_lic_zarchive)
	kyty_register_license(ZArchive SOURCE_DIR "${kyty_lic_zarchive}")

	# The FFmpeg libraries come from the prebuilt archive, unpacked by 3rdparty/ffmpeg-core. Its
	# share/ffmpeg/copyright equals ffmpeg-core's copyright; its SOURCE.txt adds the LGPL statement,
	# the Independent JPEG Group credit, and where the matching source is.
	get_directory_property(kyty_lic_ffmpeg DIRECTORY "${CMAKE_BINARY_DIR}/3rdparty/ffmpeg-core"
		DEFINITION FFMPEG_PREBUILT_DIR)
	kyty_register_license(ffmpeg SOURCE_DIR "${kyty_lic_ffmpeg}"
		NOTICE_FROM "${kyty_lic_ffmpeg}/share/ffmpeg/SOURCE.txt" LINES 1-6)
endfunction()
