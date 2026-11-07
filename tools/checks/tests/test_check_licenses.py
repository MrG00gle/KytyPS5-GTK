"""Unit tests for tools/checks/check_licenses.py (task 26.5, Requirements 21.1, 21.4, 21.7)."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CHECKS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(CHECKS_DIR, "check_licenses.py")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CHECKS_DIR)

import check_licenses  # noqa: E402

MANIFEST = ["LICENSE", "Kyty-MIT.txt", "opus/COPYING", "zstd/LICENSE", "zstd/COPYING",
            "xbyak/NOTICE"]
LICENSE_TEXT = b"GNU GENERAL PUBLIC LICENSE\r\nVersion 2\n"
MIT_TEXT = b"MIT License\n\nCopyright (c) Kyty\n"


class CheckLicensesRun(unittest.TestCase):
    """A checkout, a complete licenses directory, its manifest, and a check_licenses.main runner."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.repo = root / "repo"
        self.dir = root / "install" / "share" / "licenses" / "io.example.App"
        self.manifest = root / "licenses-manifest.txt"
        self.write(self.repo / "LICENSE", LICENSE_TEXT)
        self.write(self.repo / "LICENSES" / "Kyty-MIT.txt", MIT_TEXT)
        for entry in MANIFEST:
            self.write(self.dir / entry, f"license text of {entry}\n".encode())
        self.write(self.dir / "LICENSE", LICENSE_TEXT)
        self.write(self.dir / "Kyty-MIT.txt", MIT_TEXT)
        self.write_manifest(MANIFEST)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def write_manifest(self, entries, text=None):
        self.manifest.write_text(text if text is not None else "".join(f"{e}\n" for e in entries),
                                 encoding="utf-8")

    def run_check(self, *extra, repo=False, directory=None):
        arguments = ["--manifest", str(self.manifest), "--dir", str(directory or self.dir)]
        if repo:
            arguments += ["--repo", str(self.repo)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = check_licenses.main(arguments + list(extra))
        return status, stdout.getvalue().splitlines(), stderr.getvalue()


class CompleteTree(CheckLicensesRun):
    def test_complete_tree_passes(self):
        status, lines, _ = self.run_check()
        self.assertEqual(status, 0)
        self.assertEqual(lines, [f"All 6 listed license files are present in {self.dir}."])

    def test_complete_tree_with_repo_passes(self):
        status, lines, _ = self.run_check(repo=True)
        self.assertEqual(status, 0)
        self.assertEqual(len(lines), 1)
        self.assertIn("LICENSE and Kyty-MIT.txt equal the checkout's", lines[0])

    def test_blank_lines_and_spaces_are_ignored(self):
        self.write_manifest(None, text="\n  LICENSE  \n\nKyty-MIT.txt\nzstd/LICENSE\n\n")
        status, lines, _ = self.run_check(repo=True)
        self.assertEqual(status, 0, lines)
        self.assertIn("All 3 listed", lines[0])

    def test_extra_installed_files_are_not_reported(self):
        self.write(self.dir / "extra" / "LICENSE", b"x\n")
        status, _, _ = self.run_check()
        self.assertEqual(status, 0)


class MissingFiles(CheckLicensesRun):
    def test_one_missing_file_is_printed_by_name(self):
        (self.dir / "zstd" / "LICENSE").unlink()
        status, lines, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertEqual([line for line in lines if line.startswith("missing")],
                         ["missing: zstd/LICENSE"])
        self.assertTrue(lines[-1].startswith("FAILED: 1 of 6 listed license files missing"))

    def test_every_missing_file_is_printed_in_manifest_order(self):
        (self.dir / "xbyak" / "NOTICE").unlink()
        (self.dir / "opus" / "COPYING").unlink()
        status, lines, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertEqual(lines[:2], ["missing: opus/COPYING", "missing: xbyak/NOTICE"])
        self.assertTrue(lines[2].startswith("FAILED: 2 of 6"))

    def test_a_directory_is_not_a_license_file(self):
        (self.dir / "zstd" / "COPYING").unlink()
        (self.dir / "zstd" / "COPYING").mkdir()
        status, lines, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("missing: zstd/COPYING", lines)

    def test_absent_directory_reports_every_file(self):
        status, lines, _ = self.run_check(directory=self.dir.parent / "absent")
        self.assertEqual(status, 1)
        self.assertEqual(lines[:6], [f"missing: {entry}" for entry in MANIFEST])

    def test_missing_root_file_with_repo_is_printed_once(self):
        (self.dir / "Kyty-MIT.txt").unlink()
        status, lines, _ = self.run_check(repo=True)
        self.assertEqual(status, 1)
        self.assertEqual([line for line in lines if "Kyty-MIT.txt" in line],
                         ["missing: Kyty-MIT.txt"])


class RepoComparison(CheckLicensesRun):
    def test_one_byte_difference_fails(self):
        self.write(self.dir / "LICENSE", LICENSE_TEXT.replace(b"\r\n", b"\n"))
        status, lines, _ = self.run_check(repo=True)
        self.assertEqual(status, 1)
        self.assertIn(f"differs: LICENSE ({self.repo / 'LICENSE'})", lines)
        self.assertFalse([line for line in lines if line.startswith("missing")])

    def test_difference_is_ignored_without_repo(self):
        self.write(self.dir / "Kyty-MIT.txt", b"changed\n")
        status, _, _ = self.run_check()
        self.assertEqual(status, 0)

    def test_root_file_absent_from_manifest_fails(self):
        self.write_manifest([entry for entry in MANIFEST if entry != "LICENSE"])
        status, lines, _ = self.run_check(repo=True)
        self.assertEqual(status, 1)
        self.assertIn("missing from the manifest: LICENSE", lines)

    def test_unreadable_checkout_file_is_an_input_error(self):
        (self.repo / "LICENSES" / "Kyty-MIT.txt").unlink()
        status, _, stderr = self.run_check(repo=True)
        self.assertEqual(status, 2)
        self.assertIn("cannot read", stderr)


class BadManifest(CheckLicensesRun):
    def test_empty_manifest_is_an_input_error(self):
        self.write_manifest(None, text="\n \n")
        status, lines, stderr = self.run_check()
        self.assertEqual(status, 2)
        self.assertEqual(lines, [])
        self.assertIn("lists no files", stderr)

    def test_unreadable_manifest_is_an_input_error(self):
        self.manifest.unlink()
        status, _, stderr = self.run_check()
        self.assertEqual(status, 2)
        self.assertIn("cannot read manifest", stderr)

    def test_paths_outside_the_directory_are_rejected(self):
        for entry in ("../LICENSE", "/etc/passwd", "zstd/../../x", "zstd//LICENSE", "a\\b"):
            with self.subTest(entry=entry):
                self.write_manifest(["LICENSE", entry])
                status, _, stderr = self.run_check()
                self.assertEqual(status, 2)
                self.assertIn("is not a path inside the licenses directory", stderr)


class CommandLine(CheckLicensesRun):
    def run_script(self, *arguments):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        return subprocess.run([sys.executable, "-B", SCRIPT, *arguments], capture_output=True,
                              text=True, env=environment, stdin=subprocess.DEVNULL, check=False)

    def test_script_exit_status_and_output(self):
        (self.dir / "opus" / "COPYING").unlink()
        result = self.run_script("--manifest", str(self.manifest), "--dir", str(self.dir),
                                 "--repo", str(self.repo))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout.splitlines()[0], "missing: opus/COPYING")

    def test_script_requires_the_manifest_and_the_directory(self):
        result = self.run_script("--dir", str(self.dir))
        self.assertEqual(result.returncode, 2)
        self.assertIn("--manifest", result.stderr)


if __name__ == "__main__":
    unittest.main()
