"""Unit tests for tools/ci/metainfo_releases.py (Requirement 14.8, official-run release lists)."""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

CI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(CI_DIR))
SCRIPT = os.path.join(CI_DIR, "metainfo_releases.py")
METAINFO = os.path.join(REPO, "packaging", "linux", "io.github.MrG00gle.KytyPS5_GTK.metainfo.xml")
# Keep the local imports below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CI_DIR)
sys.path.insert(0, os.path.join(REPO, "tools", "checks"))

import flatpak_checks  # noqa: E402
import metainfo_releases  # noqa: E402

TAG = "KytyPS5-GTK-2026-11-12-abcdef0"


def release(tag, published_at, draft=False, prerelease=False):
    return {"tag_name": tag, "published_at": published_at, "draft": draft,
            "prerelease": prerelease, "assets": [{"name": f"{tag}-x86_64.flatpak"}]}


class Run(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.metainfo = os.path.join(self.temp.name, "app.metainfo.xml")
        shutil.copyfile(METAINFO, self.metainfo)
        with open(METAINFO, encoding="utf-8") as handle:
            self.original = handle.read()
        self.releases = os.path.join(self.temp.name, "releases.json")
        self.set_releases("")

    def tearDown(self):
        self.temp.cleanup()

    def set_releases(self, text):
        with open(self.releases, "w", encoding="utf-8") as handle:
            handle.write(text)

    def pages(self, *pages):
        self.set_releases("".join(json.dumps(page, indent=1) for page in pages))

    def run_main(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = metainfo_releases.main(list(arguments))
        return status, stdout.getvalue().splitlines(), stderr.getvalue()

    def write(self, tag=TAG, date="2026-11-12", development=False):
        extra = ["--type", "development"] if development else []
        return self.run_main("write", "--metainfo", self.metainfo, "--releases", self.releases,
                             "--tag", tag, "--date", date, *extra)

    def entries(self):
        root = ET.parse(self.metainfo).getroot()
        return [(entry.get("version"), entry.get("date"))
                for entry in root.find("releases").findall("release")]

    def types(self):
        root = ET.parse(self.metainfo).getroot()
        return [entry.get("type") for entry in root.find("releases").findall("release")]

    def text(self):
        with open(self.metainfo, encoding="utf-8") as handle:
            return handle.read()


class Write(Run):
    def test_the_committed_metainfo_keeps_an_empty_releases_element(self):
        releases = ET.parse(METAINFO).getroot().find("releases")
        self.assertIsNotNone(releases)
        self.assertEqual(len(releases), 0)

    def test_first_release_gets_one_entry(self):
        status, lines, _ = self.write()
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.entries(), [(TAG, "2026-11-12")])
        self.assertEqual(self.text(), self.original.replace(
            "  <releases/>",
            f'  <releases>\n    <release version="{TAG}" date="2026-11-12"/>\n  </releases>'))

    def test_published_releases_are_listed_newest_first_below_the_tag(self):
        self.pages([release("KytyPS5-GTK-2026-10-01-1111111", "2026-10-01T08:00:00Z"),
                    release("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01T20:15:00Z")],
                   [release("KytyPS5-GTK-2026-10-20-2222222", "2026-10-21T00:30:00Z"),
                    release("draft-one", None, draft=True),
                    release("pre-one", "2026-11-05T10:00:00Z", prerelease=True)])
        status, lines, _ = self.write()
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.entries(), [
            (TAG, "2026-11-12"),
            ("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01"),
            ("KytyPS5-GTK-2026-10-20-2222222", "2026-10-21"),
            ("KytyPS5-GTK-2026-10-01-1111111", "2026-10-01")])
        root = ET.parse(self.metainfo).getroot()
        count, problems = flatpak_checks.releases_problems(root)
        self.assertEqual((count, problems), (4, []))

    def test_jq_lines_are_read_too(self):
        lines = [json.dumps({key: value for key, value in release(
            "KytyPS5-GTK-2026-11-01-3333333", "2026-11-01T20:15:00Z").items() if key != "assets"})]
        self.set_releases("\n".join(lines) + "\n")
        status, _, _ = self.write()
        self.assertEqual(status, 0)
        self.assertEqual(len(self.entries()), 2)

    def test_only_the_releases_element_changes(self):
        self.pages([release("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01T20:15:00Z")])
        self.write()
        before, after = self.original.split("<releases/>")
        self.assertTrue(self.text().startswith(before))
        self.assertTrue(self.text().endswith(after))

    def test_a_rerun_of_a_published_release_keeps_its_entry(self):
        self.pages([release(TAG, "2026-11-12T09:00:00.123Z"),
                    release("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01T20:15:00Z")])
        status, _, _ = self.write()
        self.assertEqual(status, 0)
        self.assertEqual(self.entries(), [(TAG, "2026-11-12"),
                                          ("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01")])

    def test_a_published_tag_with_another_date_is_rejected(self):
        self.pages([release(TAG, "2026-11-11T23:59:00Z")])
        status, lines, _ = self.write()
        self.assertEqual(status, 1)
        self.assertEqual(lines, [f"error: release {TAG} is already published with date "
                                 "2026-11-11, not 2026-11-12"])
        self.assertEqual(self.text(), self.original)

    def test_a_date_older_than_the_newest_release_is_rejected(self):
        self.pages([release("KytyPS5-GTK-2026-11-13-4444444", "2026-11-13T01:00:00Z")])
        status, lines, _ = self.write()
        self.assertEqual(status, 1)
        self.assertIn("is older than the newest published release", lines[0])
        self.assertEqual(self.text(), self.original)

    def test_a_tag_published_twice_is_rejected(self):
        self.pages([release("x", "2026-11-01T00:00:00Z"), release("x", "2026-11-02T00:00:00Z")])
        status, lines, _ = self.write()
        self.assertEqual(status, 1)
        self.assertEqual(lines, ["error: published more than once: x"])

    def test_a_metainfo_with_entries_is_rejected(self):
        self.write()
        status, lines, _ = self.write()
        self.assertEqual(status, 1)
        self.assertIn("already has release entries", lines[0])

    def test_special_characters_in_a_tag_are_escaped(self):
        self.pages([release('a"b<c>&d', "2026-11-01T00:00:00Z")])
        status, _, _ = self.write()
        self.assertEqual(status, 0)
        self.assertEqual(self.entries()[1], ('a"b<c>&d', "2026-11-01"))

    def test_bad_inputs_are_input_errors(self):
        cases = (
            ("not json", None, "is not JSON"),
            (json.dumps([{"published_at": "2026-11-01T00:00:00Z", "draft": False,
                          "prerelease": False}]), None, "has no tag_name"),
            (json.dumps([{"tag_name": "x", "published_at": "2026-11-01T00:00:00Z"}]), None,
             "no boolean draft and prerelease"),
            (json.dumps([release("x", "2026-11-01")]), None, "not a UTC RFC 3339 time"),
            (json.dumps([release("x", "2026-13-01T00:00:00Z")]), None, "is not a valid date"),
            (json.dumps(["x"]), None, "is not a JSON object"),
            ("", "2026-11-1", "is not a YYYY-MM-DD date"),
        )
        for text, date, message in cases:
            with self.subTest(text=text, date=date):
                self.set_releases(text)
                status, _, stderr = self.write(date=date or "2026-11-12")
                self.assertEqual(status, 2)
                self.assertIn(message, stderr)
                self.assertEqual(self.text(), self.original)

    def test_a_malformed_metainfo_is_an_input_error(self):
        with open(self.metainfo, "w", encoding="utf-8") as handle:
            handle.write("<component><releases/>")
        status, _, stderr = self.write()
        self.assertEqual(status, 2)
        self.assertIn("is not well-formed XML", stderr)


class Development(Run):
    """--type development: every run that is not official."""

    def test_first_development_entry(self):
        status, lines, _ = self.write(development=True)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.text(), self.original.replace(
            "  <releases/>",
            f'  <releases>\n    <release version="{TAG}" date="2026-11-12" '
            'type="development"/>\n  </releases>'))
        self.assertEqual(lines[1:], [f"  {TAG} (2026-11-12, development)"])

    def test_published_releases_stay_stable_below_the_development_entry(self):
        self.pages([release("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01T20:15:00Z"),
                    release("KytyPS5-GTK-2026-10-01-1111111", "2026-10-01T08:00:00Z")])
        status, _, _ = self.write(development=True)
        self.assertEqual(status, 0)
        self.assertEqual(self.entries(), [(TAG, "2026-11-12"),
                                          ("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01"),
                                          ("KytyPS5-GTK-2026-10-01-1111111", "2026-10-01")])
        self.assertEqual(self.types(), ["development", None, None])
        root = ET.parse(self.metainfo).getroot()
        self.assertEqual(flatpak_checks.releases_problems(root), (3, []))

    def test_a_released_commit_gets_no_development_entry(self):
        # A run on dev or a manual run of a commit that main already published, a day later.
        self.pages([release(TAG, "2026-11-12T09:00:00Z"),
                    release("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01T20:15:00Z")])
        status, _, _ = self.write(date="2026-11-13", development=True)
        self.assertEqual(status, 0)
        self.assertEqual(self.entries(), [(TAG, "2026-11-12"),
                                          ("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01")])
        self.assertEqual(self.types(), [None, None])

    def test_a_date_older_than_the_newest_release_is_rejected(self):
        self.pages([release("KytyPS5-GTK-2026-11-13-4444444", "2026-11-13T01:00:00Z")])
        status, lines, _ = self.write(development=True)
        self.assertEqual(status, 1)
        self.assertIn("is older than the newest published release", lines[0])
        self.assertEqual(self.text(), self.original)

    def test_another_type_is_a_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                metainfo_releases.main(["write", "--metainfo", self.metainfo, "--releases",
                                        self.releases, "--tag", TAG, "--date", "2026-11-12",
                                        "--type", "snapshot"])
        self.assertEqual(raised.exception.code, 2)


class Check(Run):
    def check(self, tag=TAG):
        return self.run_main("check", "--metainfo", self.metainfo, "--tag", tag)

    def test_the_written_entry_passes(self):
        self.pages([release("KytyPS5-GTK-2026-11-01-3333333", "2026-11-01T20:15:00Z")])
        self.write()
        status, lines, _ = self.check()
        self.assertEqual(status, 0)
        self.assertEqual(lines, [f"{self.metainfo}: the newest of 2 release entries is {TAG} "
                                 "(2026-11-12)"])

    def test_another_newest_entry_fails(self):
        self.write(tag="KytyPS5-GTK-2026-11-12-0000000")
        status, lines, _ = self.check()
        self.assertEqual(status, 1)
        self.assertIn("is 'KytyPS5-GTK-2026-11-12-0000000', not", lines[0])

    def test_a_development_entry_is_not_publishable(self):
        self.write(development=True)
        status, lines, _ = self.check()
        self.assertEqual(status, 1)
        self.assertIn("has type 'development'; a published release must be stable", lines[0])

    def test_the_committed_metainfo_fails(self):
        status, lines, _ = self.check()
        self.assertEqual(status, 1)
        self.assertIn("has no <release> entry", lines[0])

    def test_an_entry_without_a_valid_date_fails(self):
        with open(self.metainfo, "w", encoding="utf-8") as handle:
            entry = f'<release version="{TAG}" date="12.11.2026"/>'
            handle.write(self.original.replace("<releases/>", f"<releases>{entry}</releases>"))
        status, lines, _ = self.check()
        self.assertEqual(status, 1)
        self.assertIn("is not a YYYY-MM-DD date", lines[0])

    def test_script_exit_status(self):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, "-B", SCRIPT, "check", "--metainfo",
                                 self.metainfo, "--tag", TAG], capture_output=True, text=True,
                                env=environment, stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 1)
        result = subprocess.run([sys.executable, "-B", SCRIPT, "write", "--metainfo",
                                 self.metainfo], capture_output=True, text=True, env=environment,
                                stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--releases", result.stderr)


if __name__ == "__main__":
    unittest.main()
