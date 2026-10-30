# The Updater (design.md, "Updater interfaces"): kyty_updater_core, built whatever KYTY_BUILD_LAUNCHER
# is, and the kyty_updater helper executable, which the Flatpak package excludes (Requirement 13.4).
add_subdirectory(core)

if(KYTY_BUILD_LAUNCHER AND NOT KYTY_FLATPAK_BUILD)
	# The installer is copied outside the application before it runs. Keep it free of
	# Qt and application library dependencies so it can replace those files.
	add_executable(kyty_updater main.cpp UpdateInstaller.cpp)

	target_compile_options(kyty_updater PRIVATE -fexceptions)
	target_link_libraries(kyty_updater PRIVATE kyty_updater_core)
	kyty_link_with_lld(kyty_updater)

	install(TARGETS kyty_updater RUNTIME DESTINATION .)
endif()
