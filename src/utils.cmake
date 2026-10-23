# Keep the existing Linux IWYU warnings non-fatal.
set(KYTY_IWYU_COMMON "-Xiwyu;--cxx17ns;-Qunused-arguments")
if (LINUX)
	set(KYTY_IWYU_STRICT "")
else()
	set(KYTY_IWYU_STRICT ";-Werror")
endif()

function(include_what_you_use target dirs)
  if (CLANG AND ("${target}" IN_LIST KYTY_IWYU))
    find_program (CLANG_IWYU_EXE NAMES "include-what-you-use")
    if (CLANG_IWYU_EXE)
		set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "${CLANG_IWYU_EXE};${KYTY_IWYU_COMMON}${KYTY_IWYU_STRICT}")
    endif()
  endif()
endfunction()

function(include_what_you_use_with_mappings target dirs mappings)
  if (CLANG AND ("${target}" IN_LIST KYTY_IWYU))
    find_program (CLANG_IWYU_EXE NAMES "include-what-you-use")
    if (CLANG_IWYU_EXE)
		foreach(map ${mappings})
			list(APPEND mapdirs ";-Xiwyu;--mapping_file=${map}")
		endforeach()
		set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "${CLANG_IWYU_EXE};${mapdirs};${KYTY_IWYU_COMMON}${KYTY_IWYU_STRICT}")
    endif()
  endif()
endfunction()

function(clang_tidy_check target config headers dirs)
  if (KYTY_ENABLE_CLANG_TIDY AND CLANG AND ("${target}" IN_LIST KYTY_CLANG_TIDY))
    find_program (CLANG_TIDY_EXE NAMES "clang-tidy")
    if (CLANG_TIDY_EXE)
		set(std_arg "-extra-arg=-std=c++${CMAKE_CXX_STANDARD}")
		foreach(dir ${dirs})
			list(APPEND incdirs "-extra-arg=-I${dir}")
		endforeach()
		foreach(header ${headers})
			list(APPEND filter "(${header}.*)")
		endforeach()
		string(REPLACE ";" "|" filter "${filter}")
		if ("${config}" STREQUAL "")
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-warnings-as-errors=*;-header-filter=${filter};${std_arg};${incdirs}")	
		else()		
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-config=${config};-warnings-as-errors=*;-header-filter=${filter};${std_arg};${incdirs}")	
		endif()
    endif()
  endif()
endfunction()

macro(config_compiler_and_linker)

set(KYTY_WARNINGS_ARE_ERRORS OFF)

set(KYTY_C_FLAGS "")
set(KYTY_CPP_FLAGS "")

SET(CMAKE_NINJA_FORCE_RESPONSE_FILE 1 CACHE INTERNAL "")

if(CLANG OR GCC)

	if (CLANG)
	    set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -fno-rtti -fno-exceptions -fcolor-diagnostics -finput-charset=UTF-8 -fexec-charset=UTF-8 -g -fno-strict-aliasing -fno-omit-frame-pointer -Wall -fmessage-length=0")
	else()
		set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -fno-exceptions -fdiagnostics-color=always -finput-charset=UTF-8 -fexec-charset=UTF-8 -static-libgcc -static-libstdc++ -g -fno-strict-aliasing -fno-omit-frame-pointer -Wall -Wno-unused-value -fmessage-length=0")
	endif()
	
    if(KYTY_WARNINGS_ARE_ERRORS)
        #set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -Werror")
	add_compile_options(-Werror)
    endif()	

	add_link_options("-g")
	
	unset(CMAKE_CXX_STANDARD_LIBRARIES CACHE)
	unset(CMAKE_C_STANDARD_LIBRARIES CACHE)
	
	set(KYTY_C_FLAGS "${KYTY_CPP_FLAGS}")
	
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${KYTY_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${KYTY_CPP_FLAGS}")

endmacro()
