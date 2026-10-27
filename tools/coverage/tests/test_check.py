"""Unit tests for tools/coverage/check.py (task 7.5, Requirements 18.4 to 18.11)."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from decimal import Decimal
from unittest import mock

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Keep the local imports below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS_DIR)

import check  # noqa: E402
import report  # noqa: E402

GROUPS_JSON = os.path.join(TOOLS_DIR, "groups.json")


def git_environment(home):
    """Return an environment that isolates git from the user's configuration."""
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("GIT_", "GITHUB_"))}
    env.update({"HOME": home, "GIT_CONFIG_NOSYSTEM": "1", "XDG_CONFIG_HOME": home})
    return env


def metric(covered, total, percent=None):
    value = report.percent(covered, total) if percent is None else Decimal(percent)
    return {"covered": covered, "total": total, "percent": value}


def entry(lines, functions=(1, 1), branches=(0, 0)):
    return {"lines": metric(*lines), "functions": metric(*functions),
            "branches": metric(*branches)}


def passing_summary(scope_percent="80.00"):
    """Return a summary that passes every rule against baseline 80.00."""
    scope = entry((8000, 10000))
    scope["lines"]["percent"] = Decimal(scope_percent)
    return {
        "scope": scope,
        "directories": {name: entry((1, 10)) for name in check.REQUIRED_NONZERO_DIRECTORIES},
        "files": {"src/common/a.cpp": entry((1, 10))},
        "groups": {"launcher_core": entry((90, 100), (95, 100)),
                   "updater": entry((90, 100)),
                   "gtk_presentation": entry((60, 100))},
    }


class CheckRun(unittest.TestCase):
    """Base class: temporary files and a check.main runner."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        self.env = mock.patch.dict(os.environ, git_environment(self.root), clear=True)
        self.env.start()
        self.summary = os.path.join(self.root, "summary.json")
        self.baseline = os.path.join(self.root, "coverage-baseline.txt")
        self.lcov = os.path.join(self.root, "coverage.lcov")
        self.step_summary = os.path.join(self.root, "step-summary.md")
        self.write_summary(passing_summary())
        self.write(self.baseline, "80.00\n")
        self.write(self.lcov, "")

    def tearDown(self):
        self.env.stop()
        self.temp.cleanup()

    @staticmethod
    def write(path, text):
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)

    def write_summary(self, summary):
        self.write(self.summary, report.dump_json(summary) + "\n")

    def run_check(self, *extra, source_dir=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        arguments = ["--summary", self.summary, "--lcov", self.lcov, "--baseline", self.baseline,
                     "--groups", GROUPS_JSON, "--source-dir", source_dir or self.root,
                     "--step-summary", self.step_summary, *extra]
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = check.main(arguments)
        return status, stdout.getvalue(), stderr.getvalue()

    def step_summary_text(self):
        with open(self.step_summary, encoding="utf-8") as handle:
            return handle.read()


class SummaryAndBaselineTests(CheckRun):
    def test_all_rules_pass(self):
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 0, stdout)
        self.assertIn("scope line coverage: 80.00% (8000/10000)", stdout)
        self.assertIn("coverage check passed", stdout)
        self.assertNotIn("FAIL", stdout)

    def test_regression_boundary(self):
        for measured, expected in (("79.50", 0), ("79.49", 1), ("80.00", 0), ("0.00", 1)):
            with self.subTest(measured=measured):
                self.write_summary(passing_summary(measured))
                status, stdout, _ = self.run_check()
                self.assertEqual(status, expected, stdout)
                line = f"scope line coverage: measured {measured}%, baseline 80.00%"
                self.assertIn(("FAIL " if expected else "PASS ") + line, stdout)

    def test_missing_summary(self):
        os.remove(self.summary)
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn(f"FAIL coverage summary {self.summary} is missing", stdout)
        self.assertIn("is missing", self.step_summary_text())

    def test_invalid_summary(self):
        for text in ("{not json", "[]", '{"scope": {}, "directories": {}, "files": {}, "groups": {}}',
                     '{"scope": {"lines": {"covered": "1", "total": 2, "percent": 50.00}},'
                     ' "directories": {}, "files": {}, "groups": {}}'):
            with self.subTest(text=text):
                self.write(self.summary, text)
                status, stdout, _ = self.run_check()
                self.assertEqual(status, 1)
                self.assertIn("is invalid", stdout)

    def test_valid_baselines(self):
        for text in ("80.00\n", "80.00", "0.00\n", "100.00\n", "080.00\n", "\n80.00\n\n"):
            with self.subTest(text=text):
                self.assertEqual(check.read_baseline(self.write_baseline(text))[1], None)
        self.assertEqual(check.read_baseline(self.write_baseline("75.25\n"))[0], Decimal("75.25"))

    def write_baseline(self, text):
        self.write(self.baseline, text)
        return self.baseline

    def test_malformed_baselines(self):
        for text in ("80", "80.0", "100.01", "abc", "80.00\n81.00\n", "", " 80.00", "80.00 ",
                     "1000.00", "-1.00", "8O.00", "\u0668\u0660.00"):
            with self.subTest(text=text):
                self.write_baseline(text)
                status, stdout, _ = self.run_check()
                self.assertEqual(status, 1, stdout)
                self.assertIn(f"FAIL baseline {self.baseline} is malformed", stdout)
                self.assertIn("measured scope line coverage 80.00%", stdout)

    def test_missing_baseline(self):
        os.remove(self.baseline)
        self.write_summary(passing_summary("12.34"))
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn(f"FAIL baseline {self.baseline} is missing; measured scope line coverage "
                      "12.34%", stdout)
        self.assertIn("12.34%", self.step_summary_text())


class TargetTests(CheckRun):
    def test_group_threshold(self):
        summary = passing_summary()
        summary["groups"]["launcher_core"]["lines"] = metric(8999, 10000)
        summary["groups"]["updater"]["lines"] = metric(9, 10)
        self.write_summary(summary)
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("FAIL group launcher_core lines: measured 89.99% (8999/10000), "
                      "threshold 90%", stdout)
        self.assertIn("PASS group launcher_core functions: measured 95.00%", stdout)
        self.assertIn("PASS group updater lines: measured 90.00%", stdout)
        self.assertIn("coverage check failed: 1 failures", stdout)

    def test_group_function_threshold(self):
        summary = passing_summary()
        summary["groups"]["launcher_core"]["functions"] = metric(94, 100)
        self.write_summary(summary)
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("FAIL group launcher_core functions: measured 94.00% (94/100), "
                      "threshold 95%", stdout)

    def test_group_missing_from_summary(self):
        summary = passing_summary()
        del summary["groups"]["gtk_presentation"]
        self.write_summary(summary)
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("FAIL group gtk_presentation lines: missing from the summary", stdout)

    def test_zero_coverage_directories(self):
        summary = passing_summary()
        summary["directories"]["src/kernel"] = entry((0, 500))
        del summary["directories"]["src/updater"]
        self.write_summary(summary)
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("FAIL directory src/kernel lines: measured 0.00% (0/500), must be above 0",
                      stdout)
        self.assertIn("FAIL directory src/updater lines: measured 0.00% (0/0)", stdout)
        self.assertIn("PASS directory src/common lines", stdout)
        self.assertIn("2 failures", stdout)

    def test_directory_list_matches_requirement_18_4(self):
        self.assertEqual(check.REQUIRED_NONZERO_DIRECTORIES, (
            "src/common", "src/kernel", "src/libs", "src/loader", "src/graphics/guest_gpu",
            "src/graphics/host_gpu", "src/graphics/shader", "src/graphics/presentation",
            "src/launcher", "src/updater"))

    def test_tiny_nonzero_directory_passes(self):
        summary = passing_summary()
        summary["directories"]["src/libs"] = entry((1, 300000))
        self.write_summary(summary)
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 0, stdout)
        self.assertIn("PASS directory src/libs lines: measured 0.00% (1/300000)", stdout)


class NextBaselineTests(CheckRun):
    def test_push_to_main_above_baseline(self):
        self.write_summary(passing_summary("81.23"))
        status, stdout, _ = self.run_check("--event-name", "push", "--ref", "refs/heads/main")
        self.assertEqual(status, 0)
        self.assertIn("next baseline: 81.23", stdout)
        self.assertIn("next baseline: 81.23", self.step_summary_text())

    def test_no_next_baseline(self):
        for summary_value, event, ref in (("80.00", "push", "refs/heads/main"),
                                          ("81.23", "push", "refs/heads/dev"),
                                          ("81.23", "workflow_dispatch", "refs/heads/main"),
                                          ("79.80", "push", "refs/heads/main")):
            with self.subTest(value=summary_value, event=event, ref=ref):
                self.write_summary(passing_summary(summary_value))
                status, stdout, _ = self.run_check("--event-name", event, "--ref", ref)
                self.assertEqual(status, 0)
                self.assertNotIn("next baseline", stdout)

    def test_environment_defaults(self):
        self.write_summary(passing_summary("81.23"))
        os.environ.update({"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/main",
                           "GITHUB_STEP_SUMMARY": self.step_summary})
        args = check.parse_args(["--summary", self.summary, "--lcov", self.lcov,
                                 "--baseline", self.baseline, "--groups", GROUPS_JSON,
                                 "--source-dir", self.root])
        self.assertEqual((args.event_name, args.ref, args.base_ref, args.step_summary),
                         ("push", "refs/heads/main", "", self.step_summary))
        os.environ.update({"GITHUB_EVENT_NAME": "pull_request", "GITHUB_BASE_REF": "dev"})
        args = check.parse_args(["--summary", self.summary, "--lcov", self.lcov,
                                 "--baseline", self.baseline, "--groups", GROUPS_JSON,
                                 "--source-dir", self.root])
        self.assertEqual(args.base_ref, "origin/dev")


class StepSummaryTests(CheckRun):
    def test_appends_results(self):
        self.write(self.step_summary, "# earlier step\n")
        summary = passing_summary("70.00")
        self.write_summary(summary)
        status, stdout, _ = self.run_check()
        self.assertEqual(status, 1)
        text = self.step_summary_text()
        self.assertTrue(text.startswith("# earlier step\n## Coverage check\n"))
        self.assertIn("- FAIL scope line coverage: measured 70.00%, baseline 80.00%", text)
        self.assertIn("Coverage check failed (1 failures).", text)
        for line in stdout.splitlines():
            if line.startswith(("PASS ", "FAIL ")):
                self.assertIn("- " + line, text)

    def test_no_step_summary_without_setting(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = check.main(["--summary", self.summary, "--lcov", self.lcov,
                                 "--baseline", self.baseline, "--groups", GROUPS_JSON,
                                 "--source-dir", self.root])
        self.assertEqual(status, 0)
        self.assertFalse(os.path.exists(self.step_summary))

    def test_unwritable_step_summary_fails(self):
        # Injected failure instead of permissions: CI containers run as root.
        with mock.patch.object(check, "write_step_summary", side_effect=OSError("injected")):
            status, _, stderr = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("cannot write step summary", stderr)


class DiffParsingTests(unittest.TestCase):
    def test_new_file(self):
        diff = ("diff --git a/src/new.cpp b/src/new.cpp\nnew file mode 100644\n"
                "index 0000000..1111111\n--- /dev/null\n+++ b/src/new.cpp\n"
                "@@ -0,0 +1,5 @@\n+a\n+b\n+c\n+d\n+e\n")
        self.assertEqual(check.parse_diff(diff), {"src/new.cpp": {1, 2, 3, 4, 5}})

    def test_pure_deletion_and_deleted_file(self):
        diff = ("diff --git a/src/a.cpp b/src/a.cpp\n--- a/src/a.cpp\n+++ b/src/a.cpp\n"
                "@@ -3,2 +2,0 @@\n-x\n-y\n"
                "diff --git a/src/gone.cpp b/src/gone.cpp\ndeleted file mode 100644\n"
                "--- a/src/gone.cpp\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-p\n-q\n")
        self.assertEqual(check.parse_diff(diff), {"src/a.cpp": set()})

    def test_several_hunks(self):
        diff = ("diff --git a/src/a.cpp b/src/a.cpp\n--- a/src/a.cpp\n+++ b/src/a.cpp\n"
                "@@ -1 +1 @@\n-old\n+new\n@@ -10,0 +11,2 @@ int f()\n++++ x\n+--- y\n"
                "@@ -20,3 +22 @@\n-a\n-b\n-c\n+d\n"
                "diff --git a/src/b.h b/src/b.h\n--- a/src/b.h\n+++ b/src/b.h\n@@ -4,0 +5,1 @@\n+z\n")
        self.assertEqual(check.parse_diff(diff), {"src/a.cpp": {1, 11, 12, 22}, "src/b.h": {5}})

    def test_rename(self):
        diff = ("diff --git a/src/old.cpp b/src/renamed.cpp\nsimilarity index 90%\n"
                "rename from src/old.cpp\nrename to src/renamed.cpp\n"
                "--- a/src/old.cpp\n+++ b/src/renamed.cpp\n@@ -2 +2,2 @@\n-a\n+b\n+c\n"
                "diff --git a/src/same.cpp b/src/moved.cpp\nsimilarity index 100%\n"
                "rename from src/same.cpp\nrename to src/moved.cpp\n")
        self.assertEqual(check.parse_diff(diff), {"src/renamed.cpp": {2, 3}})

    def test_quoted_and_spaced_names(self):
        diff = ('diff --git "a/src/t\\303\\244b.cpp" "b/src/t\\303\\244b.cpp"\n'
                '--- "a/src/t\\303\\244b.cpp"\n+++ "b/src/t\\303\\244b.cpp"\n@@ -0,0 +1 @@\n+x\n'
                "diff --git a/src/a b.cpp b/src/a b.cpp\n--- a/src/a b.cpp\t\n+++ b/src/a b.cpp\t\n"
                "@@ -0,0 +3 @@\n+y\n")
        self.assertEqual(check.parse_diff(diff), {"src/t\u00e4b.cpp": {1}, "src/a b.cpp": {3}})


class ChangedLineTests(CheckRun):
    def lcov_text(self, records):
        parts = []
        for path, lines in records.items():
            parts.append(f"SF:{os.path.join(self.root, path)}")
            parts.extend(f"DA:{number},{hits}" for number, hits in lines.items())
            parts.append("end_of_record")
        return "\n".join(parts) + "\n"

    def test_parse_lcov(self):
        text = (f"TN:\nSF:{self.root}/src/a.cpp\nFN:1,f\nDA:1,3\nDA:2,0,abcdef\nDA:2,1\n"
                "BRDA:1,0,0,1\nend_of_record\nSF:/usr/include/x.h\nDA:1,1\nend_of_record\n"
                f"SF:{self.root}/src/b.cpp\nDA:bad\nDA:7,0\nend_of_record\n")
        self.assertEqual(check.parse_lcov(text, self.root),
                         {"src/a.cpp": {1: 3, 2: 1}, "src/b.cpp": {7: 0}})

    def test_counts(self):
        changed = {"src/a.cpp": {1, 2, 3, 9}, "src/b.cpp": {1}, "tests/t.cpp": {1}}
        lcov = {"src/a.cpp": {1: 1, 2: 0, 3: 5}, "src/b.cpp": {1: 1}, "tests/t.cpp": {1: 0}}
        self.assertEqual(check.changed_line_counts(changed, lcov, {"src/a.cpp"}), (2, 3))

    def run_pr(self, diff, records, scope):
        self.write(self.lcov, self.lcov_text(records))
        with mock.patch.object(check, "git_diff", return_value=diff) as git_diff, \
                mock.patch.object(report, "scope_files", return_value=sorted(scope)):
            result = self.run_check("--event-name", "pull_request", "--base-ref", "origin/dev")
        git_diff.assert_called_once_with(self.root, "origin/dev")
        return result

    def new_file_diff(self, path, count):
        return (f"diff --git a/{path} b/{path}\n--- /dev/null\n+++ b/{path}\n"
                f"@@ -0,0 +1,{count} @@\n" + "+x\n" * count)

    def test_eighty_percent_passes(self):
        status, stdout, _ = self.run_pr(self.new_file_diff("src/a.cpp", 5),
                                        {"src/a.cpp": {1: 1, 2: 1, 3: 1, 4: 1, 5: 0}},
                                        ["src/a.cpp"])
        self.assertEqual(status, 0, stdout)
        self.assertIn("PASS changed-line coverage: measured 80.00% (4/5), threshold 80%", stdout)

    def test_seventy_five_percent_fails(self):
        status, stdout, _ = self.run_pr(self.new_file_diff("src/a.cpp", 4),
                                        {"src/a.cpp": {1: 1, 2: 1, 3: 1, 4: 0}}, ["src/a.cpp"])
        self.assertEqual(status, 1)
        self.assertIn("FAIL changed-line coverage: measured 75.00% (3/4), threshold 80%", stdout)
        self.assertIn("75.00%", self.step_summary_text())

    def test_failure_that_rounds_to_threshold(self):
        # 19999/25000 = 79.996 % rounds half up to 80.00 % but fails the exact comparison.
        status, stdout, _ = self.run_pr(self.new_file_diff("src/a.cpp", 25000),
                                        {"src/a.cpp": {number: int(number <= 19999)
                                                       for number in range(1, 25001)}},
                                        ["src/a.cpp"])
        self.assertEqual(status, 1, stdout)
        line = "FAIL changed-line coverage: measured 79.996% (below 80%) (19999/25000), threshold 80%"
        self.assertIn(line, stdout)
        self.assertIn("- " + line, self.step_summary_text())
        changed_lines = [text for text in stdout.splitlines() if "changed-line" in text]
        self.assertEqual(len(changed_lines), 1)
        self.assertNotIn("80.00%", changed_lines[0])

    def test_no_counted_lines(self):
        status, stdout, _ = self.run_pr(self.new_file_diff("src/a.cpp", 3),
                                        {"src/other.cpp": {1: 0}}, ["src/a.cpp", "src/other.cpp"])
        self.assertEqual(status, 0, stdout)
        self.assertIn("PASS changed-line coverage: no changed line counts toward line coverage",
                      stdout)

    def test_out_of_scope_lines_ignored(self):
        status, stdout, _ = self.run_pr(self.new_file_diff("src/gen.cpp", 2),
                                        {"src/gen.cpp": {1: 0, 2: 0}}, ["src/a.cpp"])
        self.assertEqual(status, 0, stdout)
        self.assertIn("no changed line counts", stdout)

    def test_missing_lcov_fails(self):
        os.remove(self.lcov)
        status, stdout, _ = self.run_check("--base-ref", "origin/dev")
        self.assertEqual(status, 1)
        self.assertIn("FAIL changed-line coverage: cannot read lcov file", stdout)

    def test_pull_request_without_base_is_usage_error(self):
        status, _, stderr = self.run_check("--event-name", "pull_request")
        self.assertEqual(status, 2)
        self.assertIn("--base-ref", stderr)

    def test_push_does_not_check_changed_lines(self):
        with mock.patch.object(check, "git_diff") as git_diff:
            status, stdout, _ = self.run_check("--event-name", "push", "--ref", "refs/heads/dev")
        self.assertEqual(status, 0)
        git_diff.assert_not_called()
        self.assertNotIn("changed-line", stdout)


class GitIntegrationTests(CheckRun):
    def git(self, *arguments):
        return subprocess.run(
            ["git", "-C", self.repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
             "-c", "commit.gpgsign=false", *arguments],
            check=True, capture_output=True, text=True, env=os.environ.copy())

    def write_repo(self, path, text):
        full = os.path.join(self.repo, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        self.write(full, text)

    def test_pull_request_in_temporary_repository(self):
        self.repo = os.path.join(self.root, "repo")
        os.makedirs(self.repo)
        self.git("init", "-q")
        self.write_repo("src/a.cpp", "one\ntwo\nthree\n")
        self.write_repo("src/notes.txt", "notes\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.git("branch", "base")
        self.write_repo("src/a.cpp", "one\nTWO\nthree\nfour\nfive\n")
        self.write_repo("src/new.cpp", "n1\nn2\n")
        self.write_repo("src/notes.txt", "notes\nmore\n")
        self.write_repo("tests/t.cpp", "t\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "change")
        lcov = (f"SF:{self.repo}/src/a.cpp\nDA:1,1\nDA:2,1\nDA:3,0\nDA:4,0\nDA:5,1\nend_of_record\n"
                f"SF:{self.repo}/src/new.cpp\nDA:1,1\nDA:2,1\nend_of_record\n"
                f"SF:{self.repo}/tests/t.cpp\nDA:1,0\nend_of_record\n")
        self.write(self.lcov, lcov)
        status, stdout, stderr = self.run_check("--event-name", "pull_request",
                                                "--base-ref", "base", source_dir=self.repo)
        self.assertEqual(status, 0, stdout + stderr)
        self.assertIn("PASS changed-line coverage: measured 80.00% (4/5), threshold 80%", stdout)

        self.write(self.lcov, lcov.replace("DA:5,1", "DA:5,0"))
        status, stdout, _ = self.run_check("--base-ref", "base", source_dir=self.repo)
        self.assertEqual(status, 1)
        self.assertIn("FAIL changed-line coverage: measured 60.00% (3/5), threshold 80%", stdout)

    def test_unknown_base_ref_fails(self):
        self.repo = os.path.join(self.root, "repo")
        os.makedirs(self.repo)
        self.git("init", "-q")
        self.write_repo("src/a.cpp", "one\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        status, stdout, _ = self.run_check("--base-ref", "origin/missing", source_dir=self.repo)
        self.assertEqual(status, 1)
        self.assertIn("FAIL changed-line coverage: git diff origin/missing...HEAD failed", stdout)


class UsageTests(CheckRun):
    def test_missing_argument(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
            check.main(["--summary", self.summary])
        self.assertEqual(raised.exception.code, 2)

    def test_invalid_groups(self):
        groups = os.path.join(self.root, "groups.json")
        self.write(groups, "{}")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            status = check.main(["--summary", self.summary, "--lcov", self.lcov,
                                 "--baseline", self.baseline, "--groups", groups,
                                 "--source-dir", self.root])
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
