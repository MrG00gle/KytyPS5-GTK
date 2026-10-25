# Helpers that walk the targets of the whole build. They are meant for checks that run at the end of
# configuration (cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} ...)), when every directory has
# been processed.
include_guard(GLOBAL)

# Sets <out> to the top-level source directory and every directory added below it, recursively
# through the SUBDIRECTORIES directory property.
function(kyty_collect_directories out)
	set(kyty_directories "")
	set(kyty_pending "${CMAKE_SOURCE_DIR}")
	while(kyty_pending)
		list(POP_FRONT kyty_pending kyty_directory)
		list(APPEND kyty_directories "${kyty_directory}")
		get_property(kyty_subdirectories DIRECTORY "${kyty_directory}" PROPERTY SUBDIRECTORIES)
		list(APPEND kyty_pending ${kyty_subdirectories})
	endwhile()
	set(${out} "${kyty_directories}" PARENT_SCOPE)
endfunction()

# Sets <out> to every target that a directory of this build defines (BUILDSYSTEM_TARGETS of every
# directory). Imported and ALIAS targets are not included.
function(kyty_collect_targets out)
	kyty_collect_directories(kyty_directories)
	set(kyty_targets "")
	foreach(kyty_directory IN LISTS kyty_directories)
		get_property(kyty_directory_targets DIRECTORY "${kyty_directory}" PROPERTY BUILDSYSTEM_TARGETS)
		list(APPEND kyty_targets ${kyty_directory_targets})
	endforeach()
	set(${out} "${kyty_targets}" PARENT_SCOPE)
endfunction()

# Sets <out> to TRUE when <target> is a first-party target: its SOURCE_DIR is the top-level source
# directory or below it, but not in the build tree (FetchContent sources and patched copies live
# there, and build trees may sit inside the checkout) and not under 3rdparty/.
function(kyty_is_first_party_target target out)
	set(kyty_first_party FALSE)
	get_target_property(kyty_dir ${target} SOURCE_DIR)
	if(kyty_dir)
		string(FIND "${kyty_dir}/" "${CMAKE_SOURCE_DIR}/" kyty_in_source)
		string(FIND "${kyty_dir}/" "${CMAKE_BINARY_DIR}/" kyty_in_binary)
		string(FIND "${kyty_dir}/" "${CMAKE_SOURCE_DIR}/3rdparty/" kyty_in_third_party)
		if(NOT CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR AND kyty_in_binary EQUAL 0)
			set(kyty_in_source -1)
		endif()
		if(kyty_in_source EQUAL 0 AND NOT kyty_in_third_party EQUAL 0)
			set(kyty_first_party TRUE)
		endif()
	endif()
	set(${out} ${kyty_first_party} PARENT_SCOPE)
endfunction()
