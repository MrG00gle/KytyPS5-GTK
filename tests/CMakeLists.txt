# Test executables and CTest tests. Included from the top-level CMakeLists.txt when BUILD_TESTING is ON.
# Create executables only with kyty_add_test_executable() and register tests only with kyty_add_test()
# (cmake/KytyTesting.cmake); the end-of-configure checks fail on anything else. Executables are written
# to ${CMAKE_BINARY_DIR} and keep their names, so scripts and CI can address them by name.

# Builds every test executable (kyty_add_test_executable() adds each one).
add_custom_target(kyty_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# The emulator shader headers are generated in the top-level directory. Targets here get no build
# order edge to those rules, so they depend on kyty_emulator_generated_headers.
function(add_kyty_full_emulator_test target source)
	kyty_add_test_executable(${target} SOURCES ${source} ${kyty_emulator_src} LINK ${kyty_emulator_link_libraries})
	target_include_directories(${target} PRIVATE ${inc_headers})
	add_dependencies(${target} kyty_emulator_generated_headers)
endfunction()

add_kyty_full_emulator_test(shader_cfg_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp")

kyty_add_test_executable(scalar_provenance_tests
	SOURCES
	"${KYTY_TESTS_DIR}/ScalarProvenanceTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ReadLaneElimination.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
	LINK fmt::fmt xbyak::xbyak
)
target_include_directories(scalar_provenance_tests PRIVATE ${inc_headers})

kyty_add_test_executable(page_manager_tests
	SOURCES
	"${KYTY_TESTS_DIR}/PageManagerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
	LINK common
)
target_include_directories(page_manager_tests PRIVATE ${inc_headers})

kyty_add_test_executable(archive_file_tests
	SOURCES "${KYTY_TESTS_DIR}/ArchiveFileTests.cpp"
	LINK common ZArchive::zarchive
)
target_include_directories(archive_file_tests PRIVATE ${inc_headers})

kyty_add_test_executable(trophy_system_tests
	SOURCES "${KYTY_TESTS_DIR}/TrophySystemTests.cpp"
	LINK common
)
target_include_directories(trophy_system_tests PRIVATE ${inc_headers})

kyty_add_test_executable(avplayer_file_tests
	SOURCES
	"${KYTY_TESTS_DIR}/AvPlayerFileTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
	LINK common FFmpeg::ffmpeg ZArchive::zarchive
)
target_include_directories(avplayer_file_tests PRIVATE ${inc_headers})

kyty_add_test_executable(vdecsw_tests
	SOURCES
	"${KYTY_TESTS_DIR}/VdecswTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/videoDec2Decoder.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
	LINK common FFmpeg::ffmpeg
)
target_include_directories(vdecsw_tests PRIVATE ${inc_headers})

kyty_add_test_executable(bit_array_tests
	SOURCES "${KYTY_TESTS_DIR}/BitArrayTests.cpp"
)
target_include_directories(bit_array_tests PRIVATE ${inc_headers})

kyty_add_test_executable(lru_cache_tests
	SOURCES "${KYTY_TESTS_DIR}/LruCacheTests.cpp"
)
target_include_directories(lru_cache_tests PRIVATE ${inc_headers})

kyty_add_test_executable(ime_dialog_tests
	SOURCES
	"${KYTY_TESTS_DIR}/ImeDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeCommon.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeDialog.cpp"
)
target_include_directories(ime_dialog_tests PRIVATE ${inc_headers})

kyty_add_test_executable(memory_tracker_tests
	SOURCES
	"${KYTY_TESTS_DIR}/MemoryTrackerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/memoryTracker.cpp"
	LINK fmt::fmt common
)
target_include_directories(memory_tracker_tests PRIVATE ${inc_headers})

kyty_add_test_executable(shader_vertex_metadata_tests
	SOURCES
	"${KYTY_TESTS_DIR}/ShaderVertexMetadataTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderVertexMetadata.cpp"
)
target_include_directories(shader_vertex_metadata_tests PRIVATE ${inc_headers})

kyty_add_test_executable(resource_materialization_tests
	SOURCES
	"${KYTY_TESTS_DIR}/ResourceMaterializationTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
	LINK fmt::fmt xbyak::xbyak
)
target_include_directories(resource_materialization_tests PRIVATE ${inc_headers})

kyty_add_test_executable(resource_tracking_tests
	SOURCES
	"${KYTY_TESTS_DIR}/ResourceTrackingTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderPixelParameter.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ShaderInfoCollection.cpp"
	LINK fmt::fmt xbyak::xbyak
)
target_include_directories(resource_tracking_tests PRIVATE ${inc_headers})

kyty_add_test_executable(audio_out2_port_tests
	SOURCES
	"${KYTY_TESTS_DIR}/AudioOut2PortTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/libAudio2.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
	LINK common fmt::fmt
)
target_include_directories(audio_out2_port_tests PRIVATE ${inc_headers})

kyty_add_test_executable(audio_out_timing_tests
	SOURCES "${KYTY_TESTS_DIR}/AudioOutTimingTests.cpp"
	LINK common fmt::fmt SDL3::SDL3-static
)
target_include_directories(audio_out_timing_tests PRIVATE ${inc_headers})

kyty_add_test_executable(ngs2_sampler_tests
	SOURCES
	"${KYTY_TESTS_DIR}/Ngs2SamplerTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
	LINK common fmt::fmt LibAtrac9
)
target_include_directories(ngs2_sampler_tests PRIVATE ${inc_headers})

kyty_add_test_executable(pad_haptics_tests
	SOURCES "${KYTY_TESTS_DIR}/PadHapticsTests.cpp"
	LINK common fmt::fmt SDL3::SDL3-static opus
)
target_include_directories(pad_haptics_tests PRIVATE ${inc_headers})

kyty_add_test_executable(controller_settings_tests
	SOURCES "${KYTY_TESTS_DIR}/ControllerSettingsTests.cpp"
	LINK common fmt::fmt SDL3::SDL3-static
)
target_include_directories(controller_settings_tests PRIVATE ${inc_headers})

kyty_add_test_executable(host_input_tests
	SOURCES "${KYTY_TESTS_DIR}/HostInputTests.cpp"
	LINK common fmt::fmt SDL3::SDL3-static
)
target_include_directories(host_input_tests PRIVATE ${inc_headers})

kyty_add_test_executable(save_data_memory_tests
	SOURCES "${KYTY_TESTS_DIR}/SaveDataMemoryTests.cpp"
	LINK common fmt::fmt
)
target_include_directories(save_data_memory_tests PRIVATE ${inc_headers})

kyty_add_test_executable(png_enc_tests
	SOURCES "${KYTY_TESTS_DIR}/PngEncTests.cpp"
	LINK common fmt::fmt
)
target_include_directories(png_enc_tests PRIVATE ${inc_headers})

kyty_add_test_executable(ces_tests
	SOURCES "${KYTY_TESTS_DIR}/CesTests.cpp"
	LINK common fmt::fmt SDL3::SDL3-static
)
target_include_directories(ces_tests PRIVATE ${inc_headers})

kyty_add_test_executable(save_data_dialog_tests
	SOURCES
	"${KYTY_TESTS_DIR}/SaveDataDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/dialog.cpp"
	LINK common fmt::fmt
)
target_include_directories(save_data_dialog_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(http_uri_parse_tests
	"${KYTY_TESTS_DIR}/HttpUriParseTests.cpp")

kyty_add_test_executable(event_queue_lifetime_tests
	SOURCES
	"${KYTY_TESTS_DIR}/EventQueueLifetimeTests.cpp"
	"${KYTY_SOURCE_DIR}/kernel/eventQueue.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
	LINK common fmt::fmt
)
target_include_directories(event_queue_lifetime_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(kernel_file_system_tests
	"${KYTY_TESTS_DIR}/KernelFileSystemTests.cpp")
target_link_libraries(kernel_file_system_tests ZArchive::zarchive)

kyty_add_test_executable(sync_on_address_tests
	SOURCES
	"${KYTY_TESTS_DIR}/SyncOnAddressTests.cpp"
	"${KYTY_SOURCE_DIR}/kernel/syncOnAddress.cpp"
	LINK common
)
target_include_directories(sync_on_address_tests PRIVATE ${inc_headers})

kyty_add_test_executable(image_page_table_tests
	SOURCES "${KYTY_TESTS_DIR}/ImagePageTableTests.cpp"
	LINK common fmt::fmt
)
target_include_directories(image_page_table_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(shader_recompiler_compute_tests "${KYTY_TESTS_DIR}/ShaderRecompilerComputeTests.cpp")

set(gpu_test_generated_dir "${PROJECT_BINARY_DIR}/gpu_test_shaders")
set(gpu_test_ms_depth_source
	"${gpu_tiler_shader_dir}/gpu_test_ms_depth.comp")
set(gpu_test_ms_depth_spv
	"${gpu_test_generated_dir}/gpu_test_ms_depth.spv")
set(gpu_test_ms_depth_header
	"${gpu_test_generated_dir}/gpu_test_ms_depth_spv.h")
add_custom_command(
	OUTPUT "${gpu_test_ms_depth_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_test_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -o "${gpu_test_ms_depth_spv}" "${gpu_test_ms_depth_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${gpu_test_ms_depth_spv}
	        -DOUTPUT=${gpu_test_ms_depth_header} -DSYMBOL=GPU_TEST_MS_DEPTH_SPV
	        -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${gpu_test_ms_depth_source}"
	VERBATIM
)
target_sources(shader_recompiler_compute_tests PRIVATE
	"${gpu_test_ms_depth_header}")

add_kyty_full_emulator_test(virtual_memory_allocation_tests "${KYTY_TESTS_DIR}/VirtualMemoryAllocationTests.cpp")
target_compile_definitions(virtual_memory_allocation_tests PRIVATE
	KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS=1)

kyty_add_test_executable(sse4a_emulation_tests
	SOURCES
	"${KYTY_TESTS_DIR}/Sse4aEmulationTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
	"${KYTY_SOURCE_DIR}/loader/guestInstructionPatcher.cpp"
	LINK common fmt::fmt Zydis::Zydis xbyak::xbyak
)
target_include_directories(sse4a_emulation_tests PRIVATE ${inc_headers})

# These tests use exceptions.
foreach(kyty_exception_test scalar_provenance_tests resource_tracking_tests
		virtual_memory_allocation_tests)
	target_compile_options(${kyty_exception_test} PRIVATE -fexceptions)
endforeach()

kyty_add_test(NAME emulator_user_name_cli
	COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 1234567890123456)
kyty_add_test(NAME emulator_user_name_too_long
	COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 12345678901234567
	WILL_FAIL)
kyty_add_test(NAME emulator_present_mode_cli
	COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Immediate)
kyty_add_test(NAME emulator_present_mode_invalid
	COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Invalid
	WILL_FAIL)
kyty_add_test(NAME ime_dialog COMMAND $<TARGET_FILE:ime_dialog_tests>)
kyty_add_test(NAME shader_cfg COMMAND $<TARGET_FILE:shader_cfg_tests>)
kyty_add_test(NAME scalar_provenance COMMAND $<TARGET_FILE:scalar_provenance_tests>)
kyty_add_test(NAME image_page_table COMMAND $<TARGET_FILE:image_page_table_tests>)
kyty_add_test(NAME memory_tracker COMMAND $<TARGET_FILE:memory_tracker_tests>)
kyty_add_test(NAME page_manager COMMAND $<TARGET_FILE:page_manager_tests>)
kyty_add_test(NAME archive_file COMMAND $<TARGET_FILE:archive_file_tests>)
kyty_add_test(NAME trophy_system COMMAND $<TARGET_FILE:trophy_system_tests>)
kyty_add_test(NAME avplayer_file COMMAND $<TARGET_FILE:avplayer_file_tests>)
kyty_add_test(NAME vdecsw COMMAND $<TARGET_FILE:vdecsw_tests>)
kyty_add_test(NAME bit_array COMMAND $<TARGET_FILE:bit_array_tests>)
kyty_add_test(NAME lru_cache COMMAND $<TARGET_FILE:lru_cache_tests>)
kyty_add_test(NAME shader_vertex_metadata COMMAND $<TARGET_FILE:shader_vertex_metadata_tests>)
kyty_add_test(NAME resource_materialization COMMAND $<TARGET_FILE:resource_materialization_tests>)
kyty_add_test(NAME resource_tracking COMMAND $<TARGET_FILE:resource_tracking_tests>)
kyty_add_test(NAME event_queue_lifetime COMMAND $<TARGET_FILE:event_queue_lifetime_tests>)
# Creates a Vulkan instance (WindowContext::CreateVulkan).
kyty_add_test(NAME kernel_file_system COMMAND $<TARGET_FILE:kernel_file_system_tests>
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME sync_on_address COMMAND $<TARGET_FILE:sync_on_address_tests>)
kyty_add_test(NAME audio_out2_port COMMAND $<TARGET_FILE:audio_out2_port_tests>)
kyty_add_test(NAME audio_out_timing COMMAND $<TARGET_FILE:audio_out_timing_tests>)
kyty_add_test(NAME ngs2_sampler COMMAND $<TARGET_FILE:ngs2_sampler_tests>)
kyty_add_test(NAME pad_haptics COMMAND $<TARGET_FILE:pad_haptics_tests>)
kyty_add_test(NAME controller_settings COMMAND $<TARGET_FILE:controller_settings_tests>)
kyty_add_test(NAME host_input COMMAND $<TARGET_FILE:host_input_tests>)
kyty_add_test(NAME save_data_memory COMMAND $<TARGET_FILE:save_data_memory_tests>)
kyty_add_test(NAME png_enc COMMAND $<TARGET_FILE:png_enc_tests>)
kyty_add_test(NAME ces COMMAND $<TARGET_FILE:ces_tests>)
kyty_add_test(NAME save_data_dialog COMMAND $<TARGET_FILE:save_data_dialog_tests>)
kyty_add_test(NAME http_uri_parse COMMAND $<TARGET_FILE:http_uri_parse_tests>)

# shader_recompiler_compute_tests modes. The modes that construct VulkanHarness create a Vulkan
# instance and carry the gpu label; --tessellation-only, --draw-offset-only, --image-layout-only, and
# --depth-readback-only need no Vulkan device and stay unlabeled (Requirement 17.5).
kyty_add_test(NAME shader_recompiler_compute COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>
	LABELS gpu SKIP_RETURN_CODE 77 TIMEOUT 600)
kyty_add_test(NAME shader_recompiler_tessellation
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --tessellation-only)
kyty_add_test(NAME shader_recompiler_float_min_max
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --float-min-max-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME shader_recompiler_alignbyte
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --alignbyte-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME graphics_pipeline_rasterization
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME graphics_draw_offsets
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --draw-offset-only)
kyty_add_test(NAME virtual_memory_allocation
	COMMAND $<TARGET_FILE:virtual_memory_allocation_tests>)
kyty_add_test(NAME guest_red_zone_patcher
	COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --red-zone-patcher-only)
kyty_add_test(NAME command_scheduler_timeline
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --scheduler-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME stream_buffer_ring
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --stream-buffer-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME gpu_command_lane
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-command-lane-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME pm4_context_state
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --context-state-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME gpu_tiler
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-tiler-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME image_transfer_layout
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-layout-only)

kyty_add_test(NAME texture_cache_layered_image
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --layered-image-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME host_image_allocation
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --host-image-allocation-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME texture_cache_image_views
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-view-cache-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME texture_cache_storage_sampled
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --storage-sampled-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME texture_cache_depth_readback
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --depth-readback-only)
kyty_add_test(NAME compute_meta_clear_classification
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --compute-meta-clear-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME buffer_cache_dirty_gc
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-gc-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME color_render_target_1d
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --rt1d-only
	LABELS gpu SKIP_RETURN_CODE 77)

kyty_add_test(NAME texture_cache_image_overlap
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-overlap-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME texture_cache_htile_clear
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --htile-clear-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME buffer_cache_ranges
	COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-range-only
	LABELS gpu SKIP_RETURN_CODE 77)
kyty_add_test(NAME sse4a_emulation COMMAND $<TARGET_FILE:sse4a_emulation_tests> TIMEOUT 30)

# Every first-party executable listed by the configure-time lld check was linked by LLD.
kyty_add_test(NAME lld_linked_executables
	COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/checks/check_lld.py"
	        "${CMAKE_BINARY_DIR}/kyty_lld_executables.txt"
	TIMEOUT 60
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)

kyty_add_test_directory(tests/emulator LINK kyty_emulator_options)

# kyty_emulator --help and --redzone, checked by emulator_options_tests against the parser's own
# option table and UsageText().
if(TARGET emulator_options_tests)
	kyty_add_test(NAME emulator_help
		COMMAND $<TARGET_FILE:emulator_options_tests> --check-help $<TARGET_FILE:kyty_emulator>
		TIMEOUT 5)
	kyty_add_test(NAME emulator_redzone_rejected
		COMMAND $<TARGET_FILE:emulator_options_tests> --check-redzone $<TARGET_FILE:kyty_emulator>
		TIMEOUT 30)
endif()

# Infrastructure self-tests: cmake -P drivers in tests/cmake/. The configure guard tests configure this
# source tree again in fresh directories under selftests/ of this build tree, without network access
# (KYTY_PREFETCHED_SOURCES_DIR or a guard that stops first), and delete them when they pass.
set(kyty_selftests_dir "${CMAKE_CURRENT_BINARY_DIR}/selftests")
set(kyty_configure_guard_arguments
	"-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
	"-DGENERATOR=${CMAKE_GENERATOR}"
	"-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
	"-DC_COMPILER=${CMAKE_C_COMPILER}"
	"-DCXX_COMPILER=${CMAKE_CXX_COMPILER}")
set(kyty_configure_guard_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfigureGuardTests.cmake")
kyty_add_test(NAME configure_rejects_non_linux
	COMMAND "${CMAKE_COMMAND}" -DCASE=non_linux "-DWORK_DIR=${kyty_selftests_dir}/configure_rejects_non_linux"
	        ${kyty_configure_guard_arguments} -P "${kyty_configure_guard_script}"
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
kyty_add_test(NAME platform_guard
	COMMAND "${CMAKE_COMMAND}" "-DPLATFORM_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/KytyPlatform.cmake"
	        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PlatformGuardTest.cmake"
	TIMEOUT 60)
# The pristine dependency sources of this build, linked into a prefetched directory by the test.
kyty_dependency_prefetch_entries(kyty_prefetch_entries)
list(JOIN kyty_prefetch_entries "\n" kyty_prefetch_text)
file(CONFIGURE OUTPUT "${kyty_selftests_dir}/prefetch-entries.txt" CONTENT "${kyty_prefetch_text}\n" @ONLY)
kyty_add_test(NAME sdl_backend_guard
	COMMAND "${CMAKE_COMMAND}" -DCASE=sdl_backend "-DWORK_DIR=${kyty_selftests_dir}/sdl_backend_guard"
	        "-DPREFETCH_ENTRIES=${kyty_selftests_dir}/prefetch-entries.txt"
	        ${kyty_configure_guard_arguments} -P "${kyty_configure_guard_script}"
	TIMEOUT 600
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
# Every dependency that the nested configure (BUILD_TESTING=ON) declares.
get_property(kyty_active_dependencies GLOBAL PROPERTY KYTY_DEPENDENCIES)
list(JOIN kyty_active_dependencies "," kyty_active_dependencies)
kyty_add_test(NAME prefetched_missing_sources
	COMMAND "${CMAKE_COMMAND}" -DCASE=prefetched_missing
	        "-DWORK_DIR=${kyty_selftests_dir}/prefetched_missing_sources"
	        "-DDEPENDENCIES=${kyty_active_dependencies}"
	        ${kyty_configure_guard_arguments} -P "${kyty_configure_guard_script}"
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
# KYTY_ENABLE_COVERAGE=ON with gcc and g++ (Requirement 17.12).
find_program(KYTY_SELFTEST_GCC NAMES gcc)
find_program(KYTY_SELFTEST_GXX NAMES g++)
kyty_add_test(NAME coverage_requires_clang
	COMMAND "${CMAKE_COMMAND}" -DCASE=coverage_requires_clang
	        "-DWORK_DIR=${kyty_selftests_dir}/coverage_requires_clang"
	        "-DGCC=${KYTY_SELFTEST_GCC}" "-DGXX=${KYTY_SELFTEST_GXX}"
	        ${kyty_configure_guard_arguments} -P "${kyty_configure_guard_script}"
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
# property_harness_selftest_tests is defined in properties/.
kyty_add_test(NAME property_harness_selftest
	COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:property_harness_selftest_tests>"
	        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PropertyHarnessSelfTest.cmake"
	TIMEOUT 120)
# Checks exit 77 itself, so no label and no SKIP_RETURN_CODE.
kyty_add_test(NAME gpu_skip_without_device
	COMMAND "${CMAKE_COMMAND}" "-DWORK_DIR=${kyty_selftests_dir}/gpu_skip_without_device"
	        "-DSHADER_RECOMPILER_COMPUTE_TESTS=$<TARGET_FILE:shader_recompiler_compute_tests>"
	        "-DKERNEL_FILE_SYSTEM_TESTS=$<TARGET_FILE:kernel_file_system_tests>"
	        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectGpuSkip.cmake"
	TIMEOUT 120)

# Platform macro source check (Requirements 5.7, 5.10): the scanner library (also linked by the
# source_check_tests property group), the source_check executable, the scan of src and tests, and the
# scanner's example tests in source_check/.
add_library(kyty_source_check STATIC EXCLUDE_FROM_ALL
	"${CMAKE_CURRENT_SOURCE_DIR}/source_check/platformMacroScanner.cpp")
target_include_directories(kyty_source_check PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
kyty_add_test_executable(source_check
	SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source_check/SourceCheckMain.cpp"
	LINK kyty_source_check)
kyty_add_test(NAME source_check_platform_macros
	COMMAND $<TARGET_FILE:source_check> --root "${CMAKE_SOURCE_DIR}" src tests
	LABELS source-check)
kyty_add_test_directory(tests/source_check LINK kyty_source_check LABELS source-check)

# Repository content checks (Requirements 2.3, 3.7, 3.9, 4.1, 4.2, 4.8, 4.9) and license and
# copyright preservation checks (Requirements 21.1, 21.6) over this source tree.
foreach(kyty_check IN ITEMS nix_terms windows_terms macos_terms cmake_apple bundle_strings deleted_paths
		vscode_patterns)
	kyty_add_test(NAME content_check_${kyty_check}
		COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/checks/content_checks.py"
		        --repo "${CMAKE_SOURCE_DIR}" --check ${kyty_check}
		LABELS source-check
		TIMEOUT 120
		ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
endforeach()
foreach(kyty_check IN ITEMS license copyright)
	kyty_add_test(NAME preservation_${kyty_check}
		COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/checks/check_preservation.py"
		        --repo "${CMAKE_SOURCE_DIR}" --check ${kyty_check}
		LABELS source-check
		TIMEOUT 120
		ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
endforeach()

# Unit tests of the coverage tools in tools/coverage/ (report.py and check.py).
foreach(kyty_tool IN ITEMS report check)
	kyty_add_test(NAME coverage_${kyty_tool}_tool
		COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/coverage/tests"
		        -p test_${kyty_tool}.py -v
		TIMEOUT 120
		ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
endforeach()

# Last, so that the property groups see every library.
add_subdirectory(properties)
