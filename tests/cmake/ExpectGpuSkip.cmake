# gpu_skip_without_device (Requirement 17.6): with VK_DRIVER_FILES naming a manifest that lists no
# driver, the Vulkan loader finds no device, and every GPU test executable must print
# "no Vulkan device available" and exit with 77, the SKIP_RETURN_CODE of its gpu-labeled CTests.
# This test checks the exit codes itself, so it has no label and no SKIP_RETURN_CODE.
#
#   cmake -DWORK_DIR=<dir> -DSHADER_RECOMPILER_COMPUTE_TESTS=<executable>
#         -DKERNEL_FILE_SYSTEM_TESTS=<executable> -P tests/cmake/ExpectGpuSkip.cmake
cmake_minimum_required(VERSION 3.19)

foreach(kyty_variable IN ITEMS WORK_DIR SHADER_RECOMPILER_COMPUTE_TESTS KERNEL_FILE_SYSTEM_TESTS)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "ExpectGpuSkip: -D${kyty_variable}=<value> is required")
	endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(kyty_manifest "${WORK_DIR}/empty_icd.json")
file(WRITE "${kyty_manifest}" "{\"file_format_version\": \"1.0.1\"}\n")

# Runs <command> with only the empty manifest visible to the Vulkan loader.
function(kyty_expect_gpu_skip name)
	execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=VK_ICD_FILENAMES --unset=VK_ADD_DRIVER_FILES
		"VK_DRIVER_FILES=${kyty_manifest}" ${ARGN}
		RESULT_VARIABLE kyty_result OUTPUT_VARIABLE kyty_output ERROR_VARIABLE kyty_output
		TIMEOUT 60)
	string(FIND "${kyty_output}" "no Vulkan device available" kyty_position)
	if(NOT kyty_result STREQUAL "77" OR kyty_position EQUAL -1)
		set_property(GLOBAL APPEND PROPERTY KYTY_GPU_SKIP_FAILURES
			"${name}: exit '${kyty_result}' (expected 77 and 'no Vulkan device available'), output:\n${kyty_output}")
		return()
	endif()
	string(REGEX MATCH "[^\n]*no Vulkan device available[^\n]*" kyty_line "${kyty_output}")
	message(STATUS "ExpectGpuSkip: ${name}: exit 77: ${kyty_line}")
endfunction()

kyty_expect_gpu_skip("shader_recompiler_compute_tests --scheduler-only"
	"${SHADER_RECOMPILER_COMPUTE_TESTS}" --scheduler-only)
kyty_expect_gpu_skip("kernel_file_system_tests" "${KERNEL_FILE_SYSTEM_TESTS}")

get_property(kyty_failures GLOBAL PROPERTY KYTY_GPU_SKIP_FAILURES)
if(kyty_failures)
	list(JOIN kyty_failures "\n" kyty_failures_text)
	message(FATAL_ERROR "ExpectGpuSkip failed (work directory kept: ${WORK_DIR}):\n${kyty_failures_text}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "ExpectGpuSkip: passed")
