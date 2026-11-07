#!/usr/bin/env python3
"""Check an installed licenses directory against licenses-manifest.txt.

Requirements 21.4 and 21.7: the Flatpak package installs, under
/app/share/licenses/<App_ID>, every license file that the configure step lists
in licenses-manifest.txt (cmake/KytyLicenses.cmake; installed as
/app/share/kyty/licenses-manifest.txt). The manifest has one path per line,
relative to that directory. This script prints "missing: <path>" for every
listed path that is not a regular file there.

With --repo it also requires LICENSE and Kyty-MIT.txt in that directory to be
byte-identical to the checkout's LICENSE and LICENSES/Kyty-MIT.txt
(Requirement 21.1). It prints "missing: <name>" when one is absent,
"differs: <name> (<checkout file>)" when its bytes differ, and
"missing from the manifest: <name>" when the manifest does not list it.

Usage:
    check_licenses.py --manifest FILE --dir DIR [--repo CHECKOUT]

Exit status: 0 when every listed file is present (and, with --repo, both root
files are listed and identical), 1 otherwise, 2 on a usage or input error (an
unreadable or empty manifest, a manifest path outside the directory, or an
unreadable checkout file).
"""

import argparse
import sys
from pathlib import Path, PurePosixPath

# (name in the licenses directory, path in the checkout)
REPO_FILES = (("LICENSE", "LICENSE"), ("Kyty-MIT.txt", "LICENSES/Kyty-MIT.txt"))


class ManifestError(Exception):
    """The manifest cannot be used."""


def read_manifest(path):
    """Return the manifest's paths in order, without blank lines."""
    entries = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for number, line in enumerate(lines, 1):
        entry = line.strip()
        if not entry:
            continue
        parts = entry.split("/")
        if PurePosixPath(entry).is_absolute() or "\\" in entry or any(
                part in ("", ".", "..") for part in parts):
            raise ManifestError(
                f"line {number}: {entry!r} is not a path inside the licenses directory")
        entries.append(entry)
    if not entries:
        raise ManifestError("lists no files")
    return entries


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Print every license file that licenses-manifest.txt lists and the "
        "licenses directory lacks; exit 1 if there is one.")
    parser.add_argument("--manifest", type=Path, required=True,
                        help="licenses-manifest.txt (the build tree's or the installed copy)")
    parser.add_argument("--dir", type=Path, required=True,
                        help="the installed licenses directory, share/licenses/<App_ID>")
    parser.add_argument("--repo", type=Path,
                        help="a checkout: LICENSE and Kyty-MIT.txt must equal its LICENSE and "
                        "LICENSES/Kyty-MIT.txt byte for byte")
    args = parser.parse_args(argv)

    try:
        entries = read_manifest(args.manifest)
    except (OSError, UnicodeDecodeError) as error:
        print(f"error: cannot read manifest {args.manifest}: {error}", file=sys.stderr)
        return 2
    except ManifestError as error:
        print(f"error: manifest {args.manifest}: {error}", file=sys.stderr)
        return 2

    missing = [entry for entry in entries if not (args.dir / entry).is_file()]
    for entry in missing:
        print(f"missing: {entry}")
    problems = len(missing)

    if args.repo is not None:
        for name, repo_path in REPO_FILES:
            reference = args.repo / repo_path
            try:
                expected = reference.read_bytes()
            except OSError as error:
                print(f"error: cannot read {reference}: {error}", file=sys.stderr)
                return 2
            if name not in entries:
                print(f"missing from the manifest: {name}")
                problems += 1
            installed = args.dir / name
            if not installed.is_file():
                if name not in missing:
                    print(f"missing: {name}")
                    problems += 1
                continue
            try:
                identical = installed.read_bytes() == expected
            except OSError as error:
                print(f"error: cannot read {installed}: {error}", file=sys.stderr)
                return 2
            if not identical:
                print(f"differs: {name} ({reference})")
                problems += 1

    if problems:
        print(f"FAILED: {len(missing)} of {len(entries)} listed license files missing in "
              f"{args.dir}, {problems - len(missing)} other problem(s).")
        return 1
    suffix = "; LICENSE and Kyty-MIT.txt equal the checkout's" if args.repo is not None else ""
    print(f"All {len(entries)} listed license files are present in {args.dir}{suffix}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
