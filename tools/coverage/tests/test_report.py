"""Unit tests for tools/coverage/report.py (task 7.5, Requirement 17.3)."""

import contextlib
import html.parser
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from decimal import Decimal
from unittest import mock

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS_DIR)

import report  # noqa: E402

GROUPS_JSON = os.path.join(TOOLS_DIR, "groups.json")


def git_environment(home):
    """Return an environment that isolates git from the user's configuration."""
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update({"HOME": home, "GIT_CONFIG_NOSYSTEM": "1", "XDG_CONFIG_HOME": home})
    return env


def git(repo, *arguments, env):
    subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                    "-c", "commit.gpgsign=false", *arguments],
                   check=True, capture_output=True, env=env)


def file_entry(filename, lines, functions, branches):
    """Return an llvm-cov export file entry; each metric is (covered, count)."""
    def metric(pair):
        covered, count = pair
        return {"count": count, "covered": covered,
                "percent": covered * 100.0 / count if count else 0}
    return {"filename": filename,
            "summary": {"lines": metric(lines), "functions": metric(functions),
                        "branches": metric(branches), "regions": metric((0, 0))}}


SYNTHETIC_SCOPE = {
    "src/main.cpp": "int main() { return 0; }\n",
    "src/common/a.cpp": "// a\n",
    "src/common/sub/b.h": "// b\n",
    "src/common/sub/never.cpp": "// never executed\n",
    "src/launcher/core/text.cpp": "// text\n",
    "src/updater/updater.cpp": "// updater\n",
    "src/launcher/gtk/window.cpp": "// window <&>\n",
}
OUT_OF_SCOPE = {
    "src/CMakeLists.txt": "# cmake\n",
    "src/launcher/forms/main.ui": "<ui/>\n",
    "tests/FooTests.cpp": "// test\n",
    "3rdparty/lib/lib.c": "// third party\n",
}


def make_synthetic_repo(root):
    """Create a git repository and an llvm-cov export under root.

    Returns (repository path, export path). The export also lists files
    outside the scope (3rdparty, _deps, tests, a generated file in the build
    tree, an untracked file, a non-source file) that the report must drop.
    """
    repo = os.path.join(root, "repo")
    env = git_environment(root)
    for path, text in {**SYNTHETIC_SCOPE, **OUT_OF_SCOPE}.items():
        full = os.path.join(repo, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(text)
    subprocess.run(["git", "init", "-q", repo], check=True, capture_output=True, env=env)
    git(repo, "add", "-A", env=env)
    untracked = os.path.join(repo, "src", "common", "untracked.cpp")
    with open(untracked, "w", encoding="utf-8") as handle:
        handle.write("// untracked\n")

    def absolute(path):
        return os.path.join(repo, path)

    files = [
        file_entry(absolute("src/main.cpp"), (1, 3), (1, 1), (0, 0)),
        file_entry(absolute("src/common/a.cpp"), (2, 3), (1, 2), (1, 4)),
        file_entry(absolute("src/common/sub/b.h"), (1, 8), (1, 3), (1, 2)),
        file_entry(absolute("src/launcher/core/text.cpp"), (9, 10), (19, 20), (3, 4)),
        file_entry(absolute("src/updater/updater.cpp"), (1, 200), (0, 1), (0, 0)),
        file_entry(absolute("src/launcher/gtk/window.cpp"), (1, 16000), (1, 3), (2, 3)),
        file_entry(absolute("3rdparty/lib/lib.c"), (5, 5), (1, 1), (0, 0)),
        file_entry(absolute("_Build/release/_deps/zstd-src/lib/zstd.c"), (5, 5), (1, 1), (0, 0)),
        file_entry(absolute("_Build/release/src/generated.cpp"), (5, 5), (1, 1), (0, 0)),
        file_entry(absolute("tests/FooTests.cpp"), (5, 5), (1, 1), (0, 0)),
        file_entry(absolute("src/common/untracked.cpp"), (5, 5), (1, 1), (0, 0)),
        file_entry(absolute("src/CMakeLists.txt"), (5, 5), (1, 1), (0, 0)),
        file_entry("/usr/include/c++/16/vector", (5, 5), (1, 1), (0, 0)),
    ]
    export = {"data": [{"files": files, "totals": {}}],
              "type": "llvm.coverage.json.export", "version": "2.0.1"}
    export_path = os.path.join(root, "export.json")
    with open(export_path, "w", encoding="utf-8") as handle:
        json.dump(export, handle)
    return repo, export_path


class TableParser(html.parser.HTMLParser):
    """Collect each table as {caption: [row cells]}, and the file links."""

    def __init__(self):
        super().__init__()
        self.tables = {}
        self.links = {}
        self._caption = None
        self._rows = None
        self._row = None
        self._cell = None
        self._href = None
        self._in_caption = False

    def handle_starttag(self, tag, attrs):
        if tag == "table":
            self._rows = []
            self._caption = ""
        elif tag == "caption":
            self._in_caption = True
        elif tag == "tr":
            self._row = []
        elif tag in ("td", "th"):
            self._cell = ""
        elif tag == "a":
            self._href = dict(attrs).get("href")

    def handle_endtag(self, tag):
        if tag == "caption":
            self._in_caption = False
        elif tag in ("td", "th") and self._row is not None:
            self._row.append(self._cell)
            if self._href is not None:
                self.links[self._cell] = self._href
                self._href = None
            self._cell = None
        elif tag == "tr":
            self._rows.append(self._row)
            self._row = None
        elif tag == "table":
            self.tables[self._caption] = self._rows

    def handle_data(self, data):
        if self._in_caption:
            self._caption += data
        elif self._cell is not None:
            self._cell += data


def table_values(rows):
    """Map the first cell of each body row to its metric values."""
    result = {}
    for row in rows[1:]:
        result[row[0]] = row[1:]
    return result


def expected_cells(metrics):
    cells = []
    for metric in report.METRICS:
        entry = metrics[metric]
        cells.extend((str(entry["covered"]), str(entry["total"]), f"{entry['percent']:.2f}"))
    return cells


class PercentTests(unittest.TestCase):
    def test_half_up_rounding(self):
        cases = [((1, 3), "33.33"), ((2, 3), "66.67"), ((1, 8), "12.50"),
                 ((1, 200), "0.50"), ((1, 16000), "0.01"), ((1, 1), "100.00"),
                 ((0, 5), "0.00"), ((0, 0), "0.00"), ((5, 0), "0.00")]
        for (covered, total), expected in cases:
            with self.subTest(covered=covered, total=total):
                self.assertEqual(f"{report.percent(covered, total):.2f}", expected)

    def test_half_up_not_half_even(self):
        # 1/8000 = 0.0125 -> 0.01 and 1/1600 = 0.0625 -> 0.06.
        self.assertEqual(report.percent(1, 8000), Decimal("0.01"))
        self.assertEqual(report.percent(1, 1600), Decimal("0.06"))
        # 0.125 rounds half up to 0.13 (half even would give 0.12).
        self.assertEqual(report.percent(1, 800), Decimal("0.13"))


class BuildSummaryTests(unittest.TestCase):
    def setUp(self):
        self.export = {
            "src/a/x.cpp": {"lines": (1, 2), "functions": (1, 1), "branches": (0, 2)},
            "src/a/b/y.cpp": {"lines": (3, 4), "functions": (0, 1), "branches": (1, 2)},
            "src/top.cpp": {"lines": (0, 5), "functions": (0, 2), "branches": (0, 0)},
            "3rdparty/z.c": {"lines": (9, 9), "functions": (1, 1), "branches": (0, 0)},
        }
        self.scope = ["src/a/x.cpp", "src/a/b/y.cpp", "src/top.cpp", "src/a/b/zero.h"]
        self.summary = report.build_summary(self.export, self.scope,
                                            {"a_group": ["src/a/b/"], "empty": ["src/none/"]})

    def counts(self, entry, metric):
        return entry[metric]["covered"], entry[metric]["total"], entry[metric]["percent"]

    def test_schema_keys(self):
        self.assertEqual(list(self.summary), ["scope", "directories", "files", "groups"])
        for entry in [self.summary["scope"], *self.summary["files"].values(),
                      *self.summary["directories"].values(), *self.summary["groups"].values()]:
            self.assertEqual(list(entry), ["lines", "functions", "branches"])
            for metric in entry.values():
                self.assertEqual(list(metric), ["covered", "total", "percent"])

    def test_file_entries(self):
        files = self.summary["files"]
        self.assertEqual(sorted(files), sorted(self.scope))
        self.assertEqual(self.counts(files["src/a/x.cpp"], "lines"), (1, 2, Decimal("50.00")))
        self.assertEqual(self.counts(files["src/a/b/y.cpp"], "branches"), (1, 2, Decimal("50.00")))

    def test_zero_entry_for_missing_scope_file(self):
        zero = self.summary["files"]["src/a/b/zero.h"]
        for metric in report.METRICS:
            self.assertEqual(self.counts(zero, metric), (0, 0, Decimal("0.00")))

    def test_out_of_scope_dropped(self):
        self.assertNotIn("3rdparty/z.c", self.summary["files"])
        self.assertEqual(self.counts(self.summary["scope"], "lines"), (4, 11, Decimal("36.36")))

    def test_recursive_directories(self):
        directories = self.summary["directories"]
        self.assertEqual(list(directories), ["src/a", "src/a/b"])
        self.assertEqual(self.counts(directories["src/a"], "lines"), (4, 6, Decimal("66.67")))
        self.assertEqual(self.counts(directories["src/a/b"], "lines"), (3, 4, Decimal("75.00")))
        self.assertEqual(self.counts(directories["src/a"], "functions"), (1, 2, Decimal("50.00")))
        self.assertNotIn("src", directories)

    def test_scope_totals(self):
        scope = self.summary["scope"]
        self.assertEqual(self.counts(scope, "functions"), (1, 4, Decimal("25.00")))
        self.assertEqual(self.counts(scope, "branches"), (1, 4, Decimal("25.00")))

    def test_groups(self):
        groups = self.summary["groups"]
        self.assertEqual(list(groups), ["a_group", "empty"])
        self.assertEqual(self.counts(groups["a_group"], "lines"), (3, 4, Decimal("75.00")))
        self.assertEqual(self.counts(groups["empty"], "lines"), (0, 0, Decimal("0.00")))

    def test_dump_json_two_decimals(self):
        text = report.dump_json(self.summary)
        self.assertIn('"percent": 0.00', text)
        self.assertIn('"percent": 75.00', text)
        parsed = json.loads(text, parse_float=Decimal)
        self.assertEqual(parsed["scope"]["lines"]["percent"], Decimal("36.36"))


class ExportTests(unittest.TestCase):
    def test_relative_path(self):
        root = os.path.join(os.sep, "work", "repo")
        self.assertEqual(report.relative_path("/work/repo/src/a.cpp", root), "src/a.cpp")
        self.assertEqual(report.relative_path("/work/repo/./src/../src/a.cpp", root), "src/a.cpp")
        self.assertEqual(report.relative_path("src/a.cpp", root), "src/a.cpp")
        self.assertIsNone(report.relative_path("/work/repo-other/src/a.cpp", root))
        self.assertIsNone(report.relative_path("/usr/include/stdio.h", root))

    def test_export_counts(self):
        export = {"data": [{"files": [file_entry("/r/src/a.cpp", (1, 2), (3, 4), (5, 6))]}]}
        counts = report.export_file_counts(export, "/r")
        self.assertEqual(counts, {"src/a.cpp": {"lines": (1, 2), "functions": (3, 4),
                                                "branches": (5, 6)}})

    def test_missing_branches_are_zero(self):
        entry = {"filename": "/r/src/a.cpp",
                 "summary": {"lines": {"count": 2, "covered": 1},
                             "functions": {"count": 1, "covered": 1}}}
        counts = report.export_file_counts({"data": [{"files": [entry]}]}, "/r")
        self.assertEqual(counts["src/a.cpp"]["branches"], (0, 0))

    def test_invalid_export(self):
        for export in ({}, {"data": [{"files": [{"filename": "x"}]}]},
                       {"data": [{"files": [file_entry("/r/src/a.cpp", (-1, 2), (0, 0), (0, 0))]}]}):
            with self.subTest(export=export):
                with self.assertRaises(report.InputError):
                    report.export_file_counts(export, "/r")

    def test_scope_extension_filter(self):
        for name in ("a.c", "a.cc", "a.cpp", "a.cxx", "a.h", "a.hh", "a.hpp", "a.hxx", "a.inc"):
            self.assertTrue(report.is_source_file("src/" + name), name)
        for name in ("CMakeLists.txt", "a.ui", "a.glsl", "a.png", "a.cmake", "a.py"):
            self.assertFalse(report.is_source_file("src/" + name), name)


class ReportMainTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        self.env = mock.patch.dict(os.environ, git_environment(self.root), clear=True)
        self.env.start()
        self.repo, self.export = make_synthetic_repo(self.root)
        self.out = os.path.join(self.root, "coverage")

    def tearDown(self):
        self.env.stop()
        self.temp.cleanup()

    def run_main(self, *extra):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = report.main(["--export", self.export, "--source-dir", self.repo,
                                  "--groups", GROUPS_JSON, "--output-dir", self.out, *extra])
        return status, stdout.getvalue(), stderr.getvalue()

    def load_outputs(self):
        with open(os.path.join(self.out, "summary.json"), encoding="utf-8") as handle:
            summary = json.load(handle, parse_float=Decimal)
        parser = TableParser()
        with open(os.path.join(self.out, "html", "index.html"), encoding="utf-8") as handle:
            text = handle.read()
        parser.feed(text)
        return summary, parser, text

    def test_scope_files_from_git(self):
        self.assertEqual(report.scope_files(self.repo), sorted(SYNTHETIC_SCOPE))

    def test_scope_files_outside_git(self):
        with self.assertRaises(report.InputError):
            report.scope_files(self.root)

    def test_summary_content(self):
        status, stdout, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertIn("Coverage_Scope lines:", stdout)
        summary, _, _ = self.load_outputs()
        self.assertEqual(sorted(summary["files"]), sorted(SYNTHETIC_SCOPE))
        self.assertEqual(summary["files"]["src/common/sub/never.cpp"]["lines"],
                         {"covered": 0, "total": 0, "percent": Decimal("0.00")})
        self.assertEqual(summary["files"]["src/updater/updater.cpp"]["lines"]["percent"],
                         Decimal("0.50"))
        self.assertEqual(summary["files"]["src/launcher/gtk/window.cpp"]["lines"]["percent"],
                         Decimal("0.01"))
        self.assertEqual(sorted(summary["directories"]),
                         ["src/common", "src/common/sub", "src/launcher", "src/launcher/core",
                          "src/launcher/gtk", "src/updater"])
        self.assertEqual(summary["directories"]["src/common"]["lines"],
                         {"covered": 3, "total": 11, "percent": Decimal("27.27")})
        self.assertEqual(list(summary["groups"]), ["launcher_core", "updater", "gtk_presentation"])
        self.assertEqual(summary["groups"]["launcher_core"]["functions"]["percent"],
                         Decimal("95.00"))
        self.assertEqual(summary["scope"]["lines"]["total"], 3 + 3 + 8 + 10 + 200 + 16000)

    def test_html_matches_summary(self):
        self.assertEqual(self.run_main()[0], 0)
        summary, parser, text = self.load_outputs()
        self.assertIn('<html lang="en">', text)
        self.assertNotIn("http", text)
        self.assertEqual(text.count("<caption>"), 4)
        self.assertEqual(text.count("<table>"), text.count('<thead><tr><th scope="col">'))
        tables = parser.tables
        self.assertEqual(table_values(tables["Coverage_Scope total"]),
                         {"src": expected_cells(summary["scope"])})
        for caption, section in (("Groups", "groups"),
                                 ("Directories under src (recursive)", "directories"),
                                 ("Files", "files")):
            with self.subTest(table=caption):
                self.assertEqual(table_values(tables[caption]),
                                 {name: expected_cells(metrics)
                                  for name, metrics in summary[section].items()})
        self.assertIn("src/launcher/gtk/window.cpp", table_values(tables["Files"]))

    def test_file_links_only_for_existing_pages(self):
        files_dir = os.path.join(self.root, "show")
        page = os.path.join(files_dir, "coverage",
                            os.path.join(self.repo, "src/common/a.cpp").lstrip("/") + ".html")
        os.makedirs(os.path.dirname(page))
        with open(page, "w", encoding="utf-8") as handle:
            handle.write("<html></html>")
        self.assertEqual(self.run_main("--html-files-dir", files_dir)[0], 0)
        _, parser, _ = self.load_outputs()
        self.assertEqual(list(parser.links), ["src/common/a.cpp"])
        href = parser.links["src/common/a.cpp"]
        self.assertEqual(os.path.normpath(os.path.join(self.out, "html", href)), page)

    def test_default_files_dir_links(self):
        page = os.path.join(self.out, "html", "files", "coverage",
                            os.path.join(self.repo, "src/main.cpp").lstrip("/") + ".html")
        os.makedirs(os.path.dirname(page))
        with open(page, "w", encoding="utf-8") as handle:
            handle.write("<html></html>")
        self.assertEqual(self.run_main()[0], 0)
        _, parser, _ = self.load_outputs()
        self.assertEqual(parser.links["src/main.cpp"],
                         "files/coverage/" + os.path.join(self.repo, "src/main.cpp").lstrip("/")
                         + ".html")

    def test_html_escapes_names(self):
        summary = report.build_summary({}, ["src/a<b>/c&d.cpp"], {"g<1>": ["src/"]})
        text = report.render_html(summary, {})
        self.assertIn("src/a&lt;b&gt;/c&amp;d.cpp", text)
        self.assertIn("g&lt;1&gt;", text)
        self.assertNotIn("<b>", text)

    def test_input_errors(self):
        bad = os.path.join(self.root, "bad.json")
        with open(bad, "w", encoding="utf-8") as handle:
            handle.write("{not json")
        for option, value in (("--export", bad), ("--export", os.path.join(self.root, "none")),
                              ("--groups", bad)):
            with self.subTest(option=option, value=value):
                stderr = io.StringIO()
                arguments = {"--export": self.export, "--groups": GROUPS_JSON, option: value}
                with contextlib.redirect_stderr(stderr):
                    status = report.main(["--export", arguments["--export"],
                                          "--source-dir", self.repo,
                                          "--groups", arguments["--groups"],
                                          "--output-dir", self.out])
                self.assertEqual(status, 2)
                self.assertIn("error:", stderr.getvalue())


class GroupsFileTests(unittest.TestCase):
    def test_repository_groups(self):
        prefixes, thresholds = report.load_groups(GROUPS_JSON)
        self.assertEqual(prefixes, {"launcher_core": ["src/launcher/core/"],
                                    "updater": ["src/updater/"],
                                    "gtk_presentation": ["src/launcher/gtk/"]})
        self.assertEqual(thresholds, {"launcher_core": {"lines": 90, "functions": 95},
                                      "updater": {"lines": 90},
                                      "gtk_presentation": {"lines": 60}})

    def test_invalid_groups(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "groups.json")
            for data in ({}, {"groups": {"g": {"paths": []}}},
                         {"groups": {"g": {"paths": ["src/"], "thresholds": {"regions": 5}}}},
                         {"groups": {"g": {"paths": ["src/"], "thresholds": {"lines": 101}}}}):
                with self.subTest(data=data):
                    with open(path, "w", encoding="utf-8") as handle:
                        json.dump(data, handle)
                    with self.assertRaises(report.InputError):
                        report.load_groups(path)


if __name__ == "__main__":
    unittest.main()
