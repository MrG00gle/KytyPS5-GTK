# The Updater (design.md, "Updater interfaces"): kyty_updater_core and the kyty_updater_installer
# library, built whatever KYTY_BUILD_LAUNCHER is, and the kyty_updater helper executable, which the
# Flatpak package excludes (Requirement 13.4).
add_subdirectory(core)

# The installer with injectable file operations and the kyty_updater command line (task 17.3). It
# keeps its exceptions, and it is a library so that the installer tests link it in every
# configuration.
add_library(kyty_updater_installer STATIC UpdateInstaller.cpp installFileOps.cpp updaterCommand.cpp)
target_compile_options(kyty_updater_installer PRIVATE -fexceptions -frtti)
target_link_libraries(kyty_updater_installer PUBLIC kyty_updater_core)

if(KYTY_BUILD_LAUNCHER AND NOT KYTY_FLATPAK_BUILD)
	# The installer is copied outside the application before it runs. Keep it free of
	# Qt and application library dependencies so it can replace those files.
	add_executable(kyty_updater main.cpp)

	target_compile_options(kyty_updater PRIVATE -fexceptions)
	target_link_libraries(kyty_updater PRIVATE kyty_updater_installer)
	kyty_link_with_lld(kyty_updater)

	install(TARGETS kyty_updater RUNTIME DESTINATION .)
endif()
