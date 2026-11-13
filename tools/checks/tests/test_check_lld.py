"""Unit tests for tools/checks/check_lld.py (Requirement 5.3), on small ELF64 files written here."""

import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest

CHECKS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(CHECKS_DIR, "check_lld.py")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CHECKS_DIR)

import check_lld  # noqa: E402


def elf64(comment=None):
    """A little-endian ELF64 file with a .shstrtab section and, unless comment is None, a .comment
    section holding comment."""
    names = b"\0.shstrtab\0.comment\0"
    data = names + (comment or b"")
    sections = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
                struct.pack("<IIQQQQIIQQ", 1, 3, 0, 0, 64, len(names), 0, 0, 1, 0)]
    if comment is not None:
        sections.append(struct.pack("<IIQQQQIIQQ", 11, 1, 0x30, 0, 64 + len(names), len(comment),
                                    0, 0, 1, 1))
    shoff = 64 + len(data)
    header = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    header += struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(sections),
                          1)
    return header + data + b"".join(sections)


class Run(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, content):
        path = os.path.join(self.temp.name, name)
        with open(path, "wb") as handle:
            handle.write(content)
        return path

    def run_main(self, *paths):
        manifest = self.write("manifest.txt", "".join(f"{path}\n" for path in paths).encode())
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = check_lld.main([manifest])
        return status, stdout.getvalue().splitlines(), stderr.getvalue()


class Notes(unittest.TestCase):
    def test_the_upstream_note(self):
        self.assertEqual(check_lld.lld_version(b"\0clang version 22.1.8\0Linker: LLD 22.1.8\0"),
                         "22.1.8")

    def test_a_distribution_note(self):
        # Ubuntu 24.04's lld 18 writes its name in front of LLD.
        comment = (b"\0GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0\0Ubuntu clang version 18.1.3 "
                   b"(1ubuntu1)\0Linker: Ubuntu LLD 18.1.3\0")
        self.assertEqual(check_lld.lld_version(comment), "18.1.3")

    def test_other_linkers_and_compilers_do_not_count(self):
        for comment in (b"\0GCC: (GNU) 16.1.1\0", b"\0Ubuntu clang version 18.1.3\0",
                        b"\0Linker: GNU ld 2.42\0", b"\0Linker: LLD\0", b"\0LLD 18.1.3\0"):
            with self.subTest(comment=comment):
                self.assertIsNone(check_lld.lld_version(comment))


class Files(Run):
    def test_lld_files_pass(self):
        upstream = self.write("a", elf64(b"\0Linker: LLD 22.1.8\0"))
        ubuntu = self.write("b", elf64(b"\0Ubuntu clang version 18.1.3\0Linker: Ubuntu LLD 18.1.3\0"))
        status, lines, _ = self.run_main(upstream, ubuntu)
        self.assertEqual(status, 0, lines)
        self.assertEqual(lines, [f"LLD {upstream} (22.1.8)", f"LLD {ubuntu} (18.1.3)",
                                 "All 2 executables were linked by LLD."])

    def test_problems_are_listed(self):
        gnu = self.write("gnu", elf64(b"\0GCC: (GNU) 16.1.1\0"))
        bare = self.write("bare", elf64())
        text = self.write("text", b"#!/bin/sh\n")
        missing = os.path.join(self.temp.name, "absent")
        status, lines, _ = self.run_main(gnu, bare, text, missing)
        self.assertEqual(status, 1)
        self.assertEqual(lines, [f"NOT-LLD {gnu} (no LLD note in .comment)",
                                 f"NOT-LLD {bare} (no .comment section)",
                                 f"NOT-LLD {text} (not an ELF file)",
                                 f"MISSING {missing}",
                                 "4 of 4 executables are missing or not linked by LLD."])

    def test_an_empty_manifest_is_an_input_error(self):
        status, lines, stderr = self.run_main()
        self.assertEqual(status, 2)
        self.assertEqual(lines, [])
        self.assertIn("lists no executables", stderr)

    def test_script_exit_status(self):
        path = self.write("a", elf64(b"\0Linker: Ubuntu LLD 18.1.3\0"))
        manifest = self.write("manifest.txt", f"{path}\n".encode())
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, "-B", SCRIPT, manifest], capture_output=True,
                                text=True, env=environment, stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
        self.assertIn("All 1 executables were linked by LLD.", result.stdout)


if __name__ == "__main__":
    unittest.main()
