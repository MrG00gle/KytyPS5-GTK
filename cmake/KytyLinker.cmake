# Every KytyPS5-GTK executable is linked with lld.
include(KytyTargets)

# Stops configuration when ld.lld cannot be found. Setting KYTY_LD_LLD_EXECUTABLE to a path that does
# not exist also fails, so the check can be exercised on a host that has lld installed.
function(kyty_require_lld)
	find_program(KYTY_LD_LLD_EXECUTABLE NAMES ld.lld)
	if(NOT KYTY_LD_LLD_EXECUTABLE)
		message(FATAL_ERROR "ld.lld was not found; KytyPS5-GTK links every executable with lld (-fuse-ld=lld). "
			"Install lld and reconfigure.")
	endif()
	if(NOT EXISTS "${KYTY_LD_LLD_EXECUTABLE}")
		message(FATAL_ERROR "ld.lld was not found at KYTY_LD_LLD_EXECUTABLE='${KYTY_LD_LLD_EXECUTABLE}'; "
			"KytyPS5-GTK links every executable with lld (-fuse-ld=lld). If this cache entry is stale, "
			"clear it (cmake -UKYTY_LD_LLD_EXECUTABLE <build-dir>) and reconfigure, or set it to a valid "
			"ld.lld (-DKYTY_LD_LLD_EXECUTABLE=/path/to/ld.lld).")
	endif()
	message(STATUS "Linking with lld: ${KYTY_LD_LLD_EXECUTABLE}")
endfunction()

# Links the executable target with lld.
function(kyty_link_with_lld target)
	target_link_options(${target} PRIVATE -fuse-ld=lld)
endfunction()

# Drift check, run at the end of the top-level directory:
#   cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL kyty_check_lld_executables)
# Fails naming every first-party executable target (see kyty_is_first_party_target) whose
# LINK_OPTIONS lack -fuse-ld=lld. Writes ${CMAKE_BINARY_DIR}/kyty_lld_executables.txt with the path
# of every first-party executable, one per line, for the lld_linked_executables test
# (tools/checks/check_lld.py).
function(kyty_check_lld_executables)
	kyty_collect_targets(kyty_targets)
	set(kyty_not_lld "")
	set(kyty_manifest "")
	foreach(kyty_target IN LISTS kyty_targets)
		get_target_property(kyty_type ${kyty_target} TYPE)
		if(NOT kyty_type STREQUAL "EXECUTABLE")
			continue()
		endif()
		kyty_is_first_party_target(${kyty_target} kyty_first_party)
		if(NOT kyty_first_party)
			continue()
		endif()
		get_target_property(kyty_link_options ${kyty_target} LINK_OPTIONS)
		if(NOT kyty_link_options OR NOT "-fuse-ld=lld" IN_LIST kyty_link_options)
			list(APPEND kyty_not_lld ${kyty_target})
		endif()
		string(APPEND kyty_manifest "$<TARGET_FILE:${kyty_target}>\n")
	endforeach()
	if(kyty_not_lld)
		list(JOIN kyty_not_lld ", " kyty_not_lld_text)
		message(FATAL_ERROR "These first-party executables are not linked with lld (-fuse-ld=lld): "
			"${kyty_not_lld_text}. Call kyty_link_with_lld(<target>) for each one, or create test "
			"executables with kyty_add_test_executable().")
	endif()
	file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/kyty_lld_executables.txt" CONTENT "${kyty_manifest}")
endfunction()
