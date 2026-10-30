# Property test groups (design.md, "Property test configuration"). Each group is one executable built
# from tests/properties/<group>/P<NN>_*.cpp, and each file becomes the CTest <group>_p<NN> with the
# property label (kyty_add_property_group in cmake/KytyTesting.cmake). A group is skipped until it has
# sources and every library it links exists. Add libraries to a LINK list as the group needs them.
# The Launcher_Core groups and emulator_launch_tests run with WAYLAND_DISPLAY and DISPLAY unset
# (kyty_no_display from tests/CMakeLists.txt, Requirement 6.7).
kyty_add_property_group(launcher_core_scan_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(launcher_core_library_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(launcher_core_compatibility_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(launcher_core_title_sound_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(launcher_core_configuration_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(emulator_launch_tests
	LINK kyty_emulator_options kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(launcher_core_run_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(launcher_core_content_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(launcher_core_paths_tests LINK kyty_launcher_core kyty_test_support
	ENVIRONMENT_MODIFICATION ${kyty_no_display})
kyty_add_property_group(archive_roundtrip_tests LINK common kyty_test_support)
kyty_add_property_group(updater_core_tests LINK kyty_updater_core kyty_net_test_support)
kyty_add_property_group(updater_installer_tests
	LINK kyty_updater_installer kyty_updater_core kyty_net_test_support)
kyty_add_property_group(source_check_tests LINK kyty_source_check)

# The deliberately failing property that the property_harness_selftest CTest (tests/CMakeLists.txt)
# runs.
kyty_add_property_executable(property_harness_selftest_tests
	SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/selftest/PropertyHarnessSelfTest.cpp")
