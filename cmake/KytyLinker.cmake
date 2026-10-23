# Every KytyPS5-GTK executable is linked with lld.

# Stops configuration when ld.lld cannot be found. Setting KYTY_LD_LLD_EXECUTABLE to a path that does
# not exist also fails, so the check can be exercised on a host that has lld installed.
function(kyty_require_lld)
	find_program(KYTY_LD_LLD_EXECUTABLE NAMES ld.lld)
	if(NOT KYTY_LD_LLD_EXECUTABLE OR NOT EXISTS "${KYTY_LD_LLD_EXECUTABLE}")
		message(FATAL_ERROR "ld.lld was not found; KytyPS5-GTK links every executable with lld (-fuse-ld=lld). "
			"Install lld and reconfigure.")
	endif()
	message(STATUS "Linking with lld: ${KYTY_LD_LLD_EXECUTABLE}")
endfunction()

# Links the executable target with lld (KYTY_LD_OPTIONS is -fuse-ld=lld).
function(kyty_link_with_lld target)
	target_link_options(${target} PRIVATE ${KYTY_LD_OPTIONS})
endfunction()
