# Writes the entries of the dependency registry (cmake/KytyDependencies.cmake) as JSON, for
# tools/checks/flatpak_checks.py (task 26.6). Script mode, run with the checkout as the working
# directory: script mode sets CMAKE_SOURCE_DIR to it, and the registry resolves PATCHES against it.
#
#   cmake -DBUILD_TESTING=OFF -DOUTPUT=<file.json> -P tools/checks/dependency_registry.cmake
#
# BUILD_TESTING (ON or OFF) selects the entries that a configure with that value declares; the
# Flatpak configures with OFF. OUTPUT receives
#   {"build_testing": false, "entries": [
#     {"name": ..., "kind": "GIT" or "URL", "url": ..., "commit": ..., "sha256": ..., "filename": ...,
#      "submodules": [...], "no_submodules": false, "source_subdir": ...,
#      "patches": [<path relative to the checkout>, ...]}, ...]}
# with the entries in table order, and "", [], or false for the fields an entry does not have.
cmake_minimum_required(VERSION 3.22)

foreach(kyty_variable IN ITEMS BUILD_TESTING OUTPUT)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "dependency_registry.cmake: ${kyty_variable} is not set (pass -D${kyty_variable}=...)")
	endif()
endforeach()
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." kyty_checkout)
file(REAL_PATH "${CMAKE_SOURCE_DIR}" kyty_working_dir)
if(NOT kyty_working_dir STREQUAL kyty_checkout)
	message(FATAL_ERROR "dependency_registry.cmake: run it with the checkout ${kyty_checkout} as the "
		"working directory (it is ${kyty_working_dir}); the registry resolves PATCHES against it")
endif()
include("${kyty_checkout}/cmake/KytyDependencies.cmake")

# Sets <out> to <value> as a JSON string literal.
function(_kyty_registry_json_string value out)
	string(REPLACE "\\" "\\\\" value "${value}")
	string(REPLACE "\"" "\\\"" value "${value}")
	string(REPLACE "\n" "\\n" value "${value}")
	string(REPLACE "\r" "\\r" value "${value}")
	string(REPLACE "\t" "\\t" value "${value}")
	set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()

# Sets <out> to a JSON array of the remaining arguments as string literals.
function(_kyty_registry_json_array out)
	set(kyty_items "")
	foreach(kyty_item IN LISTS ARGN)
		_kyty_registry_json_string("${kyty_item}" kyty_item)
		list(APPEND kyty_items "${kyty_item}")
	endforeach()
	list(JOIN kyty_items ", " kyty_items)
	set(${out} "[${kyty_items}]" PARENT_SCOPE)
endfunction()

get_property(kyty_names GLOBAL PROPERTY KYTY_DEPENDENCIES)
set(kyty_entries "")
foreach(kyty_name IN LISTS kyty_names)
	foreach(kyty_field IN ITEMS KIND URL COMMIT SHA256 FILENAME SUBMODULES NO_SUBMODULES SOURCE_SUBDIR PATCHES)
		get_property(kyty_${kyty_field} GLOBAL PROPERTY KYTY_DEPENDENCY_${kyty_name}_${kyty_field})
	endforeach()
	set(kyty_patches "")
	foreach(kyty_patch IN LISTS kyty_PATCHES)
		file(RELATIVE_PATH kyty_patch "${CMAKE_SOURCE_DIR}" "${kyty_patch}")
		list(APPEND kyty_patches "${kyty_patch}")
	endforeach()
	set(kyty_entry "")
	foreach(kyty_pair IN ITEMS name=kyty_name kind=kyty_KIND url=kyty_URL commit=kyty_COMMIT
			sha256=kyty_SHA256 filename=kyty_FILENAME source_subdir=kyty_SOURCE_SUBDIR)
		string(REPLACE "=" ";" kyty_pair "${kyty_pair}")
		list(GET kyty_pair 0 kyty_key)
		list(GET kyty_pair 1 kyty_variable)
		_kyty_registry_json_string("${${kyty_variable}}" kyty_value)
		string(APPEND kyty_entry "\"${kyty_key}\": ${kyty_value}, ")
	endforeach()
	_kyty_registry_json_array(kyty_submodules ${kyty_SUBMODULES})
	_kyty_registry_json_array(kyty_patches ${kyty_patches})
	if(kyty_NO_SUBMODULES)
		set(kyty_no_submodules true)
	else()
		set(kyty_no_submodules false)
	endif()
	string(APPEND kyty_entry "\"submodules\": ${kyty_submodules}, \"no_submodules\": ${kyty_no_submodules}, "
		"\"patches\": ${kyty_patches}")
	list(APPEND kyty_entries "    {${kyty_entry}}")
endforeach()

if(BUILD_TESTING)
	set(kyty_build_testing true)
else()
	set(kyty_build_testing false)
endif()
list(JOIN kyty_entries ",\n" kyty_entries)
set(kyty_json "{\"build_testing\": ${kyty_build_testing}, \"entries\": [\n${kyty_entries}\n]}\n")
# Fails on text that is not JSON.
string(JSON kyty_count LENGTH "${kyty_json}" entries)
file(WRITE "${OUTPUT}" "${kyty_json}")
message(STATUS "dependency_registry.cmake: ${kyty_count} entries with BUILD_TESTING=${BUILD_TESTING} "
	"written to ${OUTPUT}")
