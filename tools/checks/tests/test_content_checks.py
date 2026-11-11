"""Unit tests for tools/checks/content_checks.py: the core_text check (phase 9 V1, Requirement
6.12) on scratch trees.

core_text keeps the GTK launcher from showing a Launcher_Core or Updater message as it is: a read of
a .message, .reason, or .error member (also through ->), .error().c_str(), .Error(),
RefusalMessage(, or a core::k...Message / updater::k...Message constant in src/launcher/gtk/
fails unless it is inside the arguments of a log call or its line ends with "// core text: <why>".
coreMessageText.cpp (the mapping) and services/ (the library adapters) are not scanned.
"""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

CHECKS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CHECKS_DIR)

import content_checks  # noqa: E402

GTK = "src/launcher/gtk"


class ScratchTree(unittest.TestCase):
    """A temporary tree; files are written relative to it."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="kyty_content_checks_")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def write(self, path, text):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(textwrap.dedent(text))
        return path

    def violations(self, *paths):
        violations, _ = content_checks.check_core_text(self.root, sorted(paths))
        return violations


class CoreTextReads(ScratchTree):
    def test_a_raw_message_in_a_setter_fails(self):
        path = self.write(f"{GTK}/window.cpp", """\
            void Show(GtkLabel* label, const core::IoError& error) {
            \tgtk_label_set_text(label, error.message.c_str());
            }
            """)
        violations = self.violations(path)
        self.assertEqual(len(violations), 1)
        self.assertTrue(violations[0].startswith(f"{path}:2: "), violations[0])
        self.assertIn("core text .message", violations[0])

    def test_reason_error_and_arrow_reads_fail(self):
        path = self.write(f"{GTK}/window.cpp", """\
            std::string a = warning.reason;
            Toast(self, heading, error->message);
            Banner(store.load_error->reason);
            if (!result.error.empty()) {}
            bool same = a.error == b;
            """)
        lines = [violation.split(":")[1] for violation in self.violations(path)]
        self.assertEqual(lines, ["1", "2", "3", "4", "5"])

    def test_log_call_arguments_pass(self):
        path = self.write(f"{GTK}/window.cpp", """\
            g_message("cannot save %s: %s", path.c_str(), error.message.c_str());
            g_debug("update: %s%s", a.c_str(),
                    outcome.message.empty() ? "" : outcome.message.c_str());
            g_warning("%s", job.Error().c_str());
            g_critical("%s", prepared.error().c_str());
            g_info("%s", core::kNoTrophyPackageMessage);
            """)
        self.assertEqual(self.violations(path), [])

    def test_the_arguments_of_a_log_call_end_at_its_parenthesis(self):
        path = self.write(f"{GTK}/window.cpp", """\
            g_message("%s", error.message.c_str()); Show(error.message);
            """)
        violations = self.violations(path)
        self.assertEqual(len(violations), 1)
        self.assertTrue(violations[0].startswith(f"{path}:1: "))

    def test_a_marked_line_passes(self):
        path = self.write(f"{GTK}/window.cpp", """\
            const std::string& sdl = controller.error; // core text: SDL's message (library text)
            const char* text = error->message; // core text: GError's message
            if (m_callbacks.error) { // core text: a callback, not a text
            """)
        self.assertEqual(self.violations(path), [])

    def test_a_marker_needs_a_reason(self):
        path = self.write(f"{GTK}/window.cpp", """\
            const std::string& sdl = controller.error; // core text:
            const std::string& other = controller.error; // the core text: SDL's
            """)
        self.assertEqual(len(self.violations(path)), 2)

    def test_calls_and_assignments_are_not_reads(self):
        path = self.write(f"{GTK}/window.cpp", """\
            m_callbacks.error(_("The game cannot start"), body);
            run_callbacks.error = [self](const std::string& h, const std::string& b) {};
            const auto& error = saved.error();
            if (saved.error().kind == core::CheatSaveErrorKind::ChangedOnDisk) {}
            Show(IoErrorText(written.error()));
            state.messages++;
            list.reasons.clear();
            """)
        self.assertEqual(self.violations(path), [])

    def test_error_strings_and_archive_errors_fail(self):
        path = self.write(f"{GTK}/window.cpp", """\
            Refuse(Format(_("cannot: %s"), writable.error().c_str()));
            SetPage(page, heading, job.Error());
            """)
        self.assertEqual(len(self.violations(path)), 2)

    def test_core_constants_and_refusal_messages_fail(self):
        path = self.write(f"{GTK}/window.cpp", """\
            Show(core::kNoTrophyPackageMessage);
            Show(updater::kCloseGameMessage);
            Show(run_state.RefusalMessage());
            Show(core::kMaxImageBytes);
            """)
        lines = [violation.split(":")[1] for violation in self.violations(path)]
        self.assertEqual(lines, ["1", "2", "3"])

    def test_comments_and_string_literals_are_not_read(self):
        path = self.write(f"{GTK}/window.h", """\
            // Shows error.message inside the launcher's text.
            /* warning.reason */
            const char* help = "error.message and job.Error()";
            """)
        self.assertEqual(self.violations(path), [])


class CoreTextScope(ScratchTree):
    def test_the_mapping_services_and_other_directories_are_not_scanned(self):
        raw = "Show(error.message);\n"
        paths = [
            self.write(f"{GTK}/coreMessageText.cpp", raw),
            self.write(f"{GTK}/services/soupHttpClient.cpp", raw),
            self.write("src/launcher/core/cheats.cpp", raw),
            self.write("tests/launcher_gtk/Tests.cpp", raw),
            self.write(f"{GTK}/resources/ui/window.xml", raw),
        ]
        violations, scanned = content_checks.check_core_text(self.root, sorted(paths))
        self.assertEqual(violations, [])
        self.assertEqual(scanned, 0)

    def test_headers_and_sources_are_scanned(self):
        paths = [self.write(f"{GTK}/a.h", "Show(e.message);\n"),
                 self.write(f"{GTK}/b.cpp", "Show(e.message);\n")]
        violations, scanned = content_checks.check_core_text(self.root, sorted(paths))
        self.assertEqual(scanned, 2)
        self.assertEqual(len(violations), 2)


class CommandLine(ScratchTree):
    def run_main(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = content_checks.main(["--repo", self.root, "--check", "core_text"])
        return status, stdout.getvalue().splitlines()

    def test_a_scratch_repository(self):
        subprocess.run(["git", "init", "-q", self.root], check=True)
        path = self.write(f"{GTK}/window.cpp", "Show(error.message);\n")
        status, lines = self.run_main()
        self.assertEqual(status, 1)
        self.assertEqual(lines[0].split(": ")[0], f"{path}:1")
        self.assertTrue(lines[-1].startswith("content_check core_text: FAIL, 1 violation(s)"),
                        lines[-1])
        self.write(f"{GTK}/window.cpp", "Show(Render(IoErrorText(error)));\n")
        status, lines = self.run_main()
        self.assertEqual(status, 0)
        self.assertTrue(lines[-1].startswith("content_check core_text: PASS"), lines[-1])
        self.assertIn("(1 files scanned; Requirement 6.12)", lines[-1])


if __name__ == "__main__":
    unittest.main()
