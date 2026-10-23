# Stops configuration unless the target is Linux x86-64.
#
# The top-level CMakeLists.txt calls kyty_check_platform() twice: right after
# project(... LANGUAGES NONE), when CMAKE_SYSTEM_NAME and CMAKE_SYSTEM_PROCESSOR are known, and again
# after enable_language(), when CMAKE_SIZEOF_VOID_P is known. Both calls run before any target exists.
#
# The function uses only script-mode commands, so the same check also runs as
#   cmake -DCMAKE_SYSTEM_NAME=<name> -DCMAKE_SYSTEM_PROCESSOR=<cpu> [-DCMAKE_SIZEOF_VOID_P=<n>] -P cmake/KytyPlatform.cmake
function(kyty_check_platform)
	set(kyty_supported TRUE)
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(kyty_supported FALSE)
	endif()
	if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
		set(kyty_supported FALSE)
	endif()
	if(DEFINED CMAKE_SIZEOF_VOID_P)
		set(kyty_pointer_size "${CMAKE_SIZEOF_VOID_P}")
		if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
			set(kyty_supported FALSE)
		endif()
	else()
		set(kyty_pointer_size "not determined yet")
	endif()

	if(NOT kyty_supported)
		message(FATAL_ERROR "KytyPS5-GTK supports only Linux x86-64; detected "
			"CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}', CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}', "
			"CMAKE_SIZEOF_VOID_P='${kyty_pointer_size}'")
	endif()
endfunction()

if(DEFINED CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
	kyty_check_platform()
endif()
