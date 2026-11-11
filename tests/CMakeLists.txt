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

kyty_add_test_directory(tests/emulator LINK kyty_emulator_options kyty_vulkan_device_selection)

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
# Install layouts (task 26.1, cmake/KytyInstallLayout.cmake): an unknown KYTY_INSTALL_LAYOUT and an
# explicit portable layout with KYTY_FLATPAK_BUILD=ON stop the configure before any dependency is
# declared; install_layout_tree installs this build tree and checks the first-party files of its
# layout.
kyty_add_test(NAME install_layout_rejects_unknown
	COMMAND "${CMAKE_COMMAND}" -DCASE=install_layout_unknown
	        "-DWORK_DIR=${kyty_selftests_dir}/install_layout_rejects_unknown"
	        ${kyty_configure_guard_arguments} -P "${kyty_configure_guard_script}"
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
kyty_add_test(NAME install_layout_flatpak_requires_fhs
	COMMAND "${CMAKE_COMMAND}" -DCASE=install_layout_flatpak_portable
	        "-DWORK_DIR=${kyty_selftests_dir}/install_layout_flatpak_requires_fhs"
	        ${kyty_configure_guard_arguments} -P "${kyty_configure_guard_script}"
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
kyty_add_test(NAME install_layout_tree
	COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
	        "-DCONFIG=$<CONFIG>" "-DLAYOUT=${KYTY_INSTALL_LAYOUT}" "-DFLATPAK=${KYTY_FLATPAK_BUILD}"
	        "-DLAUNCHER=${KYTY_BUILD_LAUNCHER}" "-DAPP_ID=${KYTY_APP_ID}"
	        "-DWORK_DIR=${kyty_selftests_dir}/install_layout_tree"
	        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallLayoutTest.cmake"
	TIMEOUT 120)
# License files (tasks 26.3 and 26.5, cmake/KytyLicenses.cmake): the manifest that this configure wrote,
# and the unit tests of tools/checks/check_licenses.py, which checks an installed licenses directory
# against it.
kyty_add_test(NAME licenses_manifest
	COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
	        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LicensesManifestTest.cmake"
	TIMEOUT 60)
kyty_add_test(NAME check_licenses_tool
	COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/checks/tests"
	        -p test_check_licenses.py -v
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
# The CI JUnit check (task 28.1, tools/checks/junit_check.py): its unit tests, which also run this
# CMake and CTest on a generated scratch project to pin the JUnit format CTest writes. They need no
# build output, so they are registered with KYTY_BUILD_LAUNCHER OFF too.
kyty_add_test(NAME junit_check_tool
	COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/checks/tests"
	        -p test_junit_check.py -v
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1 "KYTY_CMAKE=${CMAKE_COMMAND}" "KYTY_CTEST=${CMAKE_CTEST_COMMAND}"
	            "KYTY_CMAKE_GENERATOR=${CMAKE_GENERATOR}" "KYTY_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
# Flatpak packaging checks (task 26.6, tools/checks/flatpak_checks.py): the manifest's pins against
# the registry entries active with BUILD_TESTING=OFF (read through
# tools/checks/dependency_registry.cmake), the manifest's structure and permission set, and the
# metainfo, desktop entry, and icons; then the unit tests over mutated copies. PyYAML is required.
# They need no build output, so they are registered with KYTY_BUILD_LAUNCHER OFF too.
set(kyty_flatpak_checks pins manifest metainfo)
set(kyty_flatpak_check_names flatpak_pins_match_registry flatpak_manifest_structure flatpak_metainfo_content)
foreach(kyty_check kyty_check_name IN ZIP_LISTS kyty_flatpak_checks kyty_flatpak_check_names)
	kyty_add_test(NAME ${kyty_check_name}
		COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/checks/flatpak_checks.py"
		        --repo "${CMAKE_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}" --check ${kyty_check}
		LABELS source-check
		TIMEOUT 120
		ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
endforeach()
kyty_add_test(NAME flatpak_checks_tool
	COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/checks/tests"
	        -p test_flatpak_checks.py -v
	TIMEOUT 120
	ENVIRONMENT PYTHONDONTWRITEBYTECODE=1 "KYTY_CMAKE=${CMAKE_COMMAND}")
# The dependency registry (cmake/KytyDependencies.cmake, cmake/KytyTargets.cmake) in generated
# projects that use local git repositories and prefetched directories, without network access.
foreach(kyty_case IN ITEMS hidden_entries patched_root online_after_prefetched
		prefetched_not_first_party)
	kyty_add_test(NAME dependency_registry_${kyty_case}
		COMMAND "${CMAKE_COMMAND}" -DCASE=${kyty_case} "-DMODULE_DIR=${CMAKE_SOURCE_DIR}/cmake"
		        "-DPATCHES_DIR=${CMAKE_SOURCE_DIR}/3rdparty/patches"
		        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" "-DGENERATOR=${CMAKE_GENERATOR}"
		        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
		        "-DWORK_DIR=${kyty_selftests_dir}/dependency_registry_${kyty_case}"
		        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DependencyRegistryTests.cmake"
		TIMEOUT 120)
endforeach()
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

# Repository content checks (Requirements 2.3, 3.7, 3.9, 4.1, 4.2, 4.8, 4.9; gtk_settings_save: the
# GTK launcher saves Kyty.ini only through SaveSettingsStore; task 23.2: qt_remnants 6.1 and 6.3,
# translatable 6.12, dialog_registry 17.10, and layering 6.7 as source_check_layering, the design's
# name) and license and copyright preservation checks (Requirements 21.1, 21.6) over this source
# tree. They need no build output, so they are registered with KYTY_BUILD_LAUNCHER OFF too.
foreach(kyty_check IN ITEMS nix_terms windows_terms macos_terms cmake_apple bundle_strings deleted_paths
		vscode_patterns gtk_settings_save qt_remnants translatable dialog_registry layering)
	set(kyty_check_name content_check_${kyty_check})
	if(kyty_check STREQUAL "layering")
		set(kyty_check_name source_check_layering)
	endif()
	kyty_add_test(NAME ${kyty_check_name}
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

# Process test helpers (task 15.6): argv_echo writes its arguments NUL-terminated and fake_emulator
# stands in for the Emulator in spawn tests (tests/support/helperPaths.h finds both). Each one has a
# self-test (Requirement 17.4) that checks its output bytes, exit codes, and signal deaths.
kyty_add_test_executable(argv_echo SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/helpers/ArgvEcho.cpp")
kyty_add_test_executable(fake_emulator SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/helpers/FakeEmulator.cpp")
foreach(kyty_helper IN ITEMS argv_echo fake_emulator)
	kyty_add_test(NAME ${kyty_helper}_selftest
		COMMAND "${CMAKE_COMMAND}" -DCASE=${kyty_helper} "-DEXE=$<TARGET_FILE:${kyty_helper}>"
		        "-DWORK_DIR=${kyty_selftests_dir}/${kyty_helper}_selftest"
		        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProcessHelperSelfTest.cmake")
endforeach()

# Launcher_Core tests run without a display (Requirement 6.7). CTest cannot unset a variable through
# ENVIRONMENT, so they get these ENVIRONMENT_MODIFICATION operations (also used in properties/).
set(kyty_no_display "WAYLAND_DISPLAY=unset:" "DISPLAY=unset:")

# Test support without application libraries (the HTTP and scheduler fakes, temporary directories),
# shared by the Launcher_Core and Updater tests. It links only kyty_net_interfaces, so the Updater
# tests stay free of application libraries. It compiles with -frtti because property executables
# derive from its classes.
set(kyty_net_test_support_sources
	"${CMAKE_CURRENT_SOURCE_DIR}/support/fakeHttpClient.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/support/fakeScheduler.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/support/tempDir.cpp")
add_library(kyty_net_test_support STATIC EXCLUDE_FROM_ALL ${kyty_net_test_support_sources})
target_include_directories(kyty_net_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kyty_net_test_support PUBLIC kyty_net_interfaces)
target_compile_options(kyty_net_test_support PRIVATE -frtti)
# Tests run in ${CMAKE_BINARY_DIR}; this finds tests/fixtures/.
target_compile_definitions(kyty_net_test_support PUBLIC "KYTY_TEST_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# Where argv_echo and fake_emulator are written (tests/support/helperPaths.h); building the support
# library builds them first.
target_compile_definitions(kyty_net_test_support PUBLIC "KYTY_TEST_BINARY_DIR=\"${CMAKE_BINARY_DIR}\"")
add_dependencies(kyty_net_test_support argv_echo fake_emulator)

# Shared Launcher_Core test support (fakes of the scan file system and environment): every other
# tests/support/*.cpp except the property harness main(), which kyty_add_property_executable()
# compiles into each property executable. It also compiles with -frtti.
file(GLOB kyty_test_support_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp")
list(FILTER kyty_test_support_sources EXCLUDE REGEX "/propertyMain\\.cpp$")
list(REMOVE_ITEM kyty_test_support_sources ${kyty_net_test_support_sources})
add_library(kyty_test_support STATIC EXCLUDE_FROM_ALL ${kyty_test_support_sources})
target_include_directories(kyty_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kyty_test_support PUBLIC kyty_net_test_support kyty_launcher_core)
target_compile_options(kyty_test_support PRIVATE -frtti)

kyty_add_test_directory(tests/launcher_core LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
# jsonErrorCapture.h is internal to Launcher_Core: it includes nlohmann/json.hpp, which
# kyty_launcher_core links PRIVATE. Its test includes it too, so it needs the header-only library.
target_link_libraries(launcher_core_json_error_capture_tests nlohmann_json::nlohmann_json)

# Updater core tests (task 17.5). They link no application library (UPDATER-LINK) and need no
# display-free environment: kyty_updater_core has no display code.
kyty_add_test_directory(tests/updater LINK kyty_updater_installer kyty_updater_core
	kyty_net_test_support)
# The installer reports failures as exceptions (installFileOps.h): the rollback tests' failing
# InstallFileOps throws them, and the command tests catch what RunUpdaterCommand throws, as main()
# does.
target_compile_options(updater_installer_rollback_tests PRIVATE -fexceptions)
target_compile_options(updater_command_tests PRIVATE -fexceptions)

# Display-free tests of the GTK launcher's helpers and services (plan D11): they never call
# gtk_init, run without a display and without a session bus, and use GLib's in-memory settings, the
# local VFS, and no proxy resolver. kyty_launcher_gtk exists only with KYTY_BUILD_LAUNCHER=ON, so the
# directory is OPTIONAL only when it is OFF; with ON a missing link target fails the configure.
set(kyty_launcher_gtk_optional "")
if(NOT KYTY_BUILD_LAUNCHER)
	set(kyty_launcher_gtk_optional OPTIONAL)
endif()
# The fakes of the GTK launcher's services and the LauncherContext services built from them, for
# the display-free GTK tests and the UI tests (support/gtk/, outside the support/*.cpp glob above).
# Without kyty_launcher_gtk there is nothing to fake: the sources are then exempt from the "not
# compiled into a registered test executable" check, as the skipped test directories are.
file(GLOB kyty_gtk_test_support_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/support/gtk/*.cpp")
if(TARGET kyty_launcher_gtk)
	add_library(kyty_gtk_test_support STATIC EXCLUDE_FROM_ALL ${kyty_gtk_test_support_sources})
	target_link_libraries(kyty_gtk_test_support PUBLIC kyty_launcher_gtk kyty_test_support)
	target_compile_options(kyty_gtk_test_support PRIVATE -frtti)
else()
	set_property(GLOBAL APPEND PROPERTY KYTY_TESTING_OPTIONAL_SKIPPED_SOURCES ${kyty_gtk_test_support_sources})
endif()
# SDL_AUDIO_DRIVER=dummy: a title sound that a test plays goes to SDL's dummy audio device.
kyty_add_test_directory(tests/launcher_gtk ${kyty_launcher_gtk_optional}
	LINK kyty_launcher_gtk kyty_test_support kyty_gtk_test_support
	ENVIRONMENT GSETTINGS_BACKEND=memory GIO_USE_VFS=local GIO_USE_PROXY_RESOLVER=dummy
	            SDL_AUDIO_DRIVER=dummy
	ENVIRONMENT_MODIFICATION ${kyty_no_display} DBUS_SESSION_BUS_ADDRESS=unset:)

# UI tests of the GTK launcher (task 22.1; phase 7 plan D6-D8): launcher_ui_tests runs one case per
# test, for Wayland and for X11, inside ui/with-display.sh (a private session bus, the fake portal
# ui_fake_portal, and weston or Xvfb; 77 only when a server executable is missing). The portal
# uses no launcher code; it links kyty_launcher_gtk for GIO and the GTK API floor. OPTIONAL only
# when KYTY_BUILD_LAUNCHER is OFF, as for tests/launcher_gtk.
kyty_add_ui_tests(tests/ui ${kyty_launcher_gtk_optional}
	EXECUTABLE launcher_ui_tests
	PORTAL ui_fake_portal
	PORTAL_LINK kyty_launcher_gtk
	WRAPPER "${CMAKE_CURRENT_SOURCE_DIR}/ui/with-display.sh"
	BACKENDS wayland x11
	TIMEOUT 120
	LINK kyty_launcher_gtk kyty_test_support kyty_gtk_test_support)
# The harness self-test (task 22.6; phase 7 plan D38): launcher_ui_tests --selftest <kind> inside
# ui/with-display.sh on both backends (a critical or a warning fails and is printed, a declared
# warning passes, a declared warning that never comes fails), and the wrapper's 77 for a missing
# server. A UI test, so label ui and SKIP_RETURN_CODE 77, which it returns only when this host lacks
# a display server.
if(TARGET launcher_ui_tests)
	kyty_add_test(NAME ui_harness_selftest
		COMMAND "${CMAKE_COMMAND}" "-DBASH=${KYTY_UI_TESTS_BASH}"
		        "-DWRAPPER=${CMAKE_CURRENT_SOURCE_DIR}/ui/with-display.sh"
		        "-DPORTAL=$<TARGET_FILE:ui_fake_portal>" "-DEXE=$<TARGET_FILE:launcher_ui_tests>"
		        "-DWORK_DIR=${kyty_selftests_dir}/ui_harness_selftest"
		        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/UiHarnessSelfTest.cmake"
		LABELS ui
		SKIP_RETURN_CODE 77
		TIMEOUT 300)
	# The documentation screenshot (task 26.2, Requirement 14.8; phase 8 plan D21): the case
	# docs_main_window_screenshot on Wayland writes docs/screenshots/kyty-launcher-main.png, which
	# the metainfo's screenshot URL names. Not part of all: build the target after a visible change
	# of the main window and commit the new file.
	add_custom_target(update-docs-screenshot
		COMMAND "${KYTY_UI_TESTS_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/with-display.sh"
		        --portal "$<TARGET_FILE:ui_fake_portal>" wayland --
		        "$<TARGET_FILE:launcher_ui_tests>" --case docs_main_window_screenshot
		        --screenshot-out "${CMAKE_SOURCE_DIR}/docs/screenshots/kyty-launcher-main.png"
		COMMENT "Writing docs/screenshots/kyty-launcher-main.png"
		VERBATIM)
	add_dependencies(update-docs-screenshot launcher_ui_tests ui_fake_portal)
endif()
# The launcher without a display (task 22.6, Requirement 6.15; phase 7 plan D38): exit 1 within 5 s,
# the message on stderr, the Settings_Store unchanged. It starts no display server: no label.
if(TARGET kyty_launcher)
	kyty_add_test(NAME launcher_no_display
		COMMAND "${CMAKE_COMMAND}" "-DLAUNCHER=$<TARGET_FILE:kyty_launcher>"
		        "-DWORK_DIR=${kyty_selftests_dir}/launcher_no_display"
		        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LauncherNoDisplayTest.cmake"
		TIMEOUT 60)
endif()
# The GTK module check of the configure step (task 23.2, Requirement 6.5; phase 7 plan D37):
# cmake/KytyGtk.cmake with fake pkg-config modules. It needs pkg-config, not GTK.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
	kyty_add_test(NAME gtk_pkgconfig_guard
		COMMAND "${CMAKE_COMMAND}" "-DPKG_CONFIG=${PKG_CONFIG_EXECUTABLE}"
		        "-DMODULE_DIR=${CMAKE_SOURCE_DIR}/cmake"
		        "-DWORK_DIR=${kyty_selftests_dir}/gtk_pkgconfig_guard"
		        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GtkPkgconfigGuardTest.cmake"
		TIMEOUT 60)
endif()

# Build classification (task 17.6, Requirement 13.2): kyty_classify_build() in script mode over every
# combination of its inputs.
kyty_add_test(NAME build_classification
	COMMAND "${CMAKE_COMMAND}" "-DBUILD_INFO_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/KytyBuildInfo.cmake"
	        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildClassificationTest.cmake"
	TIMEOUT 60)

# Last, so that the property groups see every library.
add_subdirectory(properties)
