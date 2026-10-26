#!/usr/bin/env python3
"""Repository content checks for the removed platforms (task 8.3).

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

The forbidden terms are built from adjacent string literals, so that this file,
which is inside the scanned scope, does not contain them.

Exit status: 0 when the check passes, 1 when it finds a violation, 2 on a usage
or repository error.
"""

import argparse
import os
import re
import stat
import subprocess
import sys

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
    requirement, description, rules, skip_fork_changes = CHECKS[check_id]
    label = "Requirements" if "," in requirement else "Requirement"
    files = list_files(repo)
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

    if violations:
        for violation in violations:
            print(violation)
        print(f"content_check {check_id}: FAIL, {len(violations)} violation(s) of {label} "
              f"{requirement} ({description})")
        return 1
    print(f"content_check {check_id}: PASS, no {description} ({scanned} files scanned; "
          f"{label} {requirement})")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check the repository for removed-platform content.")
    parser.add_argument("--repo", required=True, help="repository root (a Git work tree)")
    parser.add_argument("--check", required=True, choices=sorted(CHECKS), help="the check to run")
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
