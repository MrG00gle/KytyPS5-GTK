set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(cpuinfo)

# Configure-time dependencies come from the registry, downloaded or from KYTY_PREFETCHED_SOURCES_DIR.
include(KytyDependencies)
kyty_check_prefetched_sources()

# The Bluetooth DualSense transports speaker audio as fixed-size Opus packets.
set(kyty_build_testing_saved "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
kyty_make_dependency_available(opus)
set(BUILD_TESTING "${kyty_build_testing_saved}")
install(FILES "${opus_SOURCE_DIR}/COPYING" DESTINATION "licenses/opus")

# Headsets are driven through OpenXR. Only the static loader is built; the runtime that serves it
# (for example SteamVR, Monado or Virtual Desktop) comes from the user's system.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
kyty_make_dependency_available(openxr)
# Keep the loader's headers and library out of the installed package.
set_property(DIRECTORY "${openxr_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
# The loader uses exceptions internally.
target_compile_options(openxr_loader PRIVATE -fexceptions)
install(FILES "${openxr_SOURCE_DIR}/LICENSE" DESTINATION "licenses/openxr")

# The guest instruction patcher needs a decoder/encoder and a small
# trampoline assembler. Pin the same revisions used by shadPS4's implementation.
set(XBYAK_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)

kyty_make_dependency_available(xbyak)
kyty_make_dependency_available(zydis)
target_compile_definitions(xbyak INTERFACE XBYAK_NO_EXCEPTION)

# ZArchive provides random-access reads for compressed game dumps. Build only
# the reader/writer library and its zstd dependency; the command-line tool is
# not part of KytyPS5's runtime distribution.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
kyty_make_dependency_available(zstd)
# The registry applies patches/zarchive-reader.patch to the build-tree copy that
# zarchive_source_SOURCE_DIR names.
kyty_make_dependency_available(zarchive_source)

add_library(zarchive STATIC
	"${zarchive_source_SOURCE_DIR}/src/zarchivereader.cpp"
	"${zarchive_source_SOURCE_DIR}/src/zarchivewriter.cpp"
	"${zarchive_source_SOURCE_DIR}/src/sha_256.c"
)
add_library(ZArchive::zarchive ALIAS zarchive)
target_include_directories(zarchive SYSTEM PUBLIC "${zarchive_source_SOURCE_DIR}/include")
target_include_directories(zarchive PRIVATE "${zstd_SOURCE_DIR}/lib")
target_link_libraries(zarchive PRIVATE libzstd_static)
target_compile_features(zarchive PRIVATE cxx_std_20)
target_compile_options(zarchive PRIVATE -w)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
# The emulator needs these SDL3 backends on Linux.
set(kyty_sdl_required_backends ALSA PULSEAUDIO WAYLAND X11 LIBUDEV DBUS)
foreach(kyty_sdl_backend IN LISTS kyty_sdl_required_backends)
	set(SDL_${kyty_sdl_backend} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(SDL3)
# SDL3 sets HAVE_<backend> in its own directory scope when a backend is enabled. These are the
# values its "SDL_<backend> (Wanted: ON): ON" summary lines print.
set(kyty_sdl_missing_backends "")
foreach(kyty_sdl_backend IN LISTS kyty_sdl_required_backends)
	get_directory_property(kyty_sdl_have DIRECTORY SDL3 DEFINITION HAVE_${kyty_sdl_backend})
	if(NOT kyty_sdl_have)
		list(APPEND kyty_sdl_missing_backends "SDL_${kyty_sdl_backend}")
	endif()
endforeach()
if(kyty_sdl_missing_backends)
	list(JOIN kyty_sdl_missing_backends ", " kyty_sdl_missing_backends)
	message(FATAL_ERROR "SDL3 could not enable the required backends: ${kyty_sdl_missing_backends}. "
		"Install their development packages and reconfigure.")
endif()

add_library(Vulkan-Headers INTERFACE)
add_library(Vulkan::Headers ALIAS Vulkan-Headers)
target_include_directories(Vulkan-Headers INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include"
)

add_library(imgui STATIC
	imgui/imgui.cpp
	imgui/imgui_draw.cpp
	imgui/imgui_tables.cpp
	imgui/imgui_widgets.cpp
	imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/imgui"
	"${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
)
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(imgui PUBLIC Vulkan::Headers)
target_compile_options(imgui PRIVATE -w)

set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Headers" CACHE PATH "" FORCE)
add_subdirectory(SPIRV-Tools)

add_library(spirv-tools ALIAS SPIRV-Tools-static)
add_library(spirv-tools-opt ALIAS SPIRV-Tools-opt)

add_subdirectory(xxHash/build/cmake)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_MODULE OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(fmt EXCLUDE_FROM_ALL)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
add_subdirectory(nlohmann_json EXCLUDE_FROM_ALL)

set(SPDLOG_BUILD_ALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_WARNINGS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
add_subdirectory(spdlog EXCLUDE_FROM_ALL)

if (NOT TARGET FFmpeg::ffmpeg)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
		set(ARCHITECTURE arm64)
	else()
		set(ARCHITECTURE x86_64)
	endif()
	if(KYTY_PREFETCHED_SOURCES_DIR)
		# ffmpeg-core downloads its archive with file(DOWNLOAD), which no FetchContent setting
		# redirects, so a patched build-tree copy unpacks the prefetched archive instead.
		kyty_make_dependency_available(ffmpeg_core_prebuilt)
		set(KYTY_FFMPEG_PREBUILT_ARCHIVE "${ffmpeg_core_prebuilt_ARCHIVE}")
		set(KYTY_FFMPEG_PREBUILT_SHA256 "${ffmpeg_core_prebuilt_SHA256}")
		file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-core/CMakeLists.txt" kyty_ffmpeg_core_sha256)
		kyty_patch_source_copy(ffmpeg-core
			SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-core"
			REVISION "CMakeLists.txt sha256 ${kyty_ffmpeg_core_sha256}"
			PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/ffmpeg-core-prefetch.patch"
			OUTPUT_VARIABLE kyty_ffmpeg_core_dir)
		add_subdirectory("${kyty_ffmpeg_core_dir}" "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-core")
	else()
		add_subdirectory(ffmpeg-core)
		get_directory_property(kyty_ffmpeg_zip DIRECTORY ffmpeg-core DEFINITION FFMPEG_ZIP_PATH)
		if(kyty_ffmpeg_zip)
			kyty_dependency_set_source(ffmpeg_core_prebuilt "${kyty_ffmpeg_zip}")
		endif()
	endif()
	add_library(FFmpeg::ffmpeg ALIAS ffmpeg)
endif()

file(GLOB LIBATRAC9_SOURCES LibAtrac9/C/src/*.c)
add_library(LibAtrac9 STATIC ${LIBATRAC9_SOURCES})
target_include_directories(LibAtrac9 PUBLIC LibAtrac9/C/src)

set(TRACY_STATIC ON CACHE BOOL "" FORCE)
set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_DELAYED_INIT ON CACHE BOOL "" FORCE)
set(TRACY_MANUAL_LIFETIME ON CACHE BOOL "" FORCE)
add_subdirectory(tracy EXCLUDE_FROM_ALL)
if (TARGET TracyClient)
	if (CLANG)
		target_compile_options(TracyClient PRIVATE
			-Wno-error
			-Wno-braced-scalar-init
			-Wno-format
			-Wno-unused-variable
		)
	endif()
endif()
