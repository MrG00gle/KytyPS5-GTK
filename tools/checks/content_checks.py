#!/usr/bin/env python3
"""Repository content checks for the removed platforms (task 8.3), the GTK settings rule, the
Qt removal and GTK launcher rules (task 23.2), and the launcher's own texts for core errors (phase
9 V1).

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

The forbidden terms are built from adjacent string literals, so that this file,
which is inside the scanned scope, does not contain them.

Exit status: 0 when the check passes, 1 when it finds a violation, 2 on a usage
or repository error.
"""

import argparse
import bisect
import os
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
