"""Unit tests for tools/checks/check_licenses.py (task 26.5, Requirements 21.1, 21.4, 21.7; the
release archive mode)."""

import contextlib
import io
import os
import subprocess
import sys
import tarfile
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


class ReleaseArchive(CheckLicensesRun):
    """--archive: a .tar.gz of the portable install, LICENSE at the top and LICENSES/<path>."""

    def setUp(self):
        super().setUp()
        self.portable = Path(self.temp.name) / "portable"
        self.write(self.portable / "kyty_emulator", b"\x7fELF\n")
        self.write(self.portable / "LICENSE", LICENSE_TEXT)
        for entry in MANIFEST[1:]:
            self.write(self.portable / "LICENSES" / entry, (self.dir / entry).read_bytes())
        self.archive = Path(self.temp.name) / "KytyPS5-GTK-2026-11-12-abcdef0-Linux-x86_64.tar.gz"

    def pack(self, prefix="./"):
        """Pack the portable tree the way the workflow does (tar -C <dir> -czf <archive> .)."""
        with tarfile.open(self.archive, "w:gz") as archive:
            archive.add(self.portable, arcname=prefix.rstrip("/") or ".", recursive=True)
        return self.archive

    def run_archive(self, repo=True, archive=None):
        arguments = ["--manifest", str(self.manifest), "--archive", str(archive or self.pack())]
        if repo:
            arguments += ["--repo", str(self.repo)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = check_licenses.main(arguments)
        return status, stdout.getvalue().splitlines(), stderr.getvalue()

    def test_complete_archive_passes(self):
        status, lines, _ = self.run_archive()
        self.assertEqual(status, 0, lines)
        self.assertEqual(len(lines), 1)
        self.assertIn("All 6 listed license files are present in", lines[0])
        self.assertIn("LICENSE and Kyty-MIT.txt equal the checkout's", lines[0])

    def test_member_names_without_the_dot_prefix_pass(self):
        with tarfile.open(self.archive, "w:gz") as archive:
            for path in sorted(self.portable.rglob("*")):
                archive.add(path, arcname=path.relative_to(self.portable).as_posix(),
                            recursive=False)
        status, lines, _ = self.run_archive(archive=self.archive)
        self.assertEqual(status, 0, lines)

    def test_missing_component_file_is_printed_with_its_archive_path(self):
        (self.portable / "LICENSES" / "zstd" / "COPYING").unlink()
        status, lines, _ = self.run_archive()
        self.assertEqual(status, 1)
        self.assertEqual([line for line in lines if line.startswith("missing")],
                         ["missing: LICENSES/zstd/COPYING"])
        self.assertTrue(lines[-1].startswith("FAILED: 1 of 6 listed license files missing"))

    def test_component_file_at_the_flatpak_path_does_not_count(self):
        (self.portable / "LICENSES" / "opus" / "COPYING").unlink()
        self.write(self.portable / "opus" / "COPYING", b"x\n")
        status, lines, _ = self.run_archive(repo=False)
        self.assertEqual(status, 1)
        self.assertIn("missing: LICENSES/opus/COPYING", lines)

    def test_changed_license_fails(self):
        self.write(self.portable / "LICENSE", LICENSE_TEXT.replace(b"\r\n", b"\n"))
        status, lines, _ = self.run_archive()
        self.assertEqual(status, 1)
        self.assertIn(f"differs: LICENSE ({self.repo / 'LICENSE'})", lines)
        self.assertFalse([line for line in lines if line.startswith("missing")])

    def test_changed_kyty_mit_fails(self):
        self.write(self.portable / "LICENSES" / "Kyty-MIT.txt", b"changed\n")
        status, lines, _ = self.run_archive()
        self.assertEqual(status, 1)
        self.assertIn(f"differs: LICENSES/Kyty-MIT.txt ({self.repo / 'LICENSES' / 'Kyty-MIT.txt'})",
                      lines)

    def test_missing_license_with_repo_is_printed_once(self):
        (self.portable / "LICENSE").unlink()
        status, lines, _ = self.run_archive()
        self.assertEqual(status, 1)
        self.assertEqual([line for line in lines if "LICENSE" in line and "LICENSES/" not in line],
                         ["missing: LICENSE"])

    def test_a_directory_member_is_not_a_license_file(self):
        (self.portable / "LICENSES" / "xbyak" / "NOTICE").unlink()
        (self.portable / "LICENSES" / "xbyak" / "NOTICE").mkdir()
        status, lines, _ = self.run_archive(repo=False)
        self.assertEqual(status, 1)
        self.assertIn("missing: LICENSES/xbyak/NOTICE", lines)

    def test_non_gzip_file_is_an_input_error(self):
        self.archive.write_bytes(b"not a tar file\n")
        status, lines, stderr = self.run_archive(archive=self.archive)
        self.assertEqual(status, 2)
        self.assertEqual(lines, [])
        self.assertIn("is not a readable gzip-compressed tar file", stderr)

    def test_truncated_archive_is_an_input_error(self):
        data = self.pack().read_bytes()
        self.archive.write_bytes(data[:len(data) // 2])
        status, _, stderr = self.run_archive(archive=self.archive)
        self.assertEqual(status, 2)
        self.assertIn("is not a readable gzip-compressed tar file", stderr)

    def test_absent_archive_is_an_input_error(self):
        status, _, stderr = self.run_archive(archive=Path(self.temp.name) / "absent.tar.gz")
        self.assertEqual(status, 2)
        self.assertIn("is not a readable gzip-compressed tar file", stderr)


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

    def test_script_requires_a_directory_or_an_archive(self):
        result = self.run_script("--manifest", str(self.manifest))
        self.assertEqual(result.returncode, 2)
        self.assertIn("--dir", result.stderr)

    def test_script_rejects_a_directory_and_an_archive_together(self):
        result = self.run_script("--manifest", str(self.manifest), "--dir", str(self.dir),
                                 "--archive", str(self.manifest))
        self.assertEqual(result.returncode, 2)
        self.assertIn("not allowed with", result.stderr)


if __name__ == "__main__":
    unittest.main()
