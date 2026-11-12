#!/usr/bin/env python3
"""Check installed license files against licenses-manifest.txt.

Requirements 21.4 and 21.7: the Flatpak package installs, under
/app/share/licenses/<App_ID>, every license file that the configure step lists
in licenses-manifest.txt (cmake/KytyLicenses.cmake; installed as
/app/share/kyty/licenses-manifest.txt). The manifest has one path per line,
relative to that directory. This script prints "missing: <path>" for every
listed path that is not a regular file there.

With --archive instead of --dir it checks a release archive
(<tag>-Linux-x86_64.tar.gz, a gzip-compressed tar of the portable install).
The portable layout holds the files the way the repository holds its own two:
LICENSE at the top of the archive and every other manifest path under
LICENSES/, so LICENSES/Kyty-MIT.txt and LICENSES/<component>/<file>. Member
names may carry a "./" prefix (what `tar -C <dir> -czf <archive> .` writes).
Only regular file members count. Missing files are printed with their path in
the archive.

With --repo it also requires LICENSE and Kyty-MIT.txt (in the archive,
LICENSE and LICENSES/Kyty-MIT.txt) to be byte-identical to the checkout's
LICENSE and LICENSES/Kyty-MIT.txt (Requirement 21.1). It prints
"missing: <name>" when one is absent, "differs: <name> (<checkout file>)" when
its bytes differ, and "missing from the manifest: <name>" when the manifest
does not list it.

Usage:
    check_licenses.py --manifest FILE (--dir DIR | --archive TAR_GZ) [--repo CHECKOUT]

Exit status: 0 when every listed file is present (and, with --repo, both root
files are listed and identical), 1 otherwise, 2 on a usage or input error (an
unreadable or empty manifest, a manifest path outside the directory, an
unreadable checkout file, or an archive that is not a readable gzip-compressed
tar file).
"""

import argparse
import sys
import tarfile
import zlib
from pathlib import Path, PurePosixPath

# (manifest path, path in the checkout)
REPO_FILES = (("LICENSE", "LICENSE"), ("Kyty-MIT.txt", "LICENSES/Kyty-MIT.txt"))
REPO_NAMES = tuple(name for name, _ in REPO_FILES)


class ManifestError(Exception):
    """The manifest cannot be used."""


class TreeError(Exception):
    """The licenses directory or the archive cannot be read."""


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


class DirectoryTree:
    """An installed licenses directory: each manifest path is a file below it."""

    def __init__(self, root):
        self.root = root
        self.label = str(root)

    @staticmethod
    def location(entry):
        return entry

    def has(self, entry):
        return (self.root / entry).is_file()

    def read(self, entry):
        try:
            return (self.root / entry).read_bytes()
        except OSError as error:
            raise TreeError(f"cannot read {self.root / entry}: {error}") from error


def normalized_member_name(name):
    """Drop "./" prefixes from a tar member name."""
    while name.startswith("./"):
        name = name[2:]
    return name


class ArchiveTree:
    """A release archive in the portable layout: LICENSE at the top, the rest under LICENSES/."""

    def __init__(self, path):
        self.label = str(path)
        self.files = set()
        self.contents = {}
        wanted = {self.location(name) for name in REPO_NAMES}
        try:
            # Reads the whole archive once; a truncated or corrupt one fails here.
            with tarfile.open(path, "r:gz") as archive:
                for member in archive:
                    if not member.isfile():
                        continue
                    name = normalized_member_name(member.name)
                    self.files.add(name)
                    if name in wanted:
                        self.contents[name] = archive.extractfile(member).read()
        except (OSError, EOFError, tarfile.TarError, zlib.error) as error:
            raise TreeError(f"{path} is not a readable gzip-compressed tar file: "
                            f"{error}") from error

    @staticmethod
    def location(entry):
        return entry if entry == "LICENSE" else f"LICENSES/{entry}"

    def has(self, entry):
        return self.location(entry) in self.files

    def read(self, entry):
        return self.contents[self.location(entry)]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Print every license file that licenses-manifest.txt lists and the "
        "licenses directory or the release archive lacks; exit 1 if there is one.")
    parser.add_argument("--manifest", type=Path, required=True,
                        help="licenses-manifest.txt (the build tree's or the installed copy)")
    where = parser.add_mutually_exclusive_group(required=True)
    where.add_argument("--dir", type=Path,
                       help="the installed licenses directory, share/licenses/<App_ID>")
    where.add_argument("--archive", type=Path,
                       help="a release archive (.tar.gz) of the portable install: LICENSE at its "
                       "top, every other listed file under LICENSES/")
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

    try:
        tree = DirectoryTree(args.dir) if args.dir is not None else ArchiveTree(args.archive)
    except TreeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    missing = [entry for entry in entries if not tree.has(entry)]
    for entry in missing:
        print(f"missing: {tree.location(entry)}")
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
            if not tree.has(name):
                if name not in missing:
                    print(f"missing: {tree.location(name)}")
                    problems += 1
                continue
            try:
                identical = tree.read(name) == expected
            except TreeError as error:
                print(f"error: {error}", file=sys.stderr)
                return 2
            if not identical:
                print(f"differs: {tree.location(name)} ({reference})")
                problems += 1

    if problems:
        print(f"FAILED: {len(missing)} of {len(entries)} listed license files missing in "
              f"{tree.label}, {problems - len(missing)} other problem(s).")
        return 1
    suffix = "; LICENSE and Kyty-MIT.txt equal the checkout's" if args.repo is not None else ""
    print(f"All {len(entries)} listed license files are present in {tree.label}{suffix}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
