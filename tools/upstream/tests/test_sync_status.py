"""Unit tests for tools/upstream/sync_status.py, on scratch repositories.

The forecast tests use a fake strip function (FakeStripper), so they need no unifdef.
"""

import contextlib
import io
import os
import subprocess
import unittest

from scratch_repository import FakeStripper, ScratchRepository

import sync_status  # noqa: E402
from test_platform_merge import BASE, OURS, PATH, THEIRS  # noqa: E402

UPSTREAM_REF = "refs/upstream/heads/main"


class Status(ScratchRepository):
    def main(self, *arguments, stripper=None):
        """sync_status.main on the fork; (status, stdout lines, stderr)."""
        stdout, stderr = io.StringIO(), io.StringIO()
        factory = (lambda scratch: stripper) if stripper else None
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = sync_status.main(["--repo", self.repo, *arguments], stripper_factory=factory)
        return status, stdout.getvalue().splitlines(), stderr.getvalue()

    def upstream(self, base, ours, upstream_commits):
        """A trio whose upstream branch is also refs/upstream/heads/main; its SHA."""
        theirs = self.trio(base, ours, None, upstream_commits)
        self.git("update-ref", UPSTREAM_REF, theirs)
        return theirs

    def short(self, revision, fmt="%h %s (%ad)"):
        return self.git("log", "-1", f"--format={fmt}", "--date=short", revision)

    def test_counts_and_the_merge_base(self):
        theirs = self.upstream({PATH: BASE}, {PATH: OURS},
                               [("one", {"src/a.cpp": "1\n"}), ("two", {"src/b.cpp": "2\n"}),
                                ("three", {PATH: THEIRS})])
        self.commit("fork two", {"src/fork.cpp": "f\n"})
        base = self.git("merge-base", "HEAD", theirs)
        status, lines, stderr = self.main()
        self.assertEqual(status, 0, stderr)
        stat = self.git("diff", "--shortstat", base, theirs)
        self.assertEqual(lines, [
            f"sync_status: upstream head: {self.short(theirs)}",
            f"sync_status: merge base (last synced upstream commit): {self.short(base)}",
            "sync_status: upstream commits not merged: 3",
            "sync_status: fork commits since the merge base: 2",
            f"sync_status: upstream changes since the merge base: {stat}"])

    def test_fetch_by_url_without_tags_or_a_remote(self):
        upstream = self.new_repository("upstream")
        self.commit("base", {"src/a.cpp": "1\n"}, repo=upstream)
        fork = os.path.join(self.temp.name, "clone")
        subprocess.run(["git", "clone", "-q", "--no-tags", f"file://{upstream}", fork],
                       check=True, capture_output=True, stdin=subprocess.DEVNULL)
        self.repo = fork
        head = self.commit("upstream: one more", {"src/b.cpp": "2\n"}, repo=upstream)
        self.git("tag", "v1", repo=upstream)
        self.git("tag", "-a", "-m", "annotated", "v2", repo=upstream)
        config = os.path.join(fork, ".git", "config")
        with open(config, "rb") as handle:
            before = handle.read()
        remotes = self.git("remote")
        status, lines, stderr = self.main("--fetch", "--url", f"file://{upstream}")
        self.assertEqual(status, 0, stderr)
        self.assertEqual(lines[0], f"sync_status: fetched file://{upstream} into "
                         "refs/upstream/heads/ (no tags)")
        self.assertIn("sync_status: upstream commits not merged: 1", lines)
        self.assertEqual(self.git("rev-parse", UPSTREAM_REF), head)
        self.assertEqual(self.git("for-each-ref", "refs/tags"), "")
        with open(config, "rb") as handle:
            self.assertEqual(handle.read(), before)
        self.assertEqual(self.git("remote"), remotes)

    def test_forecast_on_the_trio_changes_nothing(self):
        added = "src/common/added.cpp"
        self.upstream({PATH: BASE, "CMakeLists.txt": "project(x)\n"}, {PATH: OURS},
                      [("upstream", {PATH: THEIRS, "CMakeLists.txt": "project(y)\n",
                                     "src/launcher/src/dialog.cpp": "int d;\n",
                                     added: "int x;\n#if defined(__linux__) || defined(_WIN32)\n"
                                            "#endif\n"})])

        def snapshot():
            return [self.git("status", "--porcelain", "--untracked-files=all"),
                    self.git("ls-files", "--stage"), self.git("count-objects", "-v"),
                    self.git("for-each-ref")]

        before = snapshot()
        stripper = FakeStripper()
        status, lines, stderr = self.main("--forecast", stripper=stripper)
        self.assertEqual(status, 0, stderr)
        forecast = [line for line in lines if not line.startswith("sync_status:")]
        self.assertEqual(forecast, [
            "forecast: a plain merge (git merge-tree) leaves 1 paths in conflict",
            f"  {PATH}",
            "forecast: platform_merge.py strip-merges 2 C/C++ paths, 0 with conflict hunks "
            "(0 hunks)",
            "forecast: removed-platform directives left after stripping: 1 in 1 files",
            f"  {added}:2: #if defined(__linux__) || defined(_WIN32)",
            "forecast: unifdef -t retries: 0",
            "forecast: fork-owned paths changed upstream (fork side kept): 1, 0 in conflict in a "
            "plain merge",
            "  CMakeLists.txt: 1 upstream commits (changed upstream)",
            "forecast: removed-area paths changed upstream (left out): 1",
            "  src/launcher/src/dialog.cpp: added upstream; left out",
            "forecast: fork-deleted paths changed upstream (kept deleted): 0",
            "forecast: C/C++ paths deleted upstream and changed in the fork: 0",
            "forecast: other conflicts (git's merge, renames off): 0"])
        self.assertEqual(sorted(stripper.calls),
                         sorted([(PATH, "base"), (PATH, "upstream"), (added, "upstream")]))
        self.assertEqual(snapshot(), before)

    def test_forecast_with_nothing_to_merge(self):
        self.commit("base", {PATH: BASE})
        self.git("update-ref", UPSTREAM_REF, "HEAD")
        status, lines, _ = self.main("--forecast", stripper=FakeStripper())
        self.assertEqual(status, 0)
        self.assertIn("sync_status: upstream commits not merged: 0", lines)
        self.assertEqual(lines[-1], "forecast: nothing to merge")

    def test_tsv_draft_columns_and_dispositions(self):
        commits = [
            ("kernel: fix a", {"src/common/a.cpp": "int a = 1;\n"}),
            ("launcher: add a dialog", {"src/launcher/src/dialog.cpp": "x\ny\n"}),
            ("emulator: an option and its dialog", {"src/launcher/src/dialog.cpp": "x\n",
                                                    "src/common/a.cpp": "int a = 2;\n"}),
            ("build: add a library", {"CMakeLists.txt": "project(y)\n",
                                      "src/common/b.cpp": "int b;\n"}),
            ("docs: update the readme", {"README.md": "y\n", ".github/workflows/ci.yml": "x\n"}),
        ]
        self.upstream({"README.md": "x\n", "CMakeLists.txt": "project(x)\n",
                       "src/common/a.cpp": "int a;\n"}, {}, commits)
        path = os.path.join(self.temp.name, "draft.tsv")
        status, lines, stderr = self.main("--tsv", path)
        self.assertEqual(status, 0, stderr)
        self.assertEqual(lines[-1], f"sync_status: wrote 5 rows to {path}")
        with open(path, encoding="utf-8") as handle:
            rows = [line.split("\t") for line in handle.read().splitlines()]
        self.assertEqual(rows[0], list(sync_status.TSV_COLUMNS))
        self.assertEqual(rows[0], ["n", "stage", "sha", "date", "author", "subject", "files",
                                   "added", "deleted", "disposition", "areas", "paths"])
        shas = self.git("rev-list", "--reverse", f"main..{UPSTREAM_REF}").splitlines()
        self.assertEqual([row[0] for row in rows[1:]], ["1", "2", "3", "4", "5"])
        self.assertEqual([row[1] for row in rows[1:]], [""] * 5)
        self.assertEqual([row[2] for row in rows[1:]], shas)
        self.assertEqual([row[4] for row in rows[1:]], ["Test"] * 5)
        self.assertEqual([row[5] for row in rows[1:]], [message for message, _ in commits])
        self.assertRegex(rows[1][3], r"^\d{4}-\d{2}-\d{2}$")
        self.assertEqual(rows[1][6:9], ["1", "1", "1"])
        self.assertEqual(rows[3][6:9], ["2", "1", "2"])
        self.assertEqual([row[9].split(":")[0] for row in rows[1:]], ["a", "c", "a+c", "b", "d"])
        self.assertEqual(rows[1][9], "a: emulator core, take as-is")
        self.assertEqual([row[10] for row in rows[1:]],
                         ["src/common", "src/launcher/src", "src/common,src/launcher/src",
                          "(root),src/common", "(root),.github/workflows"])
        self.assertEqual(rows[5][11], ".github/workflows/ci.yml;README.md")
        self.assertEqual(rows[3][11], "src/common/a.cpp;src/launcher/src/dialog.cpp")

    def test_usage_errors(self):
        self.commit("base", {PATH: BASE})
        cases = (((), f"{UPSTREAM_REF} names no commit"),
                 (("--forecast", "--unifdef", os.path.join(self.temp.name, "absent")),
                  "unifdef not found"),
                 (("--url", "file:///elsewhere"), "--url needs --fetch"))
        for arguments, fragment in cases:
            with self.subTest(arguments=arguments):
                stdout, stderr = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    status = sync_status.main(["--repo", self.repo, *arguments])
                self.assertEqual(status, 2)
                self.assertIn(fragment, stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
