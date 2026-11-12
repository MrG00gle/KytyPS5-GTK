#!/usr/bin/env python3
"""Repository content checks for the removed platforms (task 8.3), the GTK settings rule, the
Qt removal and GTK launcher rules (task 23.2), the launcher's own texts for core errors (phase
9 V1), and the documentation (task 29.7).

Usage:
    content_checks.py --repo DIR --check ID

Each check reads the files that `git ls-files --cached --others
--exclude-standard` lists in DIR (tracked files plus untracked files that are
not ignored), regular text files only: symlinks, missing files, and files with
a NUL byte in their first 8000 bytes are skipped, as git grep -I does. Every
match is printed as `path:line: text`. The checks mirror the phase 2 scans:

    nix_terms        Requirement 2.3: the whole-word Nix terms anywhere except
                     3rdparty/, .kiro/, and the README "## Fork changes"
                     section (up to the next "## " heading).
    windows_terms    Requirement 3.9: the whole-word Windows terms in the
                     top-level CMakeLists.txt, 3rdparty/CMakeLists.txt,
                     .github/workflows/, src/, tests/, and .vscode/.
    macos_terms      Requirement 4.9: the three Apple platform terms as
                     substrings in src/, tests/, .github/, and the three top
                     CMake files.
    cmake_apple      Requirement 4.2: whole-word APPLE and the Apple CMake
                     substrings in the three top CMake files and in every
                     CMakeLists.txt and *.cmake file under src/.
    bundle_strings   Requirement 4.8: the .app bundle extension, the bundle
                     directories, and the Vulkan portability library file
                     name in src/.
    deleted_paths    Requirements 2.1, 3.1, 4.1: the deleted paths do not
                     exist, and no file outside .kiro/ names the four deleted
                     Apple-only files.
    vscode_patterns  Requirement 3.7: the Windows patterns (case-insensitive)
                     in .vscode/.
    gtk_settings_save
                     Z3 / phase 6 rule: no call of kyty::core::SaveSettings
                     (a SaveSettings name followed by "(") in src/launcher/gtk/.
                     The GTK launcher saves Kyty.ini only through
                     SaveSettingsStore, which refuses to replace a file that
                     exists but could not be loaded.
    qt_remnants      Requirements 6.1, 6.3: no .ui, .qrc, or .qss file
                     outside 3rdparty/ and .kiro/; and in every
                     CMakeLists.txt, *.cmake, *.sh, and *.md file and every
                     file under .github/ outside 3rdparty/ and .kiro/, no Qt
                     package lookup (find_package of Qt or QT, a
                     pkg_check_modules, pkg_search_module, or pkg-config
                     lookup of a Qt5 or Qt6 module), Qt link target (Qt5::,
                     Qt6::, Qt::, Qt${...}::), Qt CMake command or variable,
                     Qt build or deployment tool, Qt installation action,
                     and no package manager command (apt-get, apt, dnf, yum,
                     pacman, zypper) that names a Qt package (qt5-, qt6-,
                     qtbase, libqt). A command that trailing backslashes
                     continue counts as one line, reported at its first
                     line. QT_EXEMPT lists the files that are exempt, with
                     the reason (none since the README rewrite).
    layering         Requirement 6.7 (CTest source_check_layering): no
                     #include of a GTK, GDK, GSK, libadwaita, GLib, GIO, or
                     libsoup header in src/launcher/core/ and src/updater/.
    translatable     Requirement 6.12: po/POTFILES.in is sorted, lists no
                     file twice, and lists only existing files; every file
                     under src/launcher/gtk/ that marks a string (_(, N_(,
                     C_(, NC_(, NGettext(, NGettextNoop(, or
                     translatable="yes") is listed (i18n.h, which defines
                     the markers, is exempt); every GtkBuilder interface in
                     src/launcher/gtk/ has domain="kyty-launcher", and each
                     text property, menu label attribute, and string list
                     item with text is translatable="yes" (or
                     translatable="no" with a comments attribute); and in
                     the C++ files of src/launcher/gtk/, no string literal
                     with a letter is passed, outside a nested call, as the
                     text argument of a GTK, libadwaita, or GMenu text
                     setter or of the alerts.h helpers (TEXT_ARGUMENTS),
                     unless its line or the call's first line ends with
                     "// not translated: <reason>". A heuristic: text built
                     elsewhere and passed in a variable is not seen.
    core_text        Requirement 6.12, phase 9 V1: no fixed English text of
                     Launcher_Core or the Updater reaches a GTK widget as it
                     is. In the C++ files of src/launcher/gtk/ (outside
                     services/, the library adapters, and coreMessageText.cpp,
                     which maps every core kind to the launcher's own texts),
                     no read of a .message, .reason, or .error member (also
                     through ->; a call such as .error() and an assignment are
                     not reads), no .error().c_str() (a core error string), no
                     .Error() (ArchiveJob's English error), no RefusalMessage(
                     call, and no core::k...Message or updater::k...Message
                     constant, unless it is inside the arguments of a log call
                     (g_debug, g_info, g_message, g_warning, g_critical) or its
                     line ends with "// core text: <why it is dynamic or not
                     shown>" (a GError's or SDL's text, a callback). Comments
                     and string literals are not read. A heuristic over names:
                     a core text copied into a variable of another name is not
                     seen.
    dialog_registry  Requirement 17.10: every G_DECLARE_FINAL_TYPE window
                     or dialog type in src/launcher/gtk/ (parent AdwWindow,
                     AdwApplicationWindow, GtkWindow, or AdwDialog) has each
                     of its kyty_<type>_new* constructors called in
                     src/launcher/gtk/dialogRegistry.cpp, and every alert
                     helper declared in a src/launcher/gtk/ header (returns
                     GtkWindow*, name starts with Show, Confirm, Ask, or
                     Choose) is called there. The generic helpers of
                     alerts.h also count when a helper that the registry
                     calls calls them.

The Documentation_Set checks (tasks 29.6 and 29.7) read README.md,
CONTRIBUTING.md, and every Markdown file under docs/. They read Markdown by
blocks: headings, list items, table rows, paragraphs, and fenced code blocks.

    readme_structure Requirements 19.1-19.6, 19.8-19.10, 20.9, 21.2, 21.3,
                     21.5 and design.md's heading order: exactly one "#"
                     heading, KytyPS5-GTK, and the "#" and "##" headings in
                     design.md's order; before the first "##" heading, one
                     sentence that calls KytyPS5-GTK a fork of upstream
                     KytyPS5 based on Kyty and links both repositories, the
                     Sony non-affiliation, and "only legally obtained" game
                     files; in "## Fork changes", a list item of its own for
                     each of the seven 19.2 topics; a paragraph, list, or
                     code block in each 19.5 topic section; at least one
                     build status badge, each naming a workflow of the fork
                     in both its image and its link; every GitHub releases
                     and issues URL under the fork's; "upstream KytyPS5" in
                     the text, sentence, or list item of every Discord,
                     upstream discussions, or kytyps5.github.io link;
                     "upstream" in the heading, sentence, list item, link
                     text, or alternative text of every KytyPS5 that is not
                     KytyPS5-GTK, outside URLs; the system requirement
                     statements; the flatpak install and flatpak run
                     commands; a relative link to every other
                     Documentation_Set file; the License statements and
                     links; and one Acknowledgments item per project.
    docs_terms       Requirements 19.7, 3.6, 4.5: in the Documentation_Set
                     outside the README Fork changes section, no
                     Removed_Platform_Term (case-sensitive whole words), no
                     word that begins with "qt" in any case, no .exe file
                     name, and none of the Apple terms of 4.5.
    issue_template   Requirements 20.11, 20.14: every file under
                     .github/ISSUE_TEMPLATE/ except config.yml is an issue
                     form with a required "distribution" input whose label
                     asks for the distribution and version, and a required
                     "installation-type" dropdown with exactly the options
                     Flatpak, release archive, and source build; no
                     Removed_Platform_Term anywhere in it; and no placeholder
                     or value with a .exe name, a drive-letter path, or a
                     backslash.
    packaging_lint_exceptions_documented
                     Requirements 14.11, 20.7: every lint ID in
                     packaging/flatpak/lint-exceptions.json, and every ID
                     that a workflow adds to a per-run copy with
                     tools/ci/lint_exceptions.py --add, appears in
                     docs/packaging.md outside code blocks, in a table row
                     with a justification sentence in another cell, or in a
                     paragraph or list item with a sentence besides the ID.

The forbidden terms are built from adjacent string literals, so that this file,
which is inside the scanned scope, does not contain them.

Exit status: 0 when the check passes, 1 when it finds a violation, 2 on a usage
or repository error.
"""

import argparse
import bisect
import json
import os
import posixpath
import re
import stat
import subprocess
import sys
import xml.parsers.expat

BINARY_PROBE_SIZE = 8000
WORD_CHARS = "A-Za-z0-9_"
FORK_CHANGES_HEADING = "## Fork changes"

NIX_TERMS = (
    "Nix" "OS", "nix" "-shell", "nix" " develop", "nix" " build", "nix" "pkgs",
    "flake" ".nix", "flake" ".lock", "shell" ".nix", "devshell" ".nix",
)
WINDOWS_TERMS = (
    "WIN" "32", "_WIN" "32", "KYTY_PLATFORM_" "WINDOWS", "_MSC" "_VER",
    "__MINGW" "32__", "__MINGW" "64__", "MS" "VC", "MIN" "GW",
    "KYTY_CLANG" "_CL", "clang" "-cl", "windeploy" "qt", "win" "pthread",
)
MACOS_TERMS = ("mac" "OS", "Molten" "VK", "Roset" "ta")
CMAKE_APPLE_WORDS = ("APP" "LE",)
CMAKE_APPLE_SUBSTRINGS = (
    "CMAKE_" "OSX_", "MAC" "OSX_", "code" "sign", "mac" "deployqt", "LD" "64",
)
BUNDLE_PATTERN = (r"\.ap" r"p\b|" + re.escape("Contents/" "MacOS") + "|"
                  + re.escape("Contents/" "Frameworks") + "|"
                  + re.escape("libMolten" "VK.dylib"))
DELETED_FILE_NAMES = (
    "macos_jit" ".entitlements", "Info.plist" ".in",
    "macosGuestAddressSpace" ".cpp", "macosSse4a" "Tests.cpp",
)
DELETED_PATHS = (
    # Requirement 2.1
    ("flake" ".nix", "2.1"), ("flake" ".lock", "2.1"), ("shell" ".nix", "2.1"),
    ("nix", "2.1"),
    # Requirement 3.1
    ("3rdparty/win" "pthread", "3.1"),
    ("src/common/platform/sysWindows" "Dbg.cpp", "3.1"),
    ("src/common/platform/sysWindows" "FileIO.cpp", "3.1"),
    ("src/common/platform/sysWindows" "Virtual.cpp", "3.1"),
    # Requirement 4.1
    ("src/" + DELETED_FILE_NAMES[0], "4.1"),
    ("src/launcher/" + DELETED_FILE_NAMES[1], "4.1"),
    ("src/kernel/" + DELETED_FILE_NAMES[2], "4.1"),
    ("tests/" + DELETED_FILE_NAMES[3], "4.1"),
)
VSCODE_PATTERN = ("cppvs" "dbg|" r"\.ex" r"e\b|(?:^|[^a-z])[a-z]:[/\\]|\\|"
                  + re.escape("clang" "-cl") + "|VsDeveloper" "Environment|"
                  "vc" "vars|VsDev" "Cmd|ms" "vc|windows" "-")
TOP_CMAKE_FILES = ("CMakeLists.txt", "3rdparty/CMakeLists.txt",
                   "3rdparty/cpuinfo/CMakeLists.txt")
# A call of SaveSettings (not SaveSettingsStore): the name as a whole word, then "(".
SETTINGS_SAVE_PATTERN = rf"(?<![{WORD_CHARS}])SaveSettings\s*\("

# qt_remnants: the Qt file types, and the Qt build, deployment, and installation words.
QT_FILE_EXTENSIONS = (".ui", ".qrc", ".qss")
QT_BUILD_PATTERN = re.compile(
    r"find_package\s*\(\s*(?i:qt)|\bQt(?:[56]|\$\{\w+\})?::|\bQt[56]_DIR\b|\bqt_add_|\bqt[56]_|"
    r"AUTO" r"MOC|AUTO" r"UIC|AUTO" r"RCC|install" r"-qt-action|aqt" r"install|"
    r"windeploy" r"qt|macdeploy" r"qt|(?i:\bq" r"make)")
QT_PACKAGE_MANAGER = re.compile(r"(?<![\w-])(?:apt-get|apt|dnf5?|yum|pacman|zypper)(?![\w-])")
QT_PACKAGE = re.compile(r"(?i)(?<![\w-])(?:qt[56]-|qtbase|libqt)")
# A pkg-config lookup of a Qt module (Qt6Widgets): a CMake pkg_check_modules or pkg_search_module
# call (up to its closing parenthesis), or a pkg-config command line.
QT_PKG_CONFIG_CALL = re.compile(r"\bpkg_(?:check_modules|search_module)\s*\(")
QT_PKG_CONFIG_TOOL = re.compile(r"(?<![\w-])pkg-?conf(?:ig)?(?![\w-])")
QT_PKG_CONFIG_MODULE = re.compile(r"\bQt[56][A-Z]\w*")
# Files that keep their Qt text for now, with the reason. None since the README rewrite.
QT_EXEMPT = {}

# layering: the GTK, GLib, and libsoup headers.
LAYERING_PATTERN = (r'^\s*#\s*include\s*[<"]\s*'
                    r"(?:gtk/|gdk/|gdk-pixbuf/|gsk/|adwaita|glib|gio/|libsoup/)")

# translatable
GTK_DIR = "src/launcher/gtk"
POTFILES = "po/POTFILES.in"
TRANSLATION_DOMAIN = "kyty-launcher"
I18N_HEADER = "src/launcher/gtk/i18n.h"
TRANSLATION_MARKERS = re.compile(
    rf'(?<![{WORD_CHARS}])(?:_|N_|C_|NC_|NGettext|NGettextNoop)\(|translatable="yes"')
BUILDER_TEXT_PROPERTIES = {"label", "title", "subtitle", "tooltip-text", "tooltip-markup",
                           "placeholder-text", "description", "text", "heading", "body"}
# The text arguments (0-based) of the setters and constructors whose argument the user sees.
TEXT_ARGUMENTS = (
    ("gtk_label_set_(?:text|markup|label)(?:_with_mnemonic)?", (1,)),
    ("gtk_label_new(?:_with_mnemonic)?", (0,)),
    ("gtk_window_set_title", (1,)),
    ("gtk_file_dialog_set_title", (1,)),
    ("gtk_file_filter_set_name", (1,)),
    ("gtk_alert_dialog_set_(?:message|detail)", (1,)),
    ("gtk_column_view_column_new", (0,)),
    ("(?:gtk_stack|adw_view_stack)_add_titled(?:_with_icon)?", (3,)),
    ("gtk_(?:button|check_button|menu_button)_set_label", (1,)),
    ("gtk_(?:button|check_button|toggle_button)_new_with_(?:label|mnemonic)", (0,)),
    ("gtk_widget_set_tooltip_(?:text|markup)", (1,)),
    ("gtk_(?:editable|entry|search_entry|text|password_entry)_set_placeholder_text", (1,)),
    ("adw_[a-z_]+_set_(?:title|subtitle|description|button_label)", (1,)),
    ("adw_toast_new", (0,)),
    ("g_menu_item_new(?:_section|_submenu)?", (0,)),
    ("g_menu_(?:append|prepend)(?:_section|_submenu)?", (1,)),
    ("gtk_string_list_append", (1,)),
    ("gtk_alert_dialog_new", (0,)),
    # alerts.h: heading, body, and the button labels.
    ("Confirm", (1, 2, 3, 4)),
    ("Choose", (1, 2, 3)),
    ("ShowError", (1, 2)),
    ("ShowErrorList", (1, 2)),
)
TEXT_ARGUMENT_CALL = re.compile(
    rf"(?<![{WORD_CHARS}])("
    + "|".join(f"(?:{name})" for name, _ in TEXT_ARGUMENTS) + r")\s*\(")
ACCESSIBLE_UPDATE_CALL = re.compile(rf"(?<![{WORD_CHARS}])gtk_accessible_update_property\s*\(")
ACCESSIBLE_TEXT_PROPERTIES = {"GTK_ACCESSIBLE_PROPERTY_LABEL",
                              "GTK_ACCESSIBLE_PROPERTY_DESCRIPTION"}
NOT_TRANSLATED_COMMENT = re.compile(r"//\s*not translated:\s*\S.*$")
# Escape sequences and printf conversions carry no words.
NON_TEXT_PARTS = re.compile(r"\\(?:x[0-9A-Fa-f]+|u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8}|[0-7]{1,3}|.)"
                            r"|%(?:%|[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|z|j|t|L)?[a-zA-Z])")

# core_text
CORE_TEXT_MAPPING = "src/launcher/gtk/coreMessageText.cpp"
# The adapters of SDL, libsoup, GIO, and the GTK dialogs: they handle library texts only.
CORE_TEXT_SKIPPED = ("src/launcher/gtk/services",)
CORE_TEXT_READ = re.compile(
    rf"(?:\.|->)(?:message|reason|error)(?![{WORD_CHARS}])(?!\s*\()(?!\s*=(?!=))"
    r"|\.error\s*\(\s*\)\s*\.\s*c_str\s*\("
    rf"|\.Error\s*\(\s*\)"
    rf"|(?<![{WORD_CHARS}])RefusalMessage\s*\("
    rf"|(?<![{WORD_CHARS}])(?:core|updater)::k\w*Message(?![{WORD_CHARS}])")
LOG_CALL = re.compile(
    rf"(?<![{WORD_CHARS}])(?:g_debug|g_info|g_message|g_warning|g_critical)\s*\(")
CORE_TEXT_COMMENT = re.compile(r"//\s*core text:\s*\S.*$")

# dialog_registry
REGISTRY_SOURCE = "src/launcher/gtk/dialogRegistry.cpp"
GENERIC_ALERTS_HEADER = "src/launcher/gtk/alerts.h"
WINDOW_PARENTS = {"AdwWindow", "AdwApplicationWindow", "GtkWindow", "AdwDialog"}
FINAL_TYPE = re.compile(
    r"G_DECLARE_FINAL_TYPE\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*\w+\s*,\s*\w+\s*,\s*(\w+)\s*\)")
ALERT_HELPER = re.compile(
    rf"(?<![{WORD_CHARS}])GtkWindow\s*\*\s*((?:Show|Confirm|Ask|Choose)\w*)\s*\(")

# The Documentation_Set: README.md, CONTRIBUTING.md, and every Markdown file under docs/.
README = "README.md"
DOCUMENTATION_TOP = (README, "CONTRIBUTING.md")
FORK_REPOSITORY = "MrG00gle/KytyPS5-GTK"
FORK_URL = "https://github.com/" + FORK_REPOSITORY
UPSTREAM_URL = "https://github.com/KytyPS5/KytyPS5"
KYTY_URL = "https://github.com/InoriRus/Kyty"
SHADPS4_URL = "https://github.com/shadps4-emu/shadPS4"
APP_ID = "io.github.MrG00gle.KytyPS5_GTK"
# design.md "Documentation set layout": the README's first- and second-level headings, in order.
README_HEADINGS = (
    "# KytyPS5-GTK", FORK_CHANGES_HEADING, "## Project status", "## System requirements",
    "## Install with Flatpak", "## Building from source", "## Running", "## Documentation",
    "## Contributing", "## License", "## Acknowledgments")
# Requirement 19.5: the topic sections, each with a paragraph, list, or code block.
README_TOPIC_SECTIONS = ("Project status", "System requirements", "Install with Flatpak",
                         "Building from source", "Running", "Contributing", "License",
                         "Acknowledgments")
ACKNOWLEDGED = (("KytyPS5", UPSTREAM_URL), ("Kyty", KYTY_URL), ("shadPS4", SHADPS4_URL))
MARKDOWN_FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
MARKDOWN_HEADING = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
BLOCKQUOTE_MARKER = re.compile(r"^\s*>\s?")
LIST_MARKER = re.compile(r"^\s*(?:[-*+]|\d+[.)])\s+")
CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)")
# A Markdown link or image: "!" for an image, the text (one level of brackets inside, so a linked
# badge's image), and the target (parentheses balanced one level deep), with an optional title.
MARKDOWN_LINK = re.compile(r"(!?)\[((?:[^\[\]]|\[[^\[\]]*\])*)\]\(\s*<?"
                           r"([^\s<>()]*(?:\([^\s()]*\)[^\s<>()]*)*)>?"
                           r"(?:\s+(?:\"[^\"]*\"|'[^']*'))?\s*\)")
HTML_TAG = re.compile(r"</?[A-Za-z][^<>]*>")
HTML_ANCHOR_ELEMENT = re.compile(r"<a\s[^<>]*?(?<![\w-])href\s*=\s*\"([^\"]*)\"[^<>]*>(.*?)</a\s*>",
                                 re.IGNORECASE | re.DOTALL)
HTML_IMAGE = re.compile(r"<img\s[^<>]*>", re.IGNORECASE)
HTML_ATTRIBUTE = re.compile(r"(?<![\w-])([A-Za-z][\w-]*)\s*=\s*\"([^\"]*)\"")
BARE_URL = re.compile(r"https?://[^\s<>\"'`()\[\]]+")
SENTENCE_END = re.compile(r"[.!?](?=\s)")
# Requirement 19.3: build status badges, and links to GitHub releases and issues pages.
BUILD_BADGE = re.compile(r"/actions/workflows/[^/?#\s]+/badge\.svg"
                         r"|shields\.io/github/"
                         r"(?:actions/workflow/status|workflow/status|check-runs)/")
GITHUB_PAGE = re.compile(r"^https?://(?:www\.)?github\.com/([^/\s?#]+/[^/\s?#]+)/(releases|issues)"
                         r"(?=$|[/?#])", re.IGNORECASE)
# Requirement 19.4: the upstream community links.
COMMUNITY_LINK = re.compile(
    r"^https?://(?:(?:www\.)?discord\.gg/|(?:www\.|ptb\.|canary\.)?discord(?:app)?\.com/invite/"
    r"|(?:www\.)?github\.com/KytyPS5/KytyPS5/discussions(?=$|[/?#])"
    r"|kytyps5\.github\.io(?=$|[/?#]))",
    re.IGNORECASE)
UPSTREAM_KYTYPS5 = re.compile(r"(?i)(?<![A-Za-z0-9_])upstream\s+KytyPS5(?!-GTK)(?![A-Za-z0-9_])")
# Requirement 19.10: KytyPS5 as a whole word, not followed by -GTK.
BARE_KYTYPS5 = re.compile(r"(?<![A-Za-z0-9_])KytyPS5(?!-GTK)(?![A-Za-z0-9_])")
UPSTREAM = re.compile(r"(?i)(?<![A-Za-z0-9_])upstream(?![A-Za-z0-9_])")


def all_of(*patterns):
    compiled = [re.compile(pattern) for pattern in patterns]
    return lambda text: all(pattern.search(text) for pattern in compiled)


# Requirement 19.2: the Fork changes topics, each in a list item of its own.
FORK_CHANGE_TOPICS = (
    ("the Linux x86-64-only scope", all_of(r"\bLinux x86-64\b", r"(?i)\bonly\b")),
    ("the GTK launcher replacing the old launcher",
     all_of(r"\bGTK\b", r"(?<![\w-])Q" r"t(?![\w-])", r"(?i)\blauncher\b")),
    ("the Flatpak package", all_of(r"\bFlatpak\b")),
    ("the removal of Win" "dows support", all_of(r"(?<![\w-])Win" r"dows(?![\w-])")),
    ("the removal of mac" "OS support", all_of(r"(?<![\w-])mac" r"OS(?![\w-])")),
    ("the removal of Nix support",
     all_of(r"(?<![\w-])Nix(?![\w-])", r"(?<![\w-])Nix" r"OS(?![\w-])")),
    ("the coverage targets (90 % and 95 %, 90 %, 60 %, and the 0.50-point rule)",
     lambda text: (len(re.findall(r"(?<![\d.])90\s?%", text)) >= 2
                   and all_of(r"(?<![\d.])95\s?%", r"(?<![\d.])60\s?%", r"(?<![\d.])0\.50(?!\d)")(
                       text))),
)
FLATPAK_INSTALL = re.compile(r"(?<![\w-])flatpak\s+install\b[^\n]*?\S\.flatpak(?![\w-])")
FLATPAK_RUN = re.compile(rf"(?<![\w-])flatpak\s+run\s+(?:-\S+\s+)*{re.escape(APP_ID)}(?![\w.-])")
# Requirements 21.2 and 21.5: the License section's statements.
LICENSE_STATEMENTS = (
    (re.compile(r"GNU General Public License,?\s+version\s+2\b"),
     "that the project is licensed under the GNU General Public License version 2", "21.2"),
    (re.compile(r"(?<![\w.-])GPL-2\.0-only(?![\w.-])"), "the SPDX identifier GPL-2.0-only",
     "21.2"),
    (re.compile(r"\bbased\s+on\b.*\bKyty\b.*\bMIT\s+License\b"),
     "that the project is based on Kyty, released under the MIT License", "21.5"),
    (re.compile(r"(?i)\bthird-party\s+components\s+remain\s+subject\s+to\s+their\s+own\s+"
                r"licenses\b"),
     "that third-party components remain subject to their own licenses", "21.5"),
)
# docs_terms: Removed_Platform_Terms (19.7), words that begin with "qt" in any case (19.7), .exe
# file names (3.6), and the Apple terms of Requirement 4.5, built from pieces as above.
REMOVED_PLATFORM_TERMS = ("Win" "dows", "MS" "VC", "clang" "-cl", "win" "pthread", "Power" "Shell",
                          "mac" "OS", "Molten" "VK", "Roset" "ta") + NIX_TERMS
QT_WORD = re.compile(r"(?i)(?<![A-Za-z0-9_])q" r"t")
EXE_NAME = re.compile(r"(?i)[A-Za-z0-9_-]\.ex" r"e(?![A-Za-z0-9_])")
APPLE_DOC_TERMS = ("Apple" " Silicon", "X" "code", "Home" "brew", "code" "sign",
                   "libMolten" "VK.dylib", "KytyPS5" ".app")
# issue_template (20.11, 20.14)
ISSUE_TEMPLATES = ".github/ISSUE_TEMPLATE"
ISSUE_TEMPLATE_CONFIGS = ("config.yml", "config.yaml")
DISTRIBUTION_FIELD = "distribution"
INSTALLATION_TYPE_FIELD = "installation-type"
INSTALLATION_TYPES = ["Flatpak", "release archive", "source build"]
# A drive-letter path or a backslash: not a Linux example.
NON_LINUX_PATH = re.compile(r"(?<![A-Za-z])[A-Za-z]:[\\/]|\\")
# packaging_lint_exceptions_documented (14.11, 20.7)
LINT_EXCEPTIONS = "packaging/flatpak/lint-exceptions.json"
PACKAGING_GUIDE = "docs/packaging.md"
LINT_EXCEPTIONS_TOOL = re.compile(r"(?<![\w./-])(?:\./)?tools/ci/lint_exceptions\.py(?![\w.-])")


class RepoError(Exception):
    pass


def whole_words(terms):
    """A regex that matches any of terms as a whole word, like git grep -w."""
    alternatives = "|".join(re.escape(term) for term in terms)
    return re.compile(rf"(?<![{WORD_CHARS}])(?:{alternatives})(?![{WORD_CHARS}])")


def substrings(terms):
    return re.compile("|".join(re.escape(term) for term in terms))


def under(*directories):
    def scope(path):
        return any(path.startswith(directory + "/") for directory in directories)
    return scope


def in_scope_nix(path):
    return not under("3rdparty", ".kiro")(path)


def outside_kiro(path):
    return not under(".kiro")(path)


def in_scope_windows(path):
    return (path in TOP_CMAKE_FILES[:2]
            or under(".github/workflows", "src", "tests", ".vscode")(path))


def in_scope_macos(path):
    return path in TOP_CMAKE_FILES or under("src", "tests", ".github")(path)


def in_scope_cmake(path):
    if path in TOP_CMAKE_FILES:
        return True
    name = path.rsplit("/", 1)[-1]
    return path.startswith("src/") and (name == "CMakeLists.txt" or name.endswith(".cmake"))


def in_scope_qt_text(path):
    if under("3rdparty", ".kiro")(path):
        return False
    name = path.rsplit("/", 1)[-1]
    return (name == "CMakeLists.txt" or name.endswith((".cmake", ".sh", ".md"))
            or path.startswith(".github/"))


def fork_changes_lines(lines):
    """Line numbers (1-based) of the README Fork_Changes_Section, heading included."""
    excluded = set()
    inside = False
    for number, line in enumerate(lines, 1):
        text = line.rstrip("\r")
        if text.rstrip() == FORK_CHANGES_HEADING:
            inside = True
        elif inside and text.startswith("## "):
            inside = False
        if inside:
            excluded.add(number)
    return excluded


# Check id -> (requirement, description, [(scope, regex)], skip the README Fork changes section)
CHECKS = {
    "nix_terms": (
        "2.3", "whole-word Nix terms outside 3rdparty/, .kiro/, and the Fork changes section",
        [(in_scope_nix, whole_words(NIX_TERMS))], True),
    "windows_terms": (
        "3.9", "whole-word Windows terms",
        [(in_scope_windows, whole_words(WINDOWS_TERMS))], False),
    "macos_terms": (
        "4.9", "Apple platform terms",
        [(in_scope_macos, substrings(MACOS_TERMS))], False),
    "cmake_apple": (
        "4.2", "Apple configuration in CMake files",
        [(in_scope_cmake, whole_words(CMAKE_APPLE_WORDS)),
         (in_scope_cmake, substrings(CMAKE_APPLE_SUBSTRINGS))], False),
    "bundle_strings": (
        "4.8", "app bundle and Vulkan portability library strings",
        [(under("src"), re.compile(BUNDLE_PATTERN, re.ASCII))], False),
    "deleted_paths": (
        "2.1, 3.1, 4.1", "deleted paths and references to the deleted Apple-only files",
        [(outside_kiro, substrings(DELETED_FILE_NAMES))], False),
    "vscode_patterns": (
        "3.7", "Windows patterns in .vscode/",
        [(under(".vscode"), re.compile(VSCODE_PATTERN, re.IGNORECASE | re.ASCII))], False),
    "gtk_settings_save": (
        "Z3 / phase 6 rule", "SaveSettings calls in src/launcher/gtk/ (use SaveSettingsStore)",
        [(under("src/launcher/gtk"), re.compile(SETTINGS_SAVE_PATTERN))], False),
    "layering": (
        "6.7", "GTK, GLib, or libsoup includes in src/launcher/core/ and src/updater/",
        [(under("src/launcher/core", "src/updater"), re.compile(LAYERING_PATTERN))], False),
}


def line_starts(text):
    """The offset of the first character of each line, for line_of()."""
    return [0] + [match.end() for match in re.finditer("\n", text)]


def line_of(starts, offset):
    return bisect.bisect_right(starts, offset)


def digit_separator(text, index):
    """Whether the ' at index separates digits (1'000) instead of opening a character literal."""
    start = index
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] in "_'"):
        start -= 1
    return start < index and text[start].isdigit()


def mask_cpp(text):
    """text with every comment replaced by spaces and the contents of every string and character
    literal replaced by "x" (newlines kept, so offsets and lines stay), and the (start, end) of each
    string literal, quotes included."""
    masked = list(text)
    literals = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            masked[index:end] = " " * (end - index)
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            masked[index:end] = [c if c == "\n" else " " for c in text[index:end]]
            index = end
        elif char == "'" and digit_separator(text, index):
            index += 1
        elif char in "\"'":
            end = index + 1
            while end < length and text[end] != char and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
            masked[index + 1:end - 1] = "x" * (end - 1 - index - 1)
            if char == '"':
                literals.append((index, end))
            index = end
        else:
            index += 1
    return "".join(masked), literals


def split_arguments(masked, open_paren):
    """The (start, end) of each argument of the call whose "(" is at open_paren, and the offset
    after its ")". Commas inside (), [], or {} do not split."""
    arguments = []
    depth = 0
    start = open_paren + 1
    index = open_paren + 1
    while index < len(masked):
        char = masked[index]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            if depth == 0:
                arguments.append((start, index))
                return arguments, index + 1
            depth -= 1
        elif char == "," and depth == 0:
            arguments.append((start, index))
            start = index + 1
        index += 1
    arguments.append((start, len(masked)))
    return arguments, len(masked)


def outer_literals(masked, literals, start, end):
    """The string literals in masked[start:end] that are not inside a nested call or subscript
    (braces do not count: the labels of an initializer list are checked)."""
    found = []
    depth = 0
    positions = {literal_start: literal_end for literal_start, literal_end in literals
                 if start <= literal_start < end}
    index = start
    while index < end:
        char = masked[index]
        if index in positions:
            if depth == 0:
                found.append((index, positions[index]))
            index = positions[index]
            continue
        if char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        index += 1
    return found


def has_text(literal):
    """A literal (quotes included) whose contents have a letter outside escapes and conversions."""
    return re.search("[A-Za-z]", NON_TEXT_PARTS.sub("", literal[1:-1])) is not None


def untranslated_literals(path, text):
    """The violations of the translatable rule for C++ literals in text."""
    masked, literals = mask_cpp(text)
    starts = line_starts(text)
    lines = text.split("\n")
    violations = []

    def exempt(line_number):
        return NOT_TRANSLATED_COMMENT.search(lines[line_number - 1].rstrip("\r")) is not None

    def check(call_start, name, argument_ranges):
        call_line = line_of(starts, call_start)
        for argument_start, argument_end in argument_ranges:
            for literal_start, literal_end in outer_literals(masked, literals, argument_start,
                                                             argument_end):
                literal = text[literal_start:literal_end]
                literal_line = line_of(starts, literal_start)
                if not has_text(literal) or exempt(literal_line) or exempt(call_line):
                    continue
                violations.append(f"{path}:{literal_line}: {display(lines[literal_line - 1])} "
                                  f"(untranslated text {display(literal)} for {name})")

    for match in TEXT_ARGUMENT_CALL.finditer(masked):
        name = match.group(1)
        indexes = next(positions for pattern, positions in TEXT_ARGUMENTS
                       if re.fullmatch(pattern, name))
        arguments, _ = split_arguments(masked, match.end() - 1)
        check(match.start(), name, [arguments[i] for i in indexes if i < len(arguments)])
    for match in ACCESSIBLE_UPDATE_CALL.finditer(masked):
        arguments, _ = split_arguments(masked, match.end() - 1)
        values = [arguments[i + 1] for i, (start, end) in enumerate(arguments[:-1])
                  if masked[start:end].strip() in ACCESSIBLE_TEXT_PROPERTIES]
        check(match.start(), "gtk_accessible_update_property", values)
    return violations


def builder_violations(path, data):
    """The violations of the translatable rule in a GtkBuilder file (no line for a parse error)."""
    violations = []
    parser = xml.parsers.expat.ParserCreate()
    stack = []

    def start(tag, attributes):
        stack.append((tag, attributes, parser.CurrentLineNumber, []))
        if tag == "interface" and attributes.get("domain") != TRANSLATION_DOMAIN:
            violations.append(f'{path}:{parser.CurrentLineNumber}: <interface> lacks '
                              f'domain="{TRANSLATION_DOMAIN}"')

    def characters(text):
        if stack:
            stack[-1][3].append(text)

    def end(tag):
        tag, attributes, line, parts = stack.pop()
        name = attributes.get("name", "").replace("_", "-")
        is_text = ((tag == "property" and name in BUILDER_TEXT_PROPERTIES)
                   or (tag == "attribute" and name == "label") or tag == "item")
        text = "".join(parts).strip()
        translatable = attributes.get("translatable")
        if (not is_text or not text or translatable == "yes"
                or (translatable == "no" and attributes.get("comments"))):
            return
        label = f'<{tag} name="{name}">' if name else f"<{tag}>"
        violations.append(f'{path}:{line}: {label}{display(text)} is not translatable="yes" '
                          f'(or translatable="no" with comments)')

    parser.StartElementHandler = start
    parser.CharacterDataHandler = characters
    parser.EndElementHandler = end
    try:
        parser.Parse(data, True)
    except xml.parsers.expat.ExpatError as error:
        violations.append(f"{path}:{error.lineno}: not well-formed XML: {error}")
    return violations


def check_qt_remnants(repo, files):
    violations = []
    scanned = 0
    for path in files:
        if under("3rdparty", ".kiro")(path):
            continue
        if path.endswith(QT_FILE_EXTENSIONS):
            violations.append(f"{path}: a Qt Designer form, Qt resource file, or Qt style sheet")
        if not in_scope_qt_text(path) or path in QT_EXEMPT:
            continue
        lines = read_text_lines(repo, path)
        if lines is None:
            continue
        scanned += 1
        # Line number -> the text shown; a statement over several lines is shown joined, at its
        # first line.
        found = {}
        for number, line in enumerate(lines, 1):
            if QT_BUILD_PATTERN.search(line):
                found[number] = display(line)
        for number, text in continued_lines(lines):
            if ((QT_PACKAGE_MANAGER.search(text) and QT_PACKAGE.search(text))
                    or (QT_PKG_CONFIG_TOOL.search(text) and QT_PKG_CONFIG_MODULE.search(text))):
                found.setdefault(number, display(text))
        whole = "\n".join(lines)
        starts = line_starts(whole)
        for match in QT_PKG_CONFIG_CALL.finditer(whole):
            _, end = split_arguments(whole, match.end() - 1)
            call = whole[match.start():end]
            if QT_PKG_CONFIG_MODULE.search(call):
                found.setdefault(line_of(starts, match.start()), display(" ".join(call.split())))
        violations.extend(f"{path}:{number}: {found[number]}" for number in sorted(found))
    return violations, scanned


def continued_lines(lines):
    """The (first line number, text) of each line together with the lines that its trailing
    backslashes continue (a shell or package list split over lines), joined with single spaces."""
    joined = []
    first = None
    parts = []
    for number, line in enumerate(lines, 1):
        text = line.rstrip("\r")
        continued = text.endswith("\\")
        if first is None:
            first = number
        parts.append((text[:-1] if continued else text).strip())
        if not continued:
            joined.append((first, " ".join(part for part in parts if part)))
            first = None
            parts = []
    if parts:
        joined.append((first, " ".join(part for part in parts if part)))
    return joined


def check_translatable(repo, files):
    violations = []
    scanned = 0
    listed_files = set(files)
    potfiles = read_text_lines(repo, POTFILES)
    if potfiles is None:
        raise RepoError(f"cannot read {POTFILES}")
    scanned += 1
    entries = []
    for number, line in enumerate(potfiles, 1):
        entry = line.strip()
        if not entry or entry.startswith("#"):
            continue
        if entries and entry < entries[-1][0]:
            violations.append(f"{POTFILES}:{number}: {entry} is not in sorted order "
                              f"(after {entries[-1][0]})")
        if any(entry == previous for previous, _ in entries):
            violations.append(f"{POTFILES}:{number}: {entry} is listed twice")
        if entry not in listed_files or not os.path.isfile(os.path.join(repo, entry)):
            violations.append(f"{POTFILES}:{number}: {entry} does not exist")
        entries.append((entry, number))
    listed = {entry for entry, _ in entries}

    for path in files:
        if not under(GTK_DIR)(path) or not path.endswith((".cpp", ".h", ".xml")):
            continue
        lines = read_text_lines(repo, path)
        if lines is None:
            continue
        scanned += 1
        text = "\n".join(lines)
        if path != I18N_HEADER and path not in listed:
            for number, line in enumerate(lines, 1):
                if TRANSLATION_MARKERS.search(line):
                    violations.append(f"{path}:{number}: {display(line)} (the file marks "
                                      f"strings for translation but is not in {POTFILES})")
                    break
        if path.endswith(".xml"):
            violations.extend(builder_violations(path, text.encode("utf-8", "surrogateescape")))
        else:
            violations.extend(untranslated_literals(path, text))
    return violations, scanned


def function_bodies(text, name):
    """The (start, end) of the body of each definition of the GtkWindow* function name in the
    masked C++ text (declarations, which end with ";", have none)."""
    bodies = []
    pattern = re.compile(rf"(?<![{WORD_CHARS}])GtkWindow\s*\*\s*(?:\w+::)*{re.escape(name)}\s*\(")
    for match in pattern.finditer(text):
        _, after = split_arguments(text, match.end() - 1)
        brace = re.match(r"\s*\{", text[after:])
        if brace is None:
            continue
        start = after + brace.end() - 1
        depth = 0
        for index in range(start, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    bodies.append((start, index + 1))
                    break
    return bodies


def calls(text, name):
    return re.search(rf"(?<![{WORD_CHARS}]){re.escape(name)}\s*\(", text) is not None


def check_dialog_registry(repo, files):
    violations = []
    registry_lines = read_text_lines(repo, REGISTRY_SOURCE)
    if registry_lines is None:
        raise RepoError(f"cannot read {REGISTRY_SOURCE}")
    registry, _ = mask_cpp("\n".join(registry_lines))
    scanned = 1
    sources = []
    helpers = []
    for path in files:
        if not under(GTK_DIR)(path) or not path.endswith((".h", ".cpp")):
            continue
        lines = read_text_lines(repo, path)
        if lines is None:
            continue
        scanned += path.endswith(".h")
        text = "\n".join(lines)
        masked, _ = mask_cpp(text)
        if path.endswith(".cpp"):
            sources.append(masked)
            continue
        starts = line_starts(masked)
        for match in FINAL_TYPE.finditer(masked):
            type_name, prefix, parent = match.groups()
            if parent not in WINDOW_PARENTS:
                continue
            line = line_of(starts, match.start())
            constructors = sorted(set(re.findall(rf"(?<![{WORD_CHARS}])({prefix}_new\w*)\s*\(",
                                                 masked)))
            if not constructors:
                violations.append(f"{path}:{line}: {type_name} ({parent}) declares no "
                                  f"{prefix}_new constructor for {REGISTRY_SOURCE} to call")
            for constructor in constructors:
                if not calls(registry, constructor):
                    violations.append(f"{path}:{line}: {type_name} ({parent}): {constructor} "
                                      f"is not called in {REGISTRY_SOURCE}")
        for match in ALERT_HELPER.finditer(masked):
            helpers.append((match.group(1), path, line_of(starts, match.start())))

    called = {name for name, _, _ in helpers if calls(registry, name)}
    through = set()
    for name in called:
        for source in sources:
            for start, end in function_bodies(source, name):
                through.update(other for other, _, _ in helpers if calls(source[start:end], other))
    reported = set()
    for name, path, line in helpers:
        if name in reported or name in called:
            continue
        if path == GENERIC_ALERTS_HEADER and name in through:
            continue
        reported.add(name)
        violations.append(f"{path}:{line}: the alert helper {name} is not opened by "
                          f"{REGISTRY_SOURCE}")
    return violations, scanned


def core_text_violations(path, text):
    """The violations of the core_text rule in the C++ text of path."""
    masked, _ = mask_cpp(text)
    starts = line_starts(text)
    lines = text.split("\n")
    logged = []
    for match in LOG_CALL.finditer(masked):
        _, end = split_arguments(masked, match.end() - 1)
        logged.append((match.end(), end))
    violations = []
    for match in CORE_TEXT_READ.finditer(masked):
        if any(start <= match.start() < end for start, end in logged):
            continue
        number = line_of(starts, match.start())
        line = lines[number - 1].rstrip("\r")
        if CORE_TEXT_COMMENT.search(line):
            continue
        what = " ".join(match.group(0).split())
        violations.append(f"{path}:{number}: {display(line)} (core text {what} shown as it is; "
                          f"use {CORE_TEXT_MAPPING}, a log call, or // core text: <why>)")
    return violations


def check_core_text(repo, files):
    violations = []
    scanned = 0
    for path in files:
        if (not under(GTK_DIR)(path) or under(*CORE_TEXT_SKIPPED)(path)
                or path == CORE_TEXT_MAPPING or not path.endswith((".cpp", ".h"))):
            continue
        lines = read_text_lines(repo, path)
        if lines is None:
            continue
        scanned += 1
        violations.extend(core_text_violations(path, "\n".join(lines)))
    return violations, scanned


# ---- The Documentation_Set (tasks 29.6 and 29.7)

class Block:
    """A Markdown block: kind is "heading", "item" (a list item), "row" (a table row),
    "paragraph", or "code" (a fenced code block); text has its lines joined with single spaces
    (a code block keeps its newlines), without blockquote markers or the list marker."""

    __slots__ = ("kind", "line", "text", "level")

    def __init__(self, kind, line, text, level=0):
        self.kind = kind
        self.line = line
        self.text = text
        self.level = level


def markdown_fenced_lines(lines):
    """The line numbers (1-based) of the fenced code blocks, fences included."""
    fenced = set()
    fence = None
    for number, line in enumerate(lines, 1):
        match = MARKDOWN_FENCE.match(line)
        if fence is None:
            if match and not (match.group(1)[0] == "`" and "`" in line[match.end():]):
                fence = match.group(1)
                fenced.add(number)
        else:
            fenced.add(number)
            if (match and match.group(1)[0] == fence[0] and len(match.group(1)) >= len(fence)
                    and not line[match.end():].strip()):
                fence = None
    return fenced


def markdown_blocks(lines):
    fenced = markdown_fenced_lines(lines)
    blocks = []
    current = []

    def close():
        if current:
            kind, line, parts = current
            joiner = "\n" if kind == "code" else " "
            blocks.append(Block(kind, line, joiner.join(parts).strip()))
            current.clear()

    for number, raw in enumerate(lines, 1):
        line = raw.rstrip("\r")
        if number in fenced:
            if not current or current[0] != "code":
                close()
                current.extend(("code", number, []))
            current[2].append(line)
            continue
        if current and current[0] == "code":
            close()
        text = BLOCKQUOTE_MARKER.sub("", line)
        heading = MARKDOWN_HEADING.match(text)
        if heading or not text.strip() or text.lstrip().startswith("|") or LIST_MARKER.match(text):
            close()
        if heading:
            title = (heading.group(2) or "").strip()
            blocks.append(Block("heading", number, title, len(heading.group(1))))
        elif text.lstrip().startswith("|"):
            blocks.append(Block("row", number, text.strip()))
        elif LIST_MARKER.match(text):
            current.extend(("item", number, [LIST_MARKER.sub("", text, count=1).strip()]))
        elif text.strip():
            if not current:
                current.extend(("paragraph", number, []))
            current[2].append(text.strip())
    close()
    return blocks


def heading_line(block):
    return "#" * block.level + " " + block.text


def readme_sections(blocks):
    """{title: blocks} of the "## " sections; "" holds the blocks before the first one."""
    sections = {"": []}
    current = ""
    for block in blocks:
        if block.kind == "heading" and block.level == 2:
            current = block.text
            sections.setdefault(current, [])
            continue
        sections[current].append(block)
    return sections


class Link:
    __slots__ = ("start", "end", "text", "text_span", "target", "target_span", "image", "parent")

    def __init__(self, start, end, text, text_span, target, target_span, image, parent=None):
        self.start = start
        self.end = end
        self.text = text
        self.text_span = text_span
        self.target = target
        self.target_span = target_span
        self.image = image
        self.parent = parent


def markdown_links(text):
    """Every Markdown link and image of text (also nested, as in a linked badge), every HTML <a>
    with an href and <img> with a src, and every bare URL outside them, in text order. An image's
    text is its alternative text; its parent is the target of the link around it."""
    links = nested_markdown_links(text, 0, None)
    anchored = []
    for match in HTML_ANCHOR_ELEMENT.finditer(text):
        anchored.append(match.span())
        links.append(Link(match.start(), match.end(), match.group(2), match.span(2),
                          match.group(1), match.span(1), False))
        for image in HTML_IMAGE.finditer(match.group(2)):
            links.extend(html_image(image, match.start(2), match.group(1)))
    for image in HTML_IMAGE.finditer(text):
        if not any(start <= image.start() < end for start, end in anchored):
            links.extend(html_image(image, 0, None))
    taken = [span for link in links for span in (link.target_span, (link.start, link.end))]
    for match in BARE_URL.finditer(text):
        if not any(start <= match.start() < end for start, end in taken):
            url = match.group(0).rstrip(".,;:!?")
            span = (match.start(), match.start() + len(url))
            links.append(Link(span[0], span[1], "", (span[0], span[0]), url, span, False))
    return sorted(links, key=lambda link: link.start)


def nested_markdown_links(text, offset, parent):
    links = []
    for match in MARKDOWN_LINK.finditer(text):
        image = match.group(1) == "!"
        links.append(Link(offset + match.start(), offset + match.end(), match.group(2),
                          (offset + match.start(2), offset + match.end(2)), match.group(3),
                          (offset + match.start(3), offset + match.end(3)), image, parent))
        links.extend(nested_markdown_links(match.group(2), offset + match.start(2),
                                           None if image else match.group(3)))
    return links


def html_image(match, offset, parent):
    attributes = {name.lower(): (value, (offset + match.start() + value_start,
                                         offset + match.start() + value_start + len(value)))
                  for name, value, value_start in (
                      (found.group(1), found.group(2), found.start(2))
                      for found in HTML_ATTRIBUTE.finditer(match.group(0)))}
    if "src" not in attributes:
        return []
    alt, alt_span = attributes.get("alt", ("", (0, 0)))
    source, source_span = attributes["src"]
    return [Link(offset + match.start(), offset + match.end(), alt, alt_span, source, source_span,
                 True, parent)]


def url_spans(text):
    """The spans of text that are URLs or link targets, which the README rules skip."""
    spans = [link.target_span for link in markdown_links(text)]
    spans += [match.span(2) for match in HTML_ATTRIBUTE.finditer(text)
              if match.group(1).lower() in ("href", "src")]
    return spans + [match.span() for match in BARE_URL.finditer(text)]


def sentences(text, links):
    """The (start, end) of each sentence of a block's text; a sentence ends after ".", "!", or
    "?" before a space, but not inside a link or an HTML tag."""
    protected = [(link.start, link.end) for link in links]
    protected += [match.span() for match in HTML_TAG.finditer(text)]
    bounds = [0]
    for match in SENTENCE_END.finditer(text):
        if not any(start < match.end() < end for start, end in protected):
            bounds.append(match.end())
    bounds.append(len(text))
    return [(start, end) for start, end in zip(bounds, bounds[1:]) if text[start:end].strip()]


def visible(text, spans):
    """text with HTML tags and the given spans blanked, offsets kept."""
    characters = list(text)
    for start, end in [match.span() for match in HTML_TAG.finditer(text)] + list(spans):
        characters[start:end] = " " * (end - start)
    return "".join(characters)


def normalized(text):
    return " ".join(text.split())


def relative_target(source, target):
    """The repository path of a relative link target (fragment and query dropped), or None."""
    target = target.strip()
    if not target or re.match(r"^(?:[A-Za-z][A-Za-z0-9+.-]*:|//|#)", target):
        return None
    path = re.split(r"[?#]", target, maxsplit=1)[0]
    joined = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
    return "" if joined == "." else joined


def documentation_set(files):
    return [path for path in files
            if path in DOCUMENTATION_TOP or (path.startswith("docs/") and path.endswith(".md"))]


def distinct_items(candidates):
    """The topics that cannot each get an item of their own; candidates[i] is the set of the items
    that topic i matches (Kuhn's matching)."""
    owner = {}

    def assign(topic, seen):
        for item in sorted(candidates[topic]):
            if item in seen:
                continue
            seen.add(item)
            if item not in owner or assign(owner[item], seen):
                owner[item] = topic
                return True
        return False

    return [topic for topic in range(len(candidates)) if not assign(topic, set())]


class ReadmeCheck:
    """The README rules of readme_structure."""

    def __init__(self, repo, files, lines):
        self.files = files
        self.lines = lines
        self.blocks = markdown_blocks(lines)
        self.sections = readme_sections(self.blocks)
        self.violations = []

    def report(self, line, text, requirement):
        where = f"{README}:{line}" if line else README
        label = "Requirements" if "," in requirement else "Requirement"
        self.violations.append(f"{where}: {text} ({label} {requirement})")

    def text_blocks(self, title):
        return [block for block in self.sections.get(title, []) if block.kind != "heading"]

    def run(self):
        self.check_headings()
        self.check_intro()
        self.check_fork_changes()
        self.check_topic_sections()
        self.check_links()
        self.check_upstream_names()
        self.check_system_requirements()
        self.check_flatpak()
        self.check_documentation_links()
        self.check_license()
        self.check_acknowledgments()
        return self.violations

    def check_headings(self):
        headings = [block for block in self.blocks if block.kind == "heading" and block.level <= 2]
        found = [heading_line(block) for block in headings]
        first_level = [block for block in headings if block.level == 1]
        if len(first_level) != 1 or first_level[0].text != README_HEADINGS[0][2:]:
            shown = ", ".join(repr(heading_line(block)) for block in first_level) or "none"
            self.report(first_level[0].line if first_level else 0,
                        f"the README needs exactly one first-level heading, "
                        f"{README_HEADINGS[0]!r}; it has {shown}", "19.1")
        if found != list(README_HEADINGS):
            self.report(0, f"the first- and second-level headings are {found}, not design.md's "
                        f"{list(README_HEADINGS)} in that order", "19.2, 19.5")

    def check_intro(self):
        intro = [block for block in self.sections[""] if block.kind in ("paragraph", "item")]
        text = normalized(" ".join(block.text for block in intro))
        statement = False
        for block in intro:
            links = markdown_links(block.text)
            for start, end in sentences(block.text, links):
                sentence = block.text[start:end]
                targets = {link.target.rstrip("/") for link in links
                           if start <= link.start < end and not link.image}
                if ("KytyPS5-GTK" in sentence and re.search(r"(?i)\bfork\b", sentence)
                        and re.search(r"(?i)\bbased\s+on\b", sentence)
                        and {UPSTREAM_URL, KYTY_URL} <= targets):
                    statement = True
        if not statement:
            self.report(0, "no sentence before the first second-level heading states that "
                        f"KytyPS5-GTK is a fork of upstream KytyPS5 ({UPSTREAM_URL}), which is "
                        f"based on Kyty ({KYTY_URL}), with a link to both", "19.1")
        if not re.search(r"not\s+affiliated\s+with\s+Sony\s+Interactive\s+Entertainment", text):
            self.report(0, "no statement before the first second-level heading that KytyPS5-GTK "
                        "is not affiliated with Sony Interactive Entertainment", "19.6")
        if not re.search(r"(?i)\bonly\s+legally\s+obtained\b", text):
            self.report(0, "no statement before the first second-level heading that users must "
                        "use only legally obtained game files", "19.6")

    def check_fork_changes(self):
        items = [block for block in self.text_blocks(FORK_CHANGES_HEADING[3:])
                 if block.kind == "item"]
        candidates = [{index for index, item in enumerate(items) if matches(item.text)}
                      for _, matches in FORK_CHANGE_TOPICS]
        for topic in distinct_items(candidates):
            name = FORK_CHANGE_TOPICS[topic][0]
            what = "no list item" if not candidates[topic] else "no list item of its own"
            self.report(0, f"{FORK_CHANGES_HEADING} has {what} for {name}", "19.2")

    def check_topic_sections(self):
        for title in README_TOPIC_SECTIONS:
            if title in self.sections and not any(
                    block.kind in ("paragraph", "item", "row", "code") and block.text
                    for block in self.sections[title]):
                self.report(0, f"the section ## {title} has no paragraph, list, or code block",
                            "19.5")

    def all_links(self):
        for block in self.blocks:
            if block.kind != "code":
                for link in markdown_links(block.text):
                    yield block, link

    def check_links(self):
        badges = 0
        workflows = {path.rsplit("/", 1)[-1] for path in self.files
                     if under(".github/workflows")(path) and path.endswith((".yml", ".yaml"))}
        for block, link in self.all_links():
            if link.image and BUILD_BADGE.search(link.target):
                badges += 1
                self.check_badge(block, link, workflows)
        if not badges:
            self.report(0, "the README has no build status badge", "19.3")
        text = "\n".join(block.text for block in self.blocks if block.kind != "code")
        masked = CODE_SPAN.sub(lambda match: " " * len(match.group(0)), text)
        for match in BARE_URL.finditer(masked):
            url = match.group(0).rstrip(".,;:!?")
            found = GITHUB_PAGE.match(url)
            if not found:
                continue
            page = found.group(2)
            wanted = f"{FORK_URL}/{page}"
            if not (url == wanted or url.startswith((wanted + "/", wanted + "?", wanted + "#"))):
                self.report(self.line_of_text(url), f"the {page} link {url} is not under "
                            f"{wanted}", "19.3")
        for block, link in self.all_links():
            if link.image or not COMMUNITY_LINK.match(link.target):
                continue
            if self.has_phrase(block, link, UPSTREAM_KYTYPS5):
                continue
            self.report(block.line, f"the link to {link.target} does not say \"upstream "
                        "KytyPS5\" in its text, sentence, or list item", "19.4")

    def check_badge(self, block, link, workflows):
        found = re.search(rf"{re.escape(FORK_REPOSITORY)}/(?:actions/workflows/)?([\w.-]+\.ya?ml)",
                          link.target)
        workflow = found.group(1) if found else None
        if workflow not in workflows:
            self.report(block.line, f"the build badge image {link.target} names no workflow of "
                        f"{FORK_REPOSITORY} under .github/workflows/", "19.3")
            return
        wanted = f"{FORK_URL}/actions/workflows/{workflow}"
        parent = link.parent or ""
        if not (parent == wanted or parent.startswith((wanted + "?", wanted + "#"))):
            self.report(block.line, f"the build badge {link.target} links to "
                        f"{parent or 'nothing'}, not {wanted}", "19.3")

    def line_of_text(self, needle):
        for number, line in enumerate(self.lines, 1):
            if needle in line:
                return number
        return 0

    @staticmethod
    def has_phrase(block, link, phrase):
        """Whether phrase is in the link's text, its sentence, or (for a list item or heading)
        the block."""
        links = markdown_links(block.text)
        shown = visible(block.text, url_spans(block.text))
        if phrase.search(link.text):
            return True
        if block.kind in ("item", "heading") and phrase.search(shown):
            return True
        return any(start <= link.start < end and phrase.search(shown[start:end])
                   for start, end in sentences(block.text, links))

    def check_upstream_names(self):
        for block in self.blocks:
            links = markdown_links(block.text)
            skipped = url_spans(block.text)
            shown = visible(block.text, skipped)
            alternatives = [link.text_span for link in links if link.image]
            link_texts = [link.text_span for link in links if not link.image]
            tags = [match.span() for match in HTML_TAG.finditer(block.text)]
            spans = sentences(block.text, links)
            for match in BARE_KYTYPS5.finditer(block.text):
                position = match.start()
                if any(start <= position < end for start, end in skipped):
                    continue
                containers = [block.text[start:end] for start, end in alternatives + link_texts
                              if start <= position < end]
                inside_tag = any(start <= position < end for start, end in tags)
                if block.kind in ("item", "heading"):
                    containers.append(shown)
                if not inside_tag:
                    containers += [shown[start:end] for start, end in spans
                                   if start <= position < end]
                if not any(UPSTREAM.search(container) for container in containers):
                    sentence = next((block.text[start:end] for start, end in spans
                                     if start <= position < end), block.text)
                    self.report(block.line, f"KytyPS5 without \"upstream\" in its heading, "
                                f"sentence, list item, link text, or alternative text: "
                                f"{display(sentence)}", "19.10")

    def section_sentences(self, title):
        for block in self.text_blocks(title):
            links = markdown_links(block.text)
            for start, end in sentences(block.text, links):
                yield block.text[start:end]

    def check_system_requirements(self):
        found = list(self.section_sentences("System requirements"))
        if not any(re.search(r"\bonly\b", sentence) and "Linux" in sentence
                   and "x86-64" in sentence for sentence in found):
            self.report(0, "## System requirements does not state that the only supported "
                        "platform is Linux on an x86-64 processor", "19.8")
        if not any("Vulkan 1.3" in sentence and "GPU" in sentence
                   and re.search(r"(?i)\b(?:requir|need|must)", sentence) for sentence in found):
            self.report(0, "## System requirements does not state that a GPU with Vulkan 1.3 "
                        "support is required", "19.8")

    def check_flatpak(self):
        lines = [line for block in self.text_blocks("Install with Flatpak")
                 for line in block.text.split("\n")]
        if not any(FLATPAK_INSTALL.search(line) for line in lines):
            self.report(0, "## Install with Flatpak has no flatpak install command for a "
                        ".flatpak bundle", "19.9")
        if not any(FLATPAK_RUN.search(line) for line in lines):
            self.report(0, f"## Install with Flatpak has no flatpak run {APP_ID} command", "19.9")

    def relative_links(self, blocks):
        return {relative_target(README, link.target) for block in blocks if block.kind != "code"
                for link in markdown_links(block.text) if not link.image} - {None}

    def check_documentation_links(self):
        linked = self.relative_links(self.blocks)
        for path in documentation_set(self.files):
            if path != README and path not in linked:
                self.report(0, f"no relative link to {path}, a file of the Documentation_Set",
                            "20.9")

    def check_license(self):
        blocks = self.text_blocks("License")
        text = normalized(" ".join(visible(block.text, url_spans(block.text)) for block in blocks))
        linked = self.relative_links(blocks)
        for pattern, what, requirement in LICENSE_STATEMENTS:
            if not pattern.search(text):
                self.report(0, f"## License does not state {what}", requirement)
        for path, requirement in (("LICENSE", "21.2"), ("LICENSES/Kyty-MIT.txt", "21.5")):
            if path not in linked:
                self.report(0, f"## License has no relative link to {path}", requirement)

    def check_acknowledgments(self):
        items = [block for block in self.text_blocks("Acknowledgments") if block.kind == "item"]
        for name, url in ACKNOWLEDGED:
            entries = [item for item in items
                       if any(link.target.rstrip("/") == url for link in markdown_links(item.text))]
            if len(entries) != 1:
                self.report(0, f"## Acknowledgments has {len(entries)} list items that link to "
                            f"{url}, not one entry for {name}", "21.3")
            elif not re.search(rf"(?<![\w-]){re.escape(name)}(?![\w-])", entries[0].text):
                self.report(entries[0].line, f"the acknowledgment that links to {url} does not "
                            f"name {name}", "21.3")


def check_readme_structure(repo, files):
    lines = read_text_lines(repo, README)
    if lines is None:
        return [f"{README}: missing (Requirement 19.1)"], 0
    return ReadmeCheck(repo, files, [line.rstrip("\r") for line in lines]).run(), 1


def check_docs_terms(repo, files):
    violations = []
    scanned = 0
    rules = ((whole_words(REMOVED_PLATFORM_TERMS), "Removed_Platform_Term", "19.7"),
             (QT_WORD, "word that begins with qt", "19.7"),
             (EXE_NAME, ".exe file name", "3.6"),
             (substrings(APPLE_DOC_TERMS), "Apple term", "4.5"))
    for path in documentation_set(files):
        lines = read_text_lines(repo, path)
        if lines is None:
            continue
        scanned += 1
        excluded = fork_changes_lines(lines) if path == README else set()
        for number, line in enumerate(lines, 1):
            if number in excluded:
                continue
            for pattern, what, requirement in rules:
                for match in pattern.finditer(line):
                    violations.append(f"{path}:{number}: {display(line)} ({what} "
                                      f"{match.group(0)!r}, Requirement {requirement})")
    return violations, scanned


def import_yaml():
    try:
        import yaml
    except ImportError:
        raise RepoError("this check needs PyYAML (python3-pyyaml or python3-yaml)") from None
    return yaml


def required(field):
    validations = field.get("validations")
    return isinstance(validations, dict) and validations.get("required") is True


def form_violations(path, fields):
    """The violations of Requirements 20.11 and 20.14 in the fields of one issue form."""
    violations = []

    def field(field_id, kind):
        found = [item for item in fields if item.get("id") == field_id]
        if len(found) != 1:
            violations.append(f"{path}: {len(found)} fields with the id {field_id}, not one "
                              f"required {kind} (Requirement 20.11)")
            return None
        if found[0].get("type") != kind or not required(found[0]):
            optional = "" if required(found[0]) else " (not required)"
            violations.append(f"{path}: the field {field_id} is a {found[0].get('type')}"
                              f"{optional}, not a required {kind} (Requirement 20.11)")
        attributes = found[0].get("attributes")
        return attributes if isinstance(attributes, dict) else {}

    attributes = field(DISTRIBUTION_FIELD, "input")
    if attributes is not None:
        label = str(attributes.get("label", ""))
        if not (re.search(r"(?i)\bdistribution\b", label) and re.search(r"(?i)\bversion\b", label)):
            violations.append(f"{path}: the label of {DISTRIBUTION_FIELD}, {label!r}, does not ask "
                              "for the Linux distribution and version (Requirement 20.11)")
    attributes = field(INSTALLATION_TYPE_FIELD, "dropdown")
    if attributes is not None and attributes.get("options") != INSTALLATION_TYPES:
        violations.append(f"{path}: the options of {INSTALLATION_TYPE_FIELD} are "
                          f"{attributes.get('options')!r}, not exactly {INSTALLATION_TYPES!r} "
                          "(Requirement 20.11)")
    for item in fields:
        attributes = item.get("attributes")
        if item.get("type") == "markdown" or not isinstance(attributes, dict):
            continue
        for key in ("placeholder", "value"):
            example = attributes.get(key)
            if isinstance(example, str) and (EXE_NAME.search(example)
                                             or NON_LINUX_PATH.search(example)):
                violations.append(f"{path}: the {key} of {item.get('id')}, {display(example)!r}, "
                                  "is not a Linux example (Requirement 20.14)")
    return violations


def check_issue_template(repo, files):
    yaml = import_yaml()
    templates = [path for path in files if under(ISSUE_TEMPLATES)(path)
                 and path.rsplit("/", 1)[-1] not in ISSUE_TEMPLATE_CONFIGS]
    violations = [] if templates else [f"{ISSUE_TEMPLATES}: no issue template (Requirement 20.11)"]
    scanned = 0
    terms = whole_words(REMOVED_PLATFORM_TERMS)
    for path in templates:
        lines = read_text_lines(repo, path)
        if lines is None:
            continue
        scanned += 1
        for number, line in enumerate(lines, 1):
            for match in terms.finditer(line):
                violations.append(f"{path}:{number}: {display(line)} (Removed_Platform_Term "
                                  f"{match.group(0)!r}, Requirement 20.14)")
        if not path.endswith((".yml", ".yaml")):
            violations.append(f"{path}: a Markdown template cannot require fields; make it an "
                              "issue form (.yml) (Requirement 20.11)")
            continue
        try:
            form = yaml.safe_load("\n".join(lines))
        except yaml.YAMLError as error:
            violations.append(f"{path}: not valid YAML: {' '.join(str(error).split())}")
            continue
        body = form.get("body") if isinstance(form, dict) else None
        if not isinstance(body, list):
            violations.append(f"{path}: has no body list of form fields (Requirement 20.11)")
            continue
        violations.extend(form_violations(path, [item for item in body if isinstance(item, dict)]))
    return violations, scanned


def has_sentence(text):
    """Whether text holds a sentence: at least four words, ending with ".", "!", or "?"."""
    for piece in re.split(r"(?<=[.!?])\s+", text.strip()):
        if (re.search(r"[.!?][\"')\]*`]*$", piece.strip())
                and len(re.findall(r"[A-Za-z]{2,}", piece)) >= 4):
            return True
    return False


def lint_exception_ids(repo, files, violations):
    """{lint ID: where it is listed}: the committed exceptions file and every ID that a workflow
    adds to a per-run copy with tools/ci/lint_exceptions.py --add."""
    ids = {}
    lines = read_text_lines(repo, LINT_EXCEPTIONS)
    if lines is None:
        raise RepoError(f"cannot read {LINT_EXCEPTIONS}")
    try:
        document = json.loads("\n".join(lines))
    except json.JSONDecodeError as error:
        violations.append(f"{LINT_EXCEPTIONS}: not JSON: {error}")
        document = {}
    if not isinstance(document, dict):
        violations.append(f"{LINT_EXCEPTIONS}: not a JSON object of App_IDs and lint ID lists")
        document = {}
    for app_id, listed in document.items():
        if not isinstance(listed, list) or not all(isinstance(entry, str) for entry in listed):
            violations.append(f"{LINT_EXCEPTIONS}: the value of {app_id} is not a list of lint "
                              "IDs")
            continue
        for lint_id in listed:
            ids.setdefault(lint_id, LINT_EXCEPTIONS)
    for path in files:
        if not under(".github/workflows")(path) or not path.endswith((".yml", ".yaml")):
            continue
        for number, text in continued_lines(read_text_lines(repo, path) or []):
            if not LINT_EXCEPTIONS_TOOL.search(text):
                continue
            words = text.split()
            for index, word in enumerate(words):
                if word == "--add" and index + 1 < len(words):
                    ids.setdefault(words[index + 1].strip("'\""), f"{path}:{number}")
                elif word.startswith("--add="):
                    ids.setdefault(word[len("--add="):].strip("'\""), f"{path}:{number}")
    return ids


def check_lint_exceptions_documented(repo, files):
    violations = []
    ids = lint_exception_ids(repo, files, violations)
    guide = read_text_lines(repo, PACKAGING_GUIDE)
    if guide is None:
        violations.append(f"{PACKAGING_GUIDE}: missing; it must justify every lint exception "
                          "(Requirements 14.11, 20.7)")
        return violations, 1
    blocks = [block for block in markdown_blocks(guide) if block.kind not in ("code", "heading")]
    for lint_id, source in sorted(ids.items()):
        pattern = re.compile(rf"(?<![\w-]){re.escape(lint_id)}(?![\w-])")
        mentions = [block for block in blocks if pattern.search(block.text)]
        if not mentions:
            violations.append(f"{PACKAGING_GUIDE}: does not name the lint exception {lint_id} "
                              f"(listed in {source}; Requirements 14.11, 20.7)")
            continue
        if not any(justified(block, pattern) for block in mentions):
            violations.append(f"{PACKAGING_GUIDE}:{mentions[0].line}: names the lint exception "
                              f"{lint_id} without a justification sentence (listed in {source}; "
                              "Requirements 14.11, 20.7)")
    return violations, 2


def justified(block, pattern):
    """Whether a table row has a sentence in a cell without the lint ID, or a paragraph or list
    item has one besides the ID."""
    if block.kind == "row":
        cells = [cell.strip() for cell in re.split(r"(?<!\\)\|", block.text.strip().strip("|"))]
        return any(has_sentence(cell) for cell in cells if not pattern.search(cell))
    return has_sentence(pattern.sub(" ", block.text))


# Check id -> (requirement, description, function(repo, files) -> (violations, files scanned)).
FUNCTION_CHECKS = {
    "qt_remnants": (
        "6.1, 6.3", "Qt files, Qt lookups, Qt link targets, or Qt installation steps",
        check_qt_remnants),
    "translatable": (
        "6.12", "untranslated user-visible strings in the GTK launcher", check_translatable),
    "core_text": (
        "6.12", "core message, reason, or error texts shown as they are in the GTK launcher",
        check_core_text),
    "dialog_registry": (
        "17.10", "window, dialog, or alert types missing from the dialog registry",
        check_dialog_registry),
    "readme_structure": (
        "19.1-19.6, 19.8-19.10, 20.9, 21.2, 21.3, 21.5",
        "README heading, statement, badge, or link that breaks the README rules",
        check_readme_structure),
    "docs_terms": (
        "3.6, 4.5, 19.7", "removed platform terms, words that begin with qt, .exe file names, or "
        "Apple terms in the Documentation_Set outside the Fork changes section", check_docs_terms),
    "issue_template": (
        "20.11, 20.14", "issue template without the required Linux distribution and installation "
        "type fields, or with a removed platform term or a non-Linux example",
        check_issue_template),
    "packaging_lint_exceptions_documented": (
        "14.11, 20.7", "flatpak-builder-lint exception without a justification in "
        "docs/packaging.md", check_lint_exceptions_documented),
}


def list_files(repo):
    command = ["git", "-C", repo, "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    try:
        result = subprocess.run(command, capture_output=True, check=False)
    except OSError as error:
        raise RepoError(f"cannot run git: {error}") from error
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise RepoError(f"git ls-files failed in {repo}: {message}")
    names = {name.decode("utf-8", "surrogateescape")
             for name in result.stdout.split(b"\0") if name}
    return sorted(names)


def read_text_lines(repo, path):
    """The lines of a regular text file, or None for anything else."""
    full = os.path.join(repo, path)
    try:
        if not stat.S_ISREG(os.lstat(full).st_mode):
            return None
        with open(full, "rb") as handle:
            data = handle.read()
    except FileNotFoundError:
        return None
    except OSError as error:
        raise RepoError(f"cannot read {path}: {error}") from error
    if b"\0" in data[:BINARY_PROBE_SIZE]:
        return None
    return data.decode("utf-8", "surrogateescape").split("\n")


def display(line):
    text = line.rstrip("\r").strip()
    text = text.encode("utf-8", "surrogateescape").decode("utf-8", "replace")
    return text if len(text) <= 200 else text[:197] + "..."


def run_check(repo, check_id):
    files = list_files(repo)
    if check_id in FUNCTION_CHECKS:
        requirement, description, function = FUNCTION_CHECKS[check_id]
        violations, scanned = function(repo, files)
    else:
        requirement, description, rules, skip_fork_changes = CHECKS[check_id]
        violations, scanned = scan_lines(repo, files, check_id, rules, skip_fork_changes)
    label = "Requirements" if "," in requirement else "Requirement"
    if violations:
        for violation in violations:
            print(violation)
        print(f"content_check {check_id}: FAIL, {len(violations)} violation(s) of {label} "
              f"{requirement} ({description})")
        return 1
    print(f"content_check {check_id}: PASS, no {description} ({scanned} files scanned; "
          f"{label} {requirement})")
    return 0


def scan_lines(repo, files, check_id, rules, skip_fork_changes):
    violations = []
    scanned = 0

    if check_id == "deleted_paths":
        listed = set(files)
        for path, path_requirement in DELETED_PATHS:
            prefix = path + "/"
            tracked = path in listed or any(name.startswith(prefix) for name in files)
            if os.path.lexists(os.path.join(repo, path)) or tracked:
                violations.append(f"{path}: exists (Requirement {path_requirement} deletes it)")

    for path in files:
        selected = [regex for scope, regex in rules if scope(path)]
        if not selected:
            continue
        lines = read_text_lines(repo, path)
        if lines is None:
            continue
        scanned += 1
        excluded = set()
        if skip_fork_changes and path == "README.md":
            excluded = fork_changes_lines(lines)
        for number, line in enumerate(lines, 1):
            if number in excluded:
                continue
            if any(regex.search(line) for regex in selected):
                violations.append(f"{path}:{number}: {display(line)}")
    return violations, scanned


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check the repository for removed-platform, Qt, and GTK launcher content.")
    parser.add_argument("--repo", required=True, help="repository root (a Git work tree)")
    parser.add_argument("--check", required=True, choices=sorted([*CHECKS, *FUNCTION_CHECKS]),
                        help="the check to run")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.repo):
        print(f"error: {args.repo} is not a directory", file=sys.stderr)
        return 2
    try:
        return run_check(args.repo, args.check)
    except RepoError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
