#!/usr/bin/env python3
"""License and copyright preservation checks (task 8.3).

Usage:
    check_preservation.py --repo DIR --check license|copyright
                          [--license-list FILE] [--copyright-list FILE]

license (Requirement 21.1): every file in main-license-sha256.txt
(`<sha256>  <path>`, the sha256sum format) exists at its recorded path in DIR
with the recorded SHA-256.

copyright (Requirement 21.6): every recorded line of main-copyright-lines.txt
(`<path>:<line number>:<line text>`, line numbers informational) is still a
line of its file. When a recorded path is missing, its renames are followed
through `git log --reverse --diff-filter=R -M --name-status <main sha>..HEAD`
(the Main_Branch SHA comes from the list's "# Main_Branch SHA:" header). The
check fails when that history is unavailable, and when neither the recorded
path nor the path it was renamed to exists.

Exit status: 0 when the check passes, 1 when it finds a violation, 2 on a usage
or input error (including an unavailable rename history).
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

CHECKS_DIR = Path(__file__).resolve().parent
DEFAULT_LICENSE_LIST = CHECKS_DIR / "main-license-sha256.txt"
DEFAULT_COPYRIGHT_LIST = CHECKS_DIR / "main-copyright-lines.txt"
MAIN_SHA_HEADER = re.compile(r"^#\s*Main_Branch SHA:\s*([0-9a-f]{40})\s*$")


class InputError(Exception):
    pass


def read_lines(path):
    try:
        return Path(path).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from error


def check_license(repo, list_path):
    records = []
    for number, line in enumerate(read_lines(list_path), 1):
        if not line.strip() or line.startswith("#"):
            continue
        match = re.fullmatch(r"([0-9a-f]{64}) [ *](.+)", line)
        if match is None:
            raise InputError(f"{list_path}:{number}: not a '<sha256>  <path>' line")
        records.append((match.group(2), match.group(1)))
    if not records:
        raise InputError(f"{list_path} lists no files")

    violations = []
    for path, expected in records:
        full = os.path.join(repo, path)
        if not os.path.isfile(full):
            violations.append(f"{path}: missing (Requirement 21.1 keeps it at this path)")
            continue
        with open(full, "rb") as handle:
            actual = hashlib.sha256(handle.read()).hexdigest()
        if actual != expected:
            violations.append(f"{path}: SHA-256 {actual} differs from the recorded {expected}")

    if violations:
        for violation in violations:
            print(violation)
        print(f"preservation license: FAIL, {len(violations)} of {len(records)} files differ from "
              f"their Main_Branch version (Requirement 21.1)")
        return 1
    print(f"preservation license: PASS, {len(records)} files match their recorded SHA-256 "
          f"(Requirement 21.1)")
    return 0


def read_copyright_list(list_path):
    main_sha = None
    records = []
    for number, line in enumerate(read_lines(list_path), 1):
        header = MAIN_SHA_HEADER.match(line)
        if header:
            main_sha = header.group(1)
            continue
        if not line or line.startswith("#"):
            continue
        parts = line.split(":", 2)
        if len(parts) != 3 or not parts[0] or not parts[1].isdigit():
            raise InputError(f"{list_path}:{number}: not a '<path>:<line>:<text>' line")
        records.append((parts[0], int(parts[1]), parts[2]))
    if main_sha is None:
        raise InputError(f"{list_path} has no '# Main_Branch SHA: <sha>' header")
    if not records:
        raise InputError(f"{list_path} lists no lines")
    return main_sha, records


def read_renames(repo, main_sha):
    """(old, new) pairs of every rename in main_sha..HEAD, oldest first."""
    command = ["git", "-C", repo, "log", "--reverse", "--diff-filter=R", "-M", "--name-status",
               "-z", "--format=", f"{main_sha}..HEAD"]
    try:
        result = subprocess.run(command, capture_output=True, check=False)
    except OSError as error:
        raise InputError(f"cannot run git to follow renames: {error}") from error
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise InputError(f"cannot follow renames: the history {main_sha}..HEAD is unavailable in "
                         f"{repo} ({message})")
    fields = [field.decode("utf-8", "surrogateescape").strip("\n")
              for field in result.stdout.split(b"\0")]
    renames = []
    index = 0
    while index < len(fields):
        if re.fullmatch(r"R[0-9]*", fields[index]) and index + 2 < len(fields):
            renames.append((fields[index + 1], fields[index + 2]))
            index += 3
        else:
            index += 1
    return renames


def check_copyright(repo, list_path):
    main_sha, records = read_copyright_list(list_path)
    paths = sorted({path for path, _, _ in records})
    renames = None
    current = {}
    violations = []
    for path in paths:
        if os.path.isfile(os.path.join(repo, path)):
            current[path] = path
            continue
        if renames is None:
            renames = read_renames(repo, main_sha)
        target = path
        for old, new in renames:
            if old == target:
                target = new
        if target != path and os.path.isfile(os.path.join(repo, target)):
            print(f"{path}: renamed to {target}")
            current[path] = target
        elif target != path:
            violations.append(f"{path}: missing; renamed to {target}, which is missing too "
                              f"(Requirement 21.6)")
        else:
            violations.append(f"{path}: missing, and not renamed in {main_sha[:12]}..HEAD "
                              f"(Requirement 21.6)")

    file_lines = {}
    for path, number, text in records:
        if path not in current:
            continue
        target = current[path]
        if target not in file_lines:
            raw = Path(repo, target).read_bytes().decode("utf-8", "surrogateescape")
            file_lines[target] = {line.rstrip("\r") for line in raw.split("\n")}
        if text not in file_lines[target]:
            shown = target if target == path else f"{target} (was {path})"
            violations.append(f"{shown}: recorded line {number} changed or removed: {text}")

    if violations:
        for violation in violations:
            print(violation)
        print(f"preservation copyright: FAIL, {len(violations)} violation(s) of Requirement 21.6")
        return 1
    print(f"preservation copyright: PASS, {len(records)} recorded lines present in "
          f"{len(paths)} files (Requirement 21.6)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check that license files and copyright lines are preserved.")
    parser.add_argument("--repo", required=True, help="repository root (a Git work tree)")
    parser.add_argument("--check", required=True, choices=("license", "copyright"))
    parser.add_argument("--license-list", default=DEFAULT_LICENSE_LIST,
                        help="recorded license hashes (default: %(default)s)")
    parser.add_argument("--copyright-list", default=DEFAULT_COPYRIGHT_LIST,
                        help="recorded copyright lines (default: %(default)s)")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.repo):
        print(f"error: {args.repo} is not a directory", file=sys.stderr)
        return 2
    try:
        if args.check == "license":
            return check_license(args.repo, args.license_list)
        return check_copyright(args.repo, args.copyright_list)
    except (InputError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
