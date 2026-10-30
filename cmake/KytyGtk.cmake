# GTK launcher dependencies (task 19.1, Requirements 6.4 and 6.5).
#
#   kyty_require_gtk_modules()
#       Looks up gtk4 >= 4.14, libadwaita-1 >= 1.5, and libsoup-3.0 >= 3.4 through pkg-config, each
#       one separately, and stops the configure with one line per missing or too old module:
#       "kyty_launcher requires <module> >= <min> (found: <version or none>)". On success the
#       imported targets PkgConfig::KYTY_GTK4, PkgConfig::KYTY_ADW, and PkgConfig::KYTY_SOUP exist
#       (GLOBAL, so that tests/ can link libraries that use them). GLib and GIO come with gtk4's
#       Requires. Every configure queries pkg-config again (no cached result), so an upgrade or a
#       downgrade in an existing build directory is seen. In script mode (cmake -P) only the
#       versions are checked, because imported targets cannot be created there.
#
# Only src/launcher/gtk/CMakeLists.txt calls it, so a configure with KYTY_BUILD_LAUNCHER=OFF looks up
# none of these modules. The message names kyty_launcher, the executable's name from task 23.1 on.
include_guard(GLOBAL)

# <ID>|<pkg-config module>|<minimum version>
set(KYTY_GTK_MODULES
	"GTK4|gtk4|4.14"
	"ADW|libadwaita-1|1.5"
	"SOUP|libsoup-3.0|3.4")

macro(kyty_require_gtk_modules)
	find_package(PkgConfig QUIET)
	if(NOT PKG_CONFIG_FOUND)
		message(FATAL_ERROR "kyty_launcher requires pkg-config (found: none)")
	endif()
	if(CMAKE_SCRIPT_MODE_FILE)
		set(kyty_gtk_imported "")
	else()
		set(kyty_gtk_imported IMPORTED_TARGET GLOBAL)
	endif()
	set(kyty_gtk_failures "")
	foreach(kyty_gtk_entry IN LISTS KYTY_GTK_MODULES)
		string(REPLACE "|" ";" kyty_gtk_fields "${kyty_gtk_entry}")
		list(GET kyty_gtk_fields 0 kyty_gtk_id)
		list(GET kyty_gtk_fields 1 kyty_gtk_module)
		list(GET kyty_gtk_fields 2 kyty_gtk_minimum)
		# No version in the request: pkg-config then reports the installed version, which the
		# message names when it is too old. FindPkgConfig would reuse a cached result on a later
		# configure, which keeps a too old version after an upgrade and misses a downgrade in an
		# existing build directory, so every configure asks pkg-config again.
		unset(__pkg_config_checked_KYTY_${kyty_gtk_id} CACHE)
		pkg_check_modules(KYTY_${kyty_gtk_id} QUIET ${kyty_gtk_imported} ${kyty_gtk_module})
		if(NOT KYTY_${kyty_gtk_id}_FOUND)
			list(APPEND kyty_gtk_failures
				"kyty_launcher requires ${kyty_gtk_module} >= ${kyty_gtk_minimum} (found: none)")
		elseif(KYTY_${kyty_gtk_id}_VERSION VERSION_LESS kyty_gtk_minimum)
			list(APPEND kyty_gtk_failures
				"kyty_launcher requires ${kyty_gtk_module} >= ${kyty_gtk_minimum} (found: ${KYTY_${kyty_gtk_id}_VERSION})")
		endif()
	endforeach()
	if(kyty_gtk_failures)
		list(JOIN kyty_gtk_failures "\n" kyty_gtk_failures_text)
		message(FATAL_ERROR "${kyty_gtk_failures_text}")
	endif()
	message(STATUS "KytyGtk: gtk4 ${KYTY_GTK4_VERSION}, libadwaita-1 ${KYTY_ADW_VERSION}, "
		"libsoup-3.0 ${KYTY_SOUP_VERSION}")
endmacro()
