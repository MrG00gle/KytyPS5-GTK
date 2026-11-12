"""Unit tests for tools/docs/check_help_options.py (task 29.7), with fake emulators: shell
scripts that print a help text, fail, print no option, or never exit."""

import contextlib
import io
import os
import shutil
import sys
import tempfile
import textwrap
import time
import unittest

DOCS_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, DOCS_TOOLS)

import check_help_options  # noqa: E402

HELP = """\
kyty_emulator --game <dir|elf|zar> [options]

Options:
  --game <dir|elf|zar>             Game directory, ELF, or ZArchive to load.
  --gpu <index>                    Vulkan physical device index. Default: auto.
  --gpu-assisted-validation        Bounds-check shader accesses on the GPU.
                                   Implies --vulkan-validation; very slow.
  --vulkan-validation              Enable Vulkan validation.
  --keymap=<Control=Input>         DualSense mapping; may be repeated.
  --help                           Print this help and exit.
  -h                               Same as --help.
"""
DOC = """\
# Running

| Option | Effect |
|---|---|
| `--game` | The game. |
| `--gpu` | The GPU. |
| `--gpu-assisted-validation` | Bounds checks. |
| `--vulkan-validation` | Validation. |
| `--keymap` | One mapping. |
| `--help` | Usage. |
"""


class FakeEmulator(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="kyty_check_help_options_")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.doc = self.write("running.md", DOC)
        self.help = self.write("help.txt", HELP)

    def write(self, name, text):
        path = os.path.join(self.root, name)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return path

    def emulator(self, body):
        path = self.write("kyty_emulator", "#!/bin/sh\n" + textwrap.dedent(body))
        os.chmod(path, 0o755)
        return path

    def run_main(self, emulator, *extra, doc=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        arguments = ["--emulator", emulator, "--doc", doc or self.doc, *extra]
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = check_help_options.main(arguments)
        return status, stdout.getvalue().splitlines(), stderr.getvalue()


class Options(FakeEmulator):
    def test_every_option_documented_passes(self):
        emulator = self.emulator(f'[ "$1" = --help ] || exit 3\ncat "{self.help}"\n')
        status, lines, _ = self.run_main(emulator)
        self.assertEqual(status, 0, lines)
        self.assertEqual(lines, ["PASS: docs_help_options: all 6 options of kyty_emulator --help "
                                 f"appear in {self.doc}"])

    def test_a_removed_option_row_fails_with_the_option(self):
        self.write("running.md", DOC.replace("| `--keymap` | One mapping. |\n", ""))
        emulator = self.emulator(f'cat "{self.help}"\n')
        status, lines, _ = self.run_main(emulator)
        self.assertEqual(status, 1)
        self.assertEqual(lines, [
            f"{self.doc} lacks the option --keymap",
            f"FAILED: docs_help_options: 1 of the 6 options of kyty_emulator --help missing from "
            f"{self.doc} (Requirements 20.3, 20.12)"])

    def test_an_option_only_inside_a_longer_one_is_missing(self):
        self.write("running.md", DOC.replace("| `--gpu` | The GPU. |\n", ""))
        status, lines, _ = self.run_main(self.emulator(f'cat "{self.help}"\n'))
        self.assertEqual(status, 1)
        self.assertEqual(lines[0], f"{self.doc} lacks the option --gpu")
        self.assertEqual(len(lines), 2)

    def test_every_missing_option_is_printed(self):
        self.write("running.md", "# Running\n\nNo table yet.\n")
        status, lines, _ = self.run_main(self.emulator(f'cat "{self.help}"\n'))
        self.assertEqual(status, 1)
        self.assertEqual([line.rsplit(" ", 1)[-1] for line in lines[:-1]],
                         ["--game", "--gpu", "--gpu-assisted-validation", "--vulkan-validation",
                          "--keymap", "--help"])

    def test_option_names_in_the_help_text(self):
        self.assertEqual(check_help_options.help_options(
            "  --a-b <x>  text --c=1 [--d] (--e) x--f ---g --h_i, -j --9\n--a-b\n"),
            ["--a-b", "--c", "--d", "--e", "--h_i", "--9"])


class Failures(FakeEmulator):
    def test_a_non_zero_exit_fails_and_shows_stderr(self):
        emulator = self.emulator(f'cat "{self.help}"\necho "unknown option" >&2\nexit 1\n')
        status, lines, _ = self.run_main(emulator)
        self.assertEqual(status, 1)
        self.assertEqual(lines, [
            "unknown option", f"{emulator} --help exited with status 1",
            "FAILED: docs_help_options: the emulator's --help cannot be checked "
            "(Requirement 20.12)"])

    def test_a_signal_death_fails(self):
        status, lines, _ = self.run_main(self.emulator("kill -9 $$\n"))
        self.assertEqual(status, 1)
        self.assertIn("--help was killed by signal 9", lines[0])

    def test_no_options_fails(self):
        status, lines, _ = self.run_main(self.emulator("echo 'usage: kyty_emulator -h'\n"))
        self.assertEqual(status, 1)
        self.assertIn("--help printed no option name (a word that begins with --)", lines[0])

    def test_options_on_stderr_only_do_not_count(self):
        status, lines, _ = self.run_main(self.emulator(f'cat "{self.help}" >&2\n'))
        self.assertEqual(status, 1)
        self.assertIn("printed no option name", lines[0])

    def test_a_timeout_fails_quickly(self):
        emulator = self.emulator("exec sleep 30\n")
        started = time.monotonic()
        status, lines, _ = self.run_main(emulator, "--timeout", "0.5")
        self.assertLess(time.monotonic() - started, 10)
        self.assertEqual(status, 1)
        self.assertEqual(lines[0], f"{emulator} --help did not exit within 0.5 s")

    def test_a_timeout_also_stops_the_emulator_s_children(self):
        emulator = self.emulator("sleep 30 &\nsleep 30\n")
        started = time.monotonic()
        status, lines, _ = self.run_main(emulator, "--timeout", "0.5")
        self.assertLess(time.monotonic() - started, 10)
        self.assertEqual(status, 1)
        self.assertIn("did not exit within 0.5 s", lines[0])

    def test_an_emulator_that_cannot_start(self):
        missing = os.path.join(self.root, "missing")
        status, lines, _ = self.run_main(missing)
        self.assertEqual(status, 1)
        self.assertTrue(lines[0].startswith(f"cannot run {missing}: "), lines)


class InputErrors(FakeEmulator):
    def test_an_unreadable_guide(self):
        status, lines, stderr = self.run_main(self.emulator(f'cat "{self.help}"\n'),
                                              doc=os.path.join(self.root, "missing.md"))
        self.assertEqual((status, lines), (2, []))
        self.assertIn("cannot read", stderr)

    def test_a_bad_timeout(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for value in ("0", "-1", "soon"):
                with self.assertRaises(SystemExit) as raised:
                    check_help_options.main(["--emulator", "x", "--doc", self.doc,
                                             "--timeout", value])
                self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
