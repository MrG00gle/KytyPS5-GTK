# Install layouts (Requirements 13.4, 14.6, 14.7, 14.14). Included from the top-level CMakeLists.txt
# right after option(KYTY_FLATPAK_BUILD ...), before any dependency is declared, so a wrong value
# stops the configure before anything is downloaded.
#
#   KYTY_INSTALL_LAYOUT (STRING)
#       portable  Flat, the default: the executables and assets/ at the top of the prefix. Release
#                 archives and the Updater depend on it, so it installs what it always did, plus
#                 LICENSE and the license files under LICENSES/ (cmake/KytyLicenses.cmake).
#       fhs       bin/ for the executables, share/kyty/assets for the Emulator assets, and, with
#                 KYTY_BUILD_LAUNCHER, the desktop entry in share/applications, the metainfo in
#                 share/metainfo, and the icons in share/icons/hicolor, under the App_ID names.
#       KYTY_FLATPAK_BUILD=ON makes fhs the default. An explicit portable with it (on the command
#       line or in the cache) is an error.
#
# Sets, for the install rules of the directories that follow:
#   KYTY_APP_ID              io.github.MrG00gle.KytyPS5_GTK
#   KYTY_INSTALL_BINDIR      . (portable) or bin (fhs)
#   KYTY_INSTALL_ASSETSDIR   assets (portable) or share/kyty/assets (fhs)
#   KYTY_INSTALL_DATADIR     share (fhs installs the desktop entry, metainfo, and icons below it)
#   KYTY_INSTALL_LICENSEDIR  share/licenses/<App_ID> (fhs only)
include_guard(GLOBAL)

set(KYTY_APP_ID "io.github.MrG00gle.KytyPS5_GTK")

# The default applies only when the cache holds no value yet. A value given with -D, or kept from an
# earlier configure, is explicit and is checked as it is.
if(KYTY_FLATPAK_BUILD)
	set(kyty_install_layout_default fhs)
else()
	set(kyty_install_layout_default portable)
endif()
set(KYTY_INSTALL_LAYOUT "${kyty_install_layout_default}" CACHE STRING
	"portable (flat, the default) or fhs (bin/ and share/, the default with KYTY_FLATPAK_BUILD)")
set_property(CACHE KYTY_INSTALL_LAYOUT PROPERTY STRINGS portable fhs)

if(NOT KYTY_INSTALL_LAYOUT STREQUAL "portable" AND NOT KYTY_INSTALL_LAYOUT STREQUAL "fhs")
	message(FATAL_ERROR "KYTY_INSTALL_LAYOUT must be portable or fhs (got '${KYTY_INSTALL_LAYOUT}')")
endif()
if(KYTY_FLATPAK_BUILD AND NOT KYTY_INSTALL_LAYOUT STREQUAL "fhs")
	message(FATAL_ERROR "KYTY_FLATPAK_BUILD=ON requires KYTY_INSTALL_LAYOUT=fhs (it is "
		"'${KYTY_INSTALL_LAYOUT}'); pass -DKYTY_INSTALL_LAYOUT=fhs, or leave it unset in a new "
		"build directory")
endif()

set(KYTY_INSTALL_DATADIR "share")
set(KYTY_INSTALL_LICENSEDIR "${KYTY_INSTALL_DATADIR}/licenses/${KYTY_APP_ID}")
if(KYTY_INSTALL_LAYOUT STREQUAL "fhs")
	set(KYTY_INSTALL_BINDIR "bin")
	set(KYTY_INSTALL_ASSETSDIR "${KYTY_INSTALL_DATADIR}/kyty/assets")
else()
	set(KYTY_INSTALL_BINDIR ".")
	set(KYTY_INSTALL_ASSETSDIR "assets")
endif()

if(KYTY_FLATPAK_BUILD)
	message(STATUS "KytyInstallLayout: fhs (implied by KYTY_FLATPAK_BUILD)")
else()
	message(STATUS "KytyInstallLayout: ${KYTY_INSTALL_LAYOUT}")
endif()
unset(kyty_install_layout_default)
