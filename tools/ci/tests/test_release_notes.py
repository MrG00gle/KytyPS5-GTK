"""Unit tests for tools/ci/release_notes.py, on temporary git repositories.

Every git call of these tests and of the script runs without the user's and the system's git
configuration (GIT_CONFIG_GLOBAL, GIT_CONFIG_NOSYSTEM) and commits with a fixed test identity.
"""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

CI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(CI_DIR, "release_notes.py")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CI_DIR)

import release_notes  # noqa: E402

REPO = "MrG00gle/KytyPS5-GTK"
URL = f"https://github.com/{REPO}"
TAG = "KytyPS5-GTK-2026-11-20-0000000"
MANUAL = "packaging/release-notes.md"
COMMENT = "<!--\nWrite the notes for the next release below this comment.\n-->\n"
GIT_ENVIRONMENT = {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
                   "PYTHONDONTWRITEBYTECODE": "1"}
IDENTITY = ("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
            "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false")


class GitRepository(unittest.TestCase):
    """A temporary repository with helpers for commits, tags, and the release list."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        patcher = mock.patch.dict(os.environ, GIT_ENVIRONMENT)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.new_repository()

    def new_repository(self):
        """An empty repository, release list, and output path in a new directory of the temp."""
        root = tempfile.mkdtemp(dir=self.temp.name)
        self.repo = os.path.join(root, "repo")
        subprocess.run(["git", "init", "-q", "-b", "main", self.repo], check=True,
                       capture_output=True, stdin=subprocess.DEVNULL)
        self.releases = []
        self.output = os.path.join(root, "notes.md")
        self.releases_path = os.path.join(root, "releases.jsonl")

    def git(self, *arguments):
        result = subprocess.run(["git", *IDENTITY, "-C", self.repo, *arguments], check=True,
                                capture_output=True, text=True, stdin=subprocess.DEVNULL)
        return result.stdout.strip()

    def commit(self, subject, files=None):
        """Commit the files ({path: text, or None to delete}) with the subject; return the SHA."""
        for path, text in (files or {}).items():
            full = os.path.join(self.repo, path)
            if text is None:
                os.remove(full)
                continue
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8", newline="") as handle:
                handle.write(text)
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", subject)
        return self.git("rev-parse", "HEAD")

    def release(self, tag, published_at, at="HEAD", draft=False, prerelease=False, tagged=True):
        """A release in the list; tagged: its tag exists in the repository at the commit at."""
        if tagged:
            self.git("tag", tag, at)
        self.releases.append({"tag_name": tag, "published_at": None if draft else published_at,
                              "draft": draft, "prerelease": prerelease})

    def notes(self, sha=None, extra=(), releases_text=None):
        """Run the script on HEAD (or sha) with the release list (or releases_text); return
        (status, the notes or None, stdout lines)."""
        if releases_text is None:
            releases_text = "".join(json.dumps(entry) + "\n" for entry in self.releases)
        with open(self.releases_path, "w", encoding="utf-8") as handle:
            handle.write(releases_text)
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = release_notes.main(["--repository", REPO, "--tag", TAG,
                                         "--sha", sha or self.git("rev-parse", "HEAD"),
                                         "--releases", self.releases_path, "--manual", MANUAL,
                                         "--output", self.output, "--git-dir", self.repo,
                                         *extra])
        text = None
        if os.path.exists(self.output):
            with open(self.output, encoding="utf-8") as handle:
                text = handle.read()
        return status, text, stdout.getvalue().splitlines()

    def item(self, text, sha):
        return f"- {text} ([{sha[:7]}]({URL}/commit/{sha}))"


class ManualNotes(GitRepository):
    """Notes written by hand are used when their text changed since the previous release."""

    def first_release(self, manual=None):
        files = {"README.md": "x\n"}
        if manual is not None:
            files[MANUAL] = manual
        self.commit("launcher: first", files)
        self.release("v1", "2026-11-18T12:00:00Z")

    def test_changed_since_the_previous_release_is_used_without_comments(self):
        self.first_release(COMMENT + "Notes of v1\n")
        self.commit("launcher: faster scans", {MANUAL: COMMENT + "## Launcher\n\n- Faster "
                    "scans\n<!-- not shown -->\n\nThanks for the reports.\n\n"})
        status, text, lines = self.notes()
        self.assertEqual(status, 0, lines)
        self.assertEqual(text, "## Launcher\n\n- Faster scans\n\n\nThanks for the reports.\n")
        self.assertIn(f"release_notes: the notes written by hand in {MANUAL}, because its text "
                      "changed since v1", lines)

    def test_old_notes_kept_next_to_new_ones_are_published_with_them(self):
        # The whole text outside comments is the release's text, not only what changed, which is
        # why the docs say to replace the notes of an earlier release.
        self.first_release(COMMENT + "Notes of v1\n")
        self.commit("launcher: faster scans",
                    {MANUAL: COMMENT + "Notes of v1\n\nNotes of v2\n"})
        status, text, _ = self.notes()
        self.assertEqual((status, text), (0, "Notes of v1\n\nNotes of v2\n"))

    def test_unchanged_since_the_previous_release_is_not_used_again(self):
        self.first_release(COMMENT + "Notes of v1\n")
        sha = self.commit("launcher: faster scans", {"README.md": "y\n"})
        status, text, lines = self.notes()
        self.assertEqual(status, 0, lines)
        self.assertEqual(text, f"## Launcher\n\n{self.item('Faster scans', sha)}\n\n"
                         f"**Full Changelog**: {URL}/compare/v1...{TAG}\n")
        self.assertIn(f"release_notes: the subjects of 1 of 1 commits since v1, because {MANUAL} "
                      "has the same text as at v1", lines)

    def test_an_edit_of_the_comment_alone_is_not_new_notes(self):
        self.first_release(COMMENT + "Notes of v1\n")
        self.commit("docs: reword the notes comment",
                    {MANUAL: "<!-- Other words. -->\n\nNotes of v1\r\n<!-- More. -->"})
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        self.assertTrue(text.startswith("## Other changes\n"), text)

    def test_missing_at_the_previous_release_and_written_now(self):
        self.first_release()
        self.commit("packaging: notes", {MANUAL: COMMENT + "First notes\n"})
        status, text, lines = self.notes()
        self.assertEqual((status, text), (0, "First notes\n"), lines)

    def test_no_text_outside_comments_is_not_notes(self):
        for manual in (COMMENT, "", "  \n\n", "<!-- unclosed\nNot shown on GitHub either\n"):
            with self.subTest(manual=manual):
                self.new_repository()
                sha = self.commit("updater: check the size", {MANUAL: manual})
                status, text, lines = self.notes()
                self.assertEqual(status, 0, lines)
                self.assertEqual(text, f"## Updater\n\n{self.item('Check the size', sha)}\n\n"
                                 f"**Full Changelog**: {URL}/commits/{TAG}\n")
                self.assertIn(f"release_notes: the subjects of 1 of 1 commits up to {sha[:7]}, "
                              f"because {MANUAL} has no text outside comments", lines)

    def test_notes_without_a_previous_release(self):
        self.commit("launcher: first", {MANUAL: COMMENT + "The first release.\n"})
        status, text, lines = self.notes()
        self.assertEqual((status, text), (0, "The first release.\n"), lines)
        self.assertIn(f"release_notes: the notes written by hand in {MANUAL}, because there is no "
                      "previous release", lines)

    def test_the_file_removed_since_the_previous_release(self):
        self.first_release(COMMENT + "Notes of v1\n")
        self.commit("packaging: drop the notes file", {MANUAL: None})
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        self.assertTrue(text.startswith("## Packaging\n\n- Drop the notes file ("), text)

    def test_the_notes_of_the_commit_not_of_the_work_tree(self):
        self.first_release(COMMENT)
        self.commit("launcher: x")
        with open(os.path.join(self.repo, MANUAL), "w", encoding="utf-8") as handle:
            handle.write("Uncommitted notes\n")
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        self.assertTrue(text.startswith("## Launcher\n"), text)


class GeneratedNotes(GitRepository):
    """Without new notes written by hand, the commit subjects since the previous release."""

    def test_groups_their_order_and_the_other_changes(self):
        self.commit("launcher: first")
        self.release("v1", "2026-11-18T12:00:00Z")
        subjects = ["docs: describe the release notes", "emulator: stop on a missing game",
                    "shader: support D16 buffer loads", "updater: retry the download",
                    "launcher: show sizes in IEC units", "packaging: name the notes file",
                    "tests: cover the notes", "Fix the build without an area",
                    "ci: write the release notes", "launcher: keep the offer open",
                    "test: an older singular area", "Launcher: an area in capitals"]
        sha = {subject: self.commit(subject) for subject in subjects}
        status, text, lines = self.notes()
        self.assertEqual(status, 0, lines)
        expected = "\n".join([
            "## Launcher", "",
            self.item("Show sizes in IEC units", sha["launcher: show sizes in IEC units"]),
            self.item("Keep the offer open", sha["launcher: keep the offer open"]), "",
            "## Updater", "",
            self.item("Retry the download", sha["updater: retry the download"]), "",
            "## Emulator", "",
            self.item("Stop on a missing game", sha["emulator: stop on a missing game"]),
            self.item("Shader: support D16 buffer loads", sha["shader: support D16 buffer loads"]),
            "",
            "## Packaging", "",
            self.item("Name the notes file", sha["packaging: name the notes file"]), "",
            "<details>", "<summary>Other changes (6)</summary>", "",
            self.item("docs: describe the release notes", sha["docs: describe the release notes"]),
            self.item("tests: cover the notes", sha["tests: cover the notes"]),
            self.item("Fix the build without an area", sha["Fix the build without an area"]),
            self.item("ci: write the release notes", sha["ci: write the release notes"]),
            self.item("test: an older singular area", sha["test: an older singular area"]),
            self.item("Launcher: an area in capitals", sha["Launcher: an area in capitals"]), "",
            "</details>", "",
            f"**Full Changelog**: {URL}/compare/v1...{TAG}", ""])
        self.assertEqual(text, expected)
        self.assertIn("release_notes: the previous release is v1 "
                      f"({self.git('rev-parse', 'v1')[:7]})", lines)
        self.assertEqual(lines[-len(expected.splitlines()):], expected.splitlines())

    def test_only_tooling_changes_give_a_plain_list(self):
        self.commit("launcher: first")
        self.release("v1", "2026-11-18T12:00:00Z")
        first = self.commit("ci: pin the actions")
        second = self.commit("Update the README")
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        self.assertEqual(text, f"## Other changes\n\n{self.item('ci: pin the actions', first)}\n"
                         f"{self.item('Update the README', second)}\n\n"
                         f"**Full Changelog**: {URL}/compare/v1...{TAG}\n")

    def test_html_in_a_subject_is_shown_as_text(self):
        sha = self.commit("launcher: keep <releases/> & the rest")
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        self.assertIn(self.item("Keep &lt;releases/&gt; &amp; the rest", sha), text)

    def test_merge_commits_are_left_out(self):
        self.commit("launcher: first")
        self.release("v1", "2026-11-18T12:00:00Z")
        self.git("checkout", "-q", "-b", "side")
        side = self.commit("updater: on the side")
        self.git("checkout", "-q", "main")
        main = self.commit("launcher: on main")
        self.git("merge", "-q", "--no-ff", "-m", "Merge branch 'side'", "side")
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        self.assertEqual(text, f"## Launcher\n\n{self.item('On main', main)}\n\n## Updater\n\n"
                         f"{self.item('On the side', side)}\n\n"
                         f"**Full Changelog**: {URL}/compare/v1...{TAG}\n")

    def test_at_most_the_newest_100_subjects(self):
        self.commit("launcher: first")
        self.release("v1", "2026-11-18T12:00:00Z")
        shas = [self.commit(f"launcher: change {number}") for number in range(1, 102)]
        status, text, lines = self.notes()
        self.assertEqual(status, 0, lines)
        items = [line for line in text.splitlines() if line.startswith("- ")]
        self.assertEqual(len(items), 100)
        self.assertEqual(items[0], self.item("Change 2", shas[1]))
        self.assertEqual(items[-1], self.item("Change 101", shas[-1]))
        self.assertIn("\n\u2026 and 1 earlier commit; see the full changelog.\n\n**Full "
                      f"Changelog**: {URL}/compare/v1...{TAG}\n", text)
        self.assertIn("release_notes: the subjects of 100 of 101 commits since v1, because "
                      f"{MANUAL} has no text outside comments", lines)

    def test_without_a_previous_release_the_newest_100_up_to_the_commit(self):
        shas = [self.commit(f"kernel: step {number}") for number in range(1, 103)]
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        items = [line for line in text.splitlines() if line.startswith("- ")]
        self.assertEqual(items[0], self.item("Kernel: step 3", shas[2]))
        self.assertEqual(len(items), 100)
        self.assertTrue(text.endswith("\u2026 and 2 earlier commits; see the full changelog.\n\n"
                                      f"**Full Changelog**: {URL}/commits/{TAG}\n"), text)

    def test_no_commits_since_the_previous_release(self):
        sha = self.commit("launcher: first")
        self.release("v1", "2026-11-18T12:00:00Z")
        status, text, _ = self.notes(sha=sha)
        self.assertEqual((status, text), (0, f"No changes since v1.\n\n**Full Changelog**: "
                                             f"{URL}/compare/v1...{TAG}\n"))


class PreviousRelease(GitRepository):
    """The newest published release before the commit is the previous release."""

    def test_the_newest_published_release_wins(self):
        self.commit("launcher: one")
        self.release("v1", "2026-11-01T08:00:00Z")
        self.commit("launcher: two")
        self.release("v2", "2026-11-10T08:00:00Z")
        sha = self.commit("launcher: three")
        # The list order does not matter; published_at decides.
        self.releases.reverse()
        status, text, _ = self.notes()
        self.assertEqual(status, 0)
        self.assertEqual(text, f"## Launcher\n\n{self.item('Three', sha)}\n\n"
                         f"**Full Changelog**: {URL}/compare/v2...{TAG}\n")

    def test_releases_that_are_never_the_previous_release(self):
        self.commit("launcher: one")
        self.release("v1", "2026-11-01T08:00:00Z")
        self.git("checkout", "-q", "-b", "side")
        self.commit("launcher: on a side branch")
        self.release("side-release", "2026-11-12T08:00:00Z")
        self.git("checkout", "-q", "main")
        self.commit("launcher: two")
        self.release("a-draft", None, draft=True)
        self.release("a-prerelease", "2026-11-13T08:00:00Z", prerelease=True)
        self.release("not-in-the-clone", "2026-11-14T08:00:00Z", tagged=False)
        sha = self.commit("launcher: three")
        # TAG itself is published already (a rerun of the release job).
        self.release(TAG, "2026-11-15T08:00:00Z")
        status, text, lines = self.notes()
        self.assertEqual(status, 0, lines)
        self.assertTrue(text.endswith(f"{self.item('Three', sha)}\n\n**Full Changelog**: "
                                      f"{URL}/compare/v1...{TAG}\n"), text)
        side = self.git("rev-parse", "side-release")
        self.assertEqual(lines[:3], [
            "release_notes: release not-in-the-clone has no tag in the checkout; not the "
            "previous release",
            f"release_notes: release side-release ({side[:7]}) is not an ancestor of {sha[:7]}; "
            "not the previous release",
            f"release_notes: the previous release is v1 ({self.git('rev-parse', 'v1')[:7]})"])

    def test_no_usable_previous_release(self):
        self.commit("launcher: one")
        self.release("a-draft", None, draft=True)
        self.release("not-in-the-clone", "2026-11-14T08:00:00Z", tagged=False)
        status, text, lines = self.notes()
        self.assertEqual(status, 0)
        self.assertTrue(text.endswith(f"**Full Changelog**: {URL}/commits/{TAG}\n"), text)
        self.assertIn("release_notes: no previous release is an ancestor of "
                      f"{self.git('rev-parse', 'HEAD')[:7]}", lines)


class Errors(GitRepository):
    def usage_error(self, *arguments):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            try:
                status = release_notes.main(list(arguments))
            except SystemExit as error:
                status = error.code
        return status, stderr.getvalue()

    def test_usage_errors(self):
        sha = self.commit("launcher: one")
        good = {"--repository": REPO, "--tag": TAG, "--sha": sha, "--releases": "r.jsonl",
                "--manual": MANUAL, "--output": self.output}
        cases = (("--repository", "MrG00gle", "is not OWNER/NAME"),
                 ("--tag", "a tag", "is not a release tag"),
                 ("--sha", "HEAD", "is not a full commit SHA"),
                 ("--sha", sha[:7], "is not a full commit SHA"),
                 ("--manual", "/etc/release-notes.md", "is not a relative path"),
                 ("--manual", "packaging/../x.md", "is not a relative path"),
                 ("--tag", None, "--tag"))
        for option, value, fragment in cases:
            with self.subTest(option=option, value=value):
                options = dict(good)
                if value is None:
                    del options[option]
                else:
                    options[option] = value
                arguments = [word for pair in options.items() for word in pair]
                status, stderr = self.usage_error(*arguments)
                self.assertEqual(status, 2)
                self.assertIn(fragment, stderr)
                self.assertFalse(os.path.exists(self.output))

    def assert_fails(self, fragment, sha=None, extra=()):
        status, text, lines = self.notes(sha=sha, extra=extra)
        self.assertEqual(status, 1, lines)
        self.assertIsNone(text)
        self.assertTrue(any(line.startswith("error: ") and fragment in line for line in lines),
                        lines)
        self.assertEqual(lines[-1], f"FAILED: release_notes: {self.output} was not written")

    def test_a_commit_that_is_not_in_the_checkout(self):
        self.commit("launcher: one")
        self.assert_fails("is not a commit of the checkout", sha="0" * 40)

    def test_a_directory_that_is_not_a_repository(self):
        sha = self.commit("launcher: one")
        self.assert_fails("is not a commit of the checkout", sha=sha,
                          extra=("--git-dir", self.temp.name))

    def test_an_unreadable_or_malformed_release_list(self):
        self.commit("launcher: one")
        release = {"tag_name": "v1", "published_at": "2026-11-01T08:00:00Z", "draft": False,
                   "prerelease": False}
        cases = (("not json\n", "is not JSON"),
                 ('"a string"\n', "a release is not a JSON object"),
                 ('{"tag_name": "v1", "draft": false}\n', "has no boolean draft and prerelease"),
                 (json.dumps(release) + "\n" + json.dumps(release) + "\n",
                  "published more than once: v1"))
        for text, fragment in cases:
            with self.subTest(text=text):
                status, notes, lines = self.notes(releases_text=text)
                self.assertEqual((status, notes), (1, None), lines)
                self.assertTrue(any(line.startswith("error: ") and fragment in line
                                    for line in lines), lines)
        self.releases_path = os.path.join(self.temp.name, "absent", "releases.jsonl")
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = release_notes.main(["--repository", REPO, "--tag", TAG,
                                         "--sha", self.git("rev-parse", "HEAD"), "--releases",
                                         self.releases_path, "--manual", MANUAL, "--output",
                                         self.output, "--git-dir", self.repo])
        self.assertEqual(status, 1)
        self.assertIn(f"error: cannot read {self.releases_path}", stdout.getvalue())

    def test_notes_that_are_not_a_text_file(self):
        self.commit("launcher: one", {f"{MANUAL}/inside.md": "x\n"})
        self.assert_fails(f"{MANUAL} at ")
        self.new_repository()
        path = os.path.join(self.repo, MANUAL)
        os.makedirs(os.path.dirname(path))
        with open(path, "wb") as handle:
            handle.write(b"Caf\xe9\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "launcher: latin-1 notes")
        self.assert_fails("is not UTF-8 text")

    def test_an_output_that_cannot_be_written(self):
        self.commit("launcher: one")
        self.output = os.path.join(self.temp.name, "missing", "notes.md")
        self.assert_fails("cannot write")


class CommandLine(GitRepository):
    def test_the_script_writes_the_notes(self):
        sha = self.commit("launcher: one")
        with open(self.releases_path, "w", encoding="utf-8") as handle:
            handle.write("\n")
        result = subprocess.run([sys.executable, "-B", SCRIPT, "--repository", REPO, "--tag", TAG,
                                 "--sha", sha, "--releases", self.releases_path, "--manual",
                                 MANUAL, "--output", self.output], cwd=self.repo,
                                capture_output=True, text=True, stdin=subprocess.DEVNULL,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with open(self.output, encoding="utf-8") as handle:
            text = handle.read()
        self.assertEqual(text, f"## Launcher\n\n{self.item('One', sha)}\n\n"
                         f"**Full Changelog**: {URL}/commits/{TAG}\n")
        self.assertIn(f"release_notes: wrote {self.output}:", result.stdout)


if __name__ == "__main__":
    unittest.main()
