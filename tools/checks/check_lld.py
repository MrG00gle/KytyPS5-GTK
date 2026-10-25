#!/usr/bin/env python3
"""Check that every first-party executable was linked by LLD.

Requirement 5.3: the Emulator, the launcher, the Updater, and every test
executable are linked with lld. The configure step writes the manifest
${CMAKE_BINARY_DIR}/kyty_lld_executables.txt (one executable path per line,
see kyty_check_lld_executables in cmake/KytyLinker.cmake). This script reads
the .comment section of each ELF64 file, where LLD records
"Linker: LLD <version>", without needing readelf.

Usage:
    check_lld.py MANIFEST

For each path it prints "LLD <path> (<version>)", "NOT-LLD <path>
(<reason>)", or "MISSING <path>".

Exit status: 0 when every executable was linked by LLD, 1 when at least one
is missing or was not linked by LLD, 2 on a usage or input error.
"""

import argparse
import struct
import sys
from pathlib import Path

LLD_MARKER = b"Linker: LLD "
SHN_XINDEX = 0xFFFF


class NotElf64Error(Exception):
    """The file is not a readable ELF64 file."""


def read_comment_section(path):
    """Return the contents of the .comment section, or None if there is none."""
    with open(path, "rb") as handle:
        header = handle.read(64)
        if len(header) < 64 or header[:4] != b"\x7fELF":
            raise NotElf64Error("not an ELF file")
        if header[4] != 2:
            raise NotElf64Error("not an ELF64 file")
        if header[5] == 1:
            order = "<"
        elif header[5] == 2:
            order = ">"
        else:
            raise NotElf64Error("unknown ELF data encoding")

        shoff, = struct.unpack_from(order + "Q", header, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", header, 0x3A)
        if shoff == 0:
            return None
        if shentsize < 64:
            raise NotElf64Error("bad section header size")

        def section_header(index):
            handle.seek(shoff + index * shentsize)
            data = handle.read(64)
            if len(data) < 64:
                raise NotElf64Error("truncated section header table")
            # sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link
            return struct.unpack_from(order + "IIQQQQI", data, 0)

        first = section_header(0)
        if shnum == 0:
            shnum = first[5]
        if shstrndx == SHN_XINDEX:
            shstrndx = first[6]
        if shstrndx >= shnum:
            raise NotElf64Error("bad section name table index")

        def section_data(entry):
            handle.seek(entry[4])
            data = handle.read(entry[5])
            if len(data) < entry[5]:
                raise NotElf64Error("truncated section")
            return data

        names = section_data(section_header(shstrndx))
        for index in range(shnum):
            entry = section_header(index)
            end = names.find(b"\0", entry[0])
            name = names[entry[0]:end if end >= 0 else len(names)]
            if name == b".comment":
                return section_data(entry)
    return None


def lld_version(comment):
    """Return the LLD version recorded in a .comment section, or None."""
    for item in comment.split(b"\0"):
        position = item.find(LLD_MARKER)
        if position >= 0:
            rest = item[position + len(LLD_MARKER):].split()
            return rest[0].decode("utf-8", "replace") if rest else "unknown version"
    return None


def check(path):
    """Return (ok, line) for one manifest entry."""
    if not path.is_file():
        return False, f"MISSING {path}"
    try:
        comment = read_comment_section(path)
    except (OSError, NotElf64Error) as error:
        return False, f"NOT-LLD {path} ({error})"
    if comment is None:
        return False, f"NOT-LLD {path} (no .comment section)"
    version = lld_version(comment)
    if version is None:
        return False, f"NOT-LLD {path} (no LLD note in .comment)"
    return True, f"LLD {path} ({version})"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Fail when an executable listed in the manifest was not "
        "linked by LLD.")
    parser.add_argument("manifest", type=Path,
                        help="kyty_lld_executables.txt written by the configure step")
    args = parser.parse_args(argv)

    try:
        text = args.manifest.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        print(f"error: cannot read manifest {args.manifest}: {error}",
              file=sys.stderr)
        return 2
    paths = [Path(line.strip()) for line in text.splitlines() if line.strip()]
    if not paths:
        print(f"error: manifest {args.manifest} lists no executables",
              file=sys.stderr)
        return 2

    bad = 0
    for path in paths:
        ok, line = check(path)
        print(line)
        if not ok:
            bad += 1

    if bad:
        print(f"{bad} of {len(paths)} executables are missing or not linked by LLD.")
        return 1
    print(f"All {len(paths)} executables were linked by LLD.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
