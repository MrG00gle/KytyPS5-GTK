"""Unit tests for tools/ci/lint_exceptions.py and the committed lint exceptions."""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

CI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(CI_DIR))
SCRIPT = os.path.join(CI_DIR, "lint_exceptions.py")
COMMITTED = os.path.join(REPO, "packaging", "flatpak", "lint-exceptions.json")
APP_ID = "io.github.MrG00gle.KytyPS5_GTK"
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CI_DIR)

import lint_exceptions  # noqa: E402


class Run(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.input = os.path.join(self.temp.name, "lint-exceptions.json")
        self.output = os.path.join(self.temp.name, "run.json")
        shutil.copyfile(COMMITTED, self.input)
        with open(COMMITTED, encoding="utf-8") as handle:
            self.committed_text = handle.read()

    def tearDown(self):
        self.temp.cleanup()

    def run_main(self, *extra, add=("appstream-missing-screenshots",), app_id=APP_ID, output=None):
        arguments = ["--exceptions", self.input, "--app-id", app_id, "--output",
                     output or self.output]
        for lint_id in add:
            arguments += ["--add", lint_id]
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = lint_exceptions.main(arguments + list(extra))
        return status, stdout.getvalue().splitlines(), stderr.getvalue()

    def written(self):
        with open(self.output, encoding="utf-8") as handle:
            return json.load(handle)

    def write_input(self, text):
        with open(self.input, "w", encoding="utf-8") as handle:
            handle.write(text)


class Committed(unittest.TestCase):
    def test_the_committed_file_excepts_exactly_the_approved_ids(self):
        with open(COMMITTED, encoding="utf-8") as handle:
            document = json.load(handle)
        self.assertEqual(document, {APP_ID: ["appid-url-not-reachable",
                                             "appstream-screenshots-not-mirrored-in-ostree"]})

    def test_main_runs_stay_strict_about_screenshots(self):
        with open(COMMITTED, encoding="utf-8") as handle:
            self.assertNotIn("appstream-missing-screenshots", json.load(handle)[APP_ID])


class Copy(Run):
    def test_the_id_is_added_to_a_copy(self):
        status, lines, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertEqual(self.written(), {APP_ID: ["appid-url-not-reachable",
                                                   "appstream-screenshots-not-mirrored-in-ostree",
                                                   "appstream-missing-screenshots"]})
        self.assertEqual(lines, [f"{self.output}: {APP_ID}: appid-url-not-reachable, "
                                 "appstream-screenshots-not-mirrored-in-ostree, "
                                 "appstream-missing-screenshots"])
        with open(self.input, encoding="utf-8") as handle:
            self.assertEqual(handle.read(), self.committed_text)

    def test_an_id_listed_already_is_not_repeated(self):
        status, _, _ = self.run_main(add=("appid-url-not-reachable", "x-y", "x-y"))
        self.assertEqual(status, 0)
        self.assertEqual(self.written()[APP_ID], ["appid-url-not-reachable",
                                                  "appstream-screenshots-not-mirrored-in-ostree",
                                                  "x-y"])

    def test_other_app_ids_are_kept(self):
        self.write_input(json.dumps({"org.example.Other": ["a"], APP_ID: []}))
        status, _, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertEqual(self.written(), {"org.example.Other": ["a"],
                                          APP_ID: ["appstream-missing-screenshots"]})

    def test_the_input_file_is_never_the_output(self):
        status, _, stderr = self.run_main(output=self.input)
        self.assertEqual(status, 2)
        self.assertIn("must not be the --exceptions file", stderr)
        with open(self.input, encoding="utf-8") as handle:
            self.assertEqual(handle.read(), self.committed_text)

    def test_bad_inputs_are_input_errors(self):
        cases = (("not json", "is not JSON"), ("[]", "is not a JSON object"),
                 (json.dumps({APP_ID: "x"}), "has no list of lint IDs"),
                 (json.dumps({APP_ID: [1]}), "has no list of lint IDs"),
                 (json.dumps({"other": []}), "has no list of lint IDs"))
        for text, message in cases:
            with self.subTest(text=text):
                self.write_input(text)
                status, _, stderr = self.run_main()
                self.assertEqual(status, 2)
                self.assertIn(message, stderr)
                self.assertFalse(os.path.exists(self.output))

    def test_a_malformed_lint_id_is_an_input_error(self):
        for lint_id in ("Upper", "two words", "x-", "a--b", ""):
            with self.subTest(lint_id=lint_id):
                status, _, stderr = self.run_main(add=(lint_id,))
                self.assertEqual(status, 2)
                self.assertIn("is not a lint ID", stderr)

    def test_script_exit_status(self):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, "-B", SCRIPT, "--exceptions", self.input,
                                 "--app-id", APP_ID, "--output", self.output],
                                capture_output=True, text=True, env=environment,
                                stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--add", result.stderr)


if __name__ == "__main__":
    unittest.main()
