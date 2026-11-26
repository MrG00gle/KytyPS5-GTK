"""How the upstream KytyPS5 merge tools treat each path of the repository.

platform_merge.py and sync_status.py put every path that upstream changed into one class:

    removed     The areas the fork removed: the old launcher's sources (src/launcher/src, include,
                forms, icons, styles, and src/launcher/CMakeLists.txt), its tests
                (tests/Launcher*Tests.cpp), its interface, resource, and style files (.ui, .qrc,
                .qss outside 3rdparty/), the removed platforms' sys* sources, and every deleted
                path of tools/checks/content_checks.py (the deleted_paths check). They stay
                deleted; upstream additions in them are left out.
    fork-owned  Files the fork rewrote for itself: .github/, .vscode/, README.md, CONTRIBUTING.md,
                the Markdown files under docs/, the top-level CMakeLists.txt, src/main.cpp,
                .gitignore, LICENSE, LICENSES/, and packaging/. The fork's side is kept, upstream
                additions are left out, and the upstream commits that change them are listed so
                that their intent can be carried over by hand.
    strip       C and C++ sources and headers under src/ and tests/: merged after the removed
                platforms' conditionals are stripped from upstream's side.
    other       Everything else, including vendored code under 3rdparty/: git's own merge.

The names of removed-platform items are built from adjacent string literals, as content_checks.py
does, so that this file does not contain them.
"""

import fnmatch
import os
import sys

# The deleted paths of the deleted_paths check are the single source of truth for that part of the
# removed areas. Keep the import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "checks"))
import content_checks  # noqa: E402

REMOVED = "removed"
FORK_OWNED = "fork-owned"
STRIP = "strip"
OTHER = "other"

OLD_LAUNCHER_DIRS = tuple(f"src/launcher/{name}/"
                          for name in ("src", "include", "forms", "icons", "styles"))
OLD_LAUNCHER_FILES = ("src/launcher/CMakeLists.txt",)
OLD_LAUNCHER_TESTS = "tests/Launcher*Tests.cpp"
OLD_LAUNCHER_EXTENSIONS = content_checks.QT_FILE_EXTENSIONS
REMOVED_PLATFORM_SOURCE = "sys" "Windows"
DELETED_PATHS = tuple(path for path, _ in content_checks.DELETED_PATHS)

FORK_OWNED_FILES = ("CMakeLists.txt", "README.md", "CONTRIBUTING.md", "src/main.cpp", ".gitignore",
                    "LICENSE")
FORK_OWNED_DIRS = (".github/", ".vscode/", "LICENSES/", "packaging/")

STRIP_DIRS = ("src/", "tests/")
STRIP_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc")

# The macros of source_check_platform_macros (tests/source_check/platformMacroScanner.cpp) and the
# conditional directives it reads.
PLATFORM_MACROS = frozenset(("_WIN" "32", "WIN" "32", "KYTY_PLATFORM_" "WINDOWS", "_MSC" "_VER",
                             "__MINGW" "32__", "__MINGW" "64__", "__APP" "LE__"))
CONDITIONAL_DIRECTIVES = frozenset(("if", "ifdef", "ifndef", "elif", "elifdef", "elifndef"))


def is_old_launcher(path):
    """True for the old launcher's sources, tests, and interface, resource, and style files."""
    if path.startswith(OLD_LAUNCHER_DIRS) or path in OLD_LAUNCHER_FILES:
        return True
    if path.startswith("tests/") and "/" not in path[len("tests/"):] \
            and fnmatch.fnmatchcase(path, OLD_LAUNCHER_TESTS):
        return True
    return not path.startswith("3rdparty/") and path.endswith(OLD_LAUNCHER_EXTENSIONS)


def is_removed(path):
    if is_old_launcher(path):
        return True
    if os.path.basename(path).startswith(REMOVED_PLATFORM_SOURCE):
        return True
    return any(path == deleted or path.startswith(deleted + "/") for deleted in DELETED_PATHS)


def is_fork_owned(path):
    if path in FORK_OWNED_FILES or path.startswith(FORK_OWNED_DIRS):
        return True
    return path.startswith("docs/") and path.endswith(".md")


def is_strip(path):
    return path.startswith(STRIP_DIRS) and path.endswith(STRIP_EXTENSIONS)


def classify(path):
    """The class of path: REMOVED, FORK_OWNED, STRIP, or OTHER, checked in that order."""
    if is_removed(path):
        return REMOVED
    if is_fork_owned(path):
        return FORK_OWNED
    if is_strip(path):
        return STRIP
    return OTHER


def area(path):
    """The area of path as upstream-commits.tsv groups them: "(root)" for a top-level file,
    "src(top)" for a file directly in src/, three components under src/graphics/ and
    src/launcher/, two under src/, .github/, and 3rdparty/, and the top directory otherwise."""
    parts = path.split("/")
    if len(parts) == 1:
        return "(root)"
    if parts[0] == "src":
        if len(parts) == 2:
            return "src(top)"
        if parts[1] in ("graphics", "launcher") and len(parts) > 3:
            return "/".join(parts[:3])
        return "/".join(parts[:2])
    if parts[0] in (".github", "3rdparty"):
        return "/".join(parts[:2])
    return parts[0]


def _splice(text):
    """[(first line number, logical line)] with backslash-newline continuations joined."""
    lines = text.split("\n")
    logical = []
    index = 0
    while index < len(lines):
        first = index
        line = lines[index].rstrip("\r")
        while line.endswith("\\") and index + 1 < len(lines):
            index += 1
            line = line[:-1] + lines[index].rstrip("\r")
        logical.append((first + 1, line))
        index += 1
    return logical


def _code(line, in_comment):
    """(line without comments, string and character literals, whether a block comment is open at
    its end). A quote between two identifier characters is a digit separator."""
    out = []
    index = 0
    while index < len(line):
        if in_comment:
            end = line.find("*/", index)
            if end < 0:
                return "".join(out), True
            index = end + 2
            in_comment = False
            out.append(" ")
            continue
        char = line[index]
        if line.startswith("//", index):
            break
        if line.startswith("/*", index):
            in_comment = True
            index += 2
            continue
        if char == "'" and 0 < index < len(line) - 1 and line[index - 1].isalnum() \
                and (line[index + 1].isalnum() or line[index + 1] == "_"):
            index += 1
            continue
        if char in "\"'":
            index += 1
            while index < len(line) and line[index] != char:
                index += 2 if line[index] == "\\" else 1
            index += 1
            out.append(" ")
            continue
        out.append(char)
        index += 1
    return "".join(out), in_comment


def _identifiers(code):
    found = []
    word = []
    for char in code + " ":
        if char.isalnum() or char == "_":
            word.append(char)
        elif word:
            if not word[0].isdigit():
                found.append("".join(word))
            word = []
    return found


def platform_directives(text):
    """[(line number, directive text)] of the conditional directives in text (C or C++ source)
    that name a removed-platform macro, as source_check_platform_macros reports them. A heuristic
    lexer: comments, string literals, and line continuations are handled, raw strings are not."""
    findings = []
    in_comment = False
    for number, line in _splice(text):
        starts_in_comment = in_comment
        code, in_comment = _code(line, in_comment)
        if starts_in_comment or not code.lstrip().startswith("#"):
            continue
        words = _identifiers(code.lstrip()[1:])
        if words and words[0] in CONDITIONAL_DIRECTIVES \
                and any(word in PLATFORM_MACROS for word in words[1:]):
            findings.append((number, " ".join(line.split())))
    return findings
