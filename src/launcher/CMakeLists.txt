
find_package(Qt6 6.8 COMPONENTS Concurrent Network Widgets REQUIRED)

set(CMAKE_AUTOUIC_SEARCH_PATHS forms)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

file(GLOB launcher_src
	"include/*.h"
	"src/*.cpp"
)

file(GLOB launcher_forms
	"forms/*.ui"
)

add_executable(launcher
	${launcher_src}
	${launcher_forms}
	resources.qrc
)


target_link_libraries(launcher Qt6::Concurrent Qt6::Network Qt6::Widgets Vulkan::Headers SDL3::SDL3-static
	LibAtrac9 kyty_git_version common)
if(LINUX)
	find_package(Qt6 COMPONENTS DBus REQUIRED)
	target_link_libraries(launcher Qt6::DBus)
endif()
if (CLANG)
target_compile_options(launcher PRIVATE -frtti)
target_link_libraries(launcher pthread)
endif()
target_include_directories(launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(launcher PROPERTIES AUTOUIC ON AUTOMOC ON AUTORCC ON)

if(LINUX)
	# Use the same linker as kyty_emulator.
	kyty_link_with_lld(launcher)
endif()

add_dependencies(launcher kyty_emulator)
# src/updater/CMakeLists.txt skips kyty_updater in Flatpak builds.
if(TARGET kyty_updater)
	add_custom_target(stage_updater
		COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:launcher>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:kyty_updater>
			$<TARGET_FILE_DIR:launcher>/$<TARGET_FILE_NAME:kyty_updater>
		DEPENDS kyty_updater
		VERBATIM)
	add_dependencies(launcher stage_updater)
endif()

set(launcher_name "kyty_launcher")

set_target_properties(launcher PROPERTIES OUTPUT_NAME ${launcher_name})

install(TARGETS launcher
	RUNTIME DESTINATION .)

# Older releases require the previous executable name when validating updates.
install(CODE "
		file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}\"
			TYPE PROGRAM RENAME \"launcher${CMAKE_EXECUTABLE_SUFFIX}\"
			FILES \"\${CMAKE_INSTALL_PREFIX}/${launcher_name}${CMAKE_EXECUTABLE_SUFFIX}\")
	")

if(LINUX)
	# Bundle the Qt runtime and plugins.
	set_target_properties(launcher PROPERTIES
		INSTALL_RPATH "$ORIGIN/lib"
		BUILD_WITH_INSTALL_RPATH FALSE)
	target_link_options(launcher PRIVATE -Wl,--disable-new-dtags)

	# Support distribution and official Qt layouts.
	get_target_property(kyty_qt_core_lib Qt6::Core IMPORTED_LOCATION_RELEASE)
	if(NOT kyty_qt_core_lib)
		get_target_property(kyty_qt_core_lib Qt6::Core IMPORTED_LOCATION)
	endif()
	get_filename_component(kyty_qt_libdir "${kyty_qt_core_lib}" DIRECTORY)

	set(kyty_qt_plugins_dir "")
	foreach(kyty_candidate
			"${kyty_qt_libdir}/qt6/plugins"
			"${kyty_qt_libdir}/../plugins"
			"${Qt6_DIR}/../../../plugins")
		if(IS_DIRECTORY "${kyty_candidate}/platforms")
			get_filename_component(kyty_qt_plugins_dir "${kyty_candidate}" ABSOLUTE)
			break()
		endif()
	endforeach()

	if(kyty_qt_plugins_dir)
		message(STATUS "Qt plugins for launcher deployment: ${kyty_qt_plugins_dir}")
		set(kyty_qt_plugin_files "")
		foreach(kyty_qt_plugin platforms tls imageformats xcbglintegrations wayland-shell-integration)
			if(IS_DIRECTORY "${kyty_qt_plugins_dir}/${kyty_qt_plugin}")
				file(GLOB kyty_qt_plugin_group
					"${kyty_qt_plugins_dir}/${kyty_qt_plugin}/*.so"
					"${kyty_qt_plugins_dir}/${kyty_qt_plugin}/*.so.*")
				list(APPEND kyty_qt_plugin_files ${kyty_qt_plugin_group})
				install(DIRECTORY "${kyty_qt_plugins_dir}/${kyty_qt_plugin}" DESTINATION "plugins")
			endif()
		endforeach()
		install(CODE "
			set(kyty_plugin_files \"${kyty_qt_plugin_files}\")
			file(GET_RUNTIME_DEPENDENCIES
				EXECUTABLES \"${CMAKE_CURRENT_BINARY_DIR}/${launcher_name}\"
				LIBRARIES \${kyty_plugin_files}
				RESOLVED_DEPENDENCIES_VAR kyty_resolved
				POST_INCLUDE_REGEXES \"libQt6\" \"libicu\"
				POST_EXCLUDE_REGEXES \".*\")
			foreach(kyty_dep \${kyty_resolved})
				file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\"
					TYPE SHARED_LIBRARY FOLLOW_SYMLINK_CHAIN FILES \"\${kyty_dep}\")
			endforeach()
		")
		install(CODE "
			file(WRITE \"\${CMAKE_INSTALL_PREFIX}/qt.conf\" \"[Paths]\\nPlugins = plugins\\n\")
		")
	else()
		message(WARNING "Qt plugin directory not found; the installed launcher will need a system Qt")
	endif()
endif()


set(tidy_dirs "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(iwyu_maps "${CMAKE_CURRENT_SOURCE_DIR}/utils/qt6_16.imp")

get_property(inc_headers TARGET launcher PROPERTY INCLUDE_DIRECTORIES)

list(APPEND inc_headers
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}/launcher_autogen/include
	${Qt6Widgets_INCLUDE_DIRS}
	${Qt6Core_INCLUDE_DIRS}
	${Qt6Gui_INCLUDE_DIRS}
	#${Qt6_DIR}/../../../include
	#${Qt6_DIR}/../../../include/QtWidgets
	#${Qt6_DIR}/../../../include/QtCore
	#${Qt6_DIR}/../../../include/QtGui
)

include_what_you_use_with_mappings(launcher "${inc_headers}" "${iwyu_maps}")

clang_tidy_check(launcher "" "${tidy_dirs}" "${inc_headers}")
