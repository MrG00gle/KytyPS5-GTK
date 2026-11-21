"""Unit tests for tools/upstream/platform_merge.py and upstream_paths.py, on scratch repositories.

The tests that need the real unifdef are skipped when it is not installed; the others use a fake
strip function (FakeStripper) or a stand-in unifdef program (scratch_repository.FAKE_UNIFDEF).
"""

import contextlib
import io
import os
import shutil
import subprocess
import unittest

from scratch_repository import FakeStripper, ScratchRepository

import platform_merge  # noqa: E402
import upstream_paths  # noqa: E402

UNIFDEF = shutil.which("unifdef")

BASE = ("// A source with a removed-platform block.\n"
        "int a = 1;\n"
        "#ifdef _WIN32\n"
        "int windows_only = 1;\n"
        "#endif\n"
        "int b = 2;\n"
        "int c = 3;\n")
# The fork removed the block (as phase 2 did) and added a line of its own.
OURS = ("// A source with a removed-platform block.\n"
        "int a = 1;\n"
        "int b = 2;\n"
        "int c = 3;\n"
        "int fork_only = 4;\n")
# Upstream changed a line outside the block and one inside it.
THEIRS = ("// A source with a removed-platform block.\n"
          "int a = 10;\n"
          "#ifdef _WIN32\n"
          "int windows_only = 2;\n"
          "#endif\n"
          "int b = 2;\n"
          "int c = 3;\n")
MERGED = ("// A source with a removed-platform block.\n"
          "int a = 10;\n"
          "int b = 2;\n"
          "int c = 3;\n"
          "int fork_only = 4;\n")
PATH = "src/common/sample.cpp"


class Helper(ScratchRepository):
    def merge(self, commit, stripper=None):
        """Run the helper with stripper (default: a FakeStripper); return (status, summary)."""
        stripper = stripper or FakeStripper()
        return platform_merge.run(self.repo, commit, self.report, self.auto,
                                  lambda scratch: stripper)

    def main(self, *arguments):
        """platform_merge.main(arguments) with the fake unifdef; (status, stdout, stderr)."""
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = platform_merge.main(["--repo", self.repo, "--report", self.report,
                                          "--auto-dir", self.auto, *arguments])
        return status, stdout.getvalue(), stderr.getvalue()

    def summary_value(self, item):
        for line in self.section("Summary"):
            cells = [cell.strip() for cell in line.strip("|").split("|")]
            if cells[0] == item:
                return cells[1]
        raise AssertionError(f"no summary item {item!r}")

    def in_merge(self):
        return self.git("rev-parse", "-q", "--verify", "MERGE_HEAD", check=False) != ""


class StripMerge(Helper):
    def test_a_removed_platform_block_on_both_sides_merges_cleanly(self):
        theirs = self.trio({PATH: BASE}, {PATH: OURS}, {PATH: THEIRS})
        # A plain merge conflicts: the fork deleted the block that upstream changed.
        plain = subprocess.run(["git", "-C", self.repo, "merge-tree", "--write-tree", "HEAD",
                                theirs], capture_output=True, stdin=subprocess.DEVNULL,
                               check=False)
        self.assertEqual(plain.returncode, 1)
        status, summary = self.merge(theirs)
        self.assertEqual(status, 0, self.report_text())
        self.assertTrue(self.in_merge())
        self.assertEqual(self.read(PATH), MERGED)
        self.assertEqual(self.staged(PATH), MERGED)
        self.assertEqual(self.unmerged(), {})
        self.assertEqual(self.git("diff", "--name-only"), "")
        with open(os.path.join(self.auto, PATH), encoding="utf-8") as handle:
            self.assertEqual(handle.read(), MERGED)
        self.assertEqual(self.summary_value("Strip-merged paths"),
                         "1, 0 with conflicts (0 hunks)")
        self.assertEqual(self.summary_value("Hand work remains"), "no")
        self.assertIn("| Hand work remains | no |", summary)
        self.assertEqual(self.section("Leftover removed-platform directives"), ["None."])

    @unittest.skipUnless(UNIFDEF, "unifdef is not installed")
    def test_both_unifdef_passes_with_the_real_unifdef(self):
        platform = ("#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS\n"
                    "int platform = {};\n"
                    "#else\n"
                    "int platform = 5;\n"
                    "#endif\n")
        theirs = self.trio({PATH: BASE + platform.format(1)},
                           {PATH: OURS + "int platform = 5;\n"},
                           {PATH: THEIRS + platform.format(2)})
        status, _ = self.merge(theirs, platform_merge.UnifdefStripper(UNIFDEF, self.temp.name))
        self.assertEqual(status, 0, self.report_text())
        self.assertEqual(self.read(PATH), MERGED + "int platform = 5;\n")
        self.assertEqual(self.staged(PATH), MERGED + "int platform = 5;\n")

    def test_a_conflict_hunk_is_reported_and_left_unmerged(self):
        theirs = self.trio({PATH: BASE}, {PATH: OURS.replace("c = 3", "c = 30")},
                           {PATH: THEIRS.replace("c = 3", "c = 300")})
        status, _ = self.merge(theirs)
        self.assertEqual(status, 1)
        self.assertEqual(self.unmerged(), {PATH: [1, 2, 3]})
        text = self.read(PATH)
        self.assertIn("<<<<<<< ours\nint c = 30;\nint fork_only = 4;\n=======\nint c = 300;\n"
                      ">>>>>>> upstream\n", text)
        self.assertNotIn("_WIN32", text)
        stripped_base = BASE.replace("#ifdef _WIN32\nint windows_only = 1;\n#endif\n", "")
        self.assertEqual(self.git("show", f":1:{PATH}") + "\n", stripped_base)
        self.assertEqual(self.git("show", f":2:{PATH}") + "\n", OURS.replace("c = 3", "c = 30"))
        self.assertNotIn("_WIN32", self.git("show", f":3:{PATH}"))
        self.assertIn(f"| `{PATH}` | changed upstream | 1 | 0 |",
                      self.section("Strip-merged paths"))
        self.assertEqual(self.summary_value("Strip-merged paths"), "1, 1 with conflicts (1 hunks)")
        self.assertEqual(self.summary_value("Hand work remains"), "yes")
        self.assertEqual(self.section("Other conflicts"), ["None."])

    def test_digit_separators_retry_the_pass_with_t(self):
        separators = "int big = 1'000;\n"
        theirs = self.trio({PATH: BASE}, {PATH: OURS},
                           {PATH: THEIRS.replace("int a = 10;\n", separators + "int a = 10;\n")})
        status, stdout, _ = self.main("--unifdef", self.fake_unifdef(), theirs)
        self.assertEqual(status, 0, self.report_text())
        self.assertEqual(self.read(PATH),
                         MERGED.replace("int a = 10;\n", separators + "int a = 10;\n"))
        self.assertEqual(self.section("unifdef -t retries"),
                         [f"- `{PATH}` (upstream, pass 1)", f"- `{PATH}` (upstream, pass 2)"])
        self.assertIn("platform_merge: unifdef -t retries: 2\n", stdout)
        self.assertIn(f"platform_merge: report: {self.report}\n", stdout)

    @unittest.skipUnless(UNIFDEF, "unifdef is not installed")
    def test_digit_separators_with_the_real_unifdef(self):
        separators = "int big = 1'000;\n"
        theirs = self.trio({PATH: BASE}, {PATH: OURS},
                           {PATH: THEIRS.replace("int a = 10;\n", separators + "int a = 10;\n")})
        status, _, stderr = self.main("--unifdef", UNIFDEF, theirs)
        self.assertEqual(status, 0, stderr)
        self.assertEqual(self.read(PATH),
                         MERGED.replace("int a = 10;\n", separators + "int a = 10;\n"))
        self.assertIn(f"- `{PATH}` (upstream, pass 1)", self.section("unifdef -t retries"))

    def test_leftover_directives_are_reported_with_their_lines(self):
        added = "src/common/added.cpp"
        content = ("int x;\n"
                   "#if defined(__linux__) || defined(_WIN32)\n"
                   "int both = 1;\n"
                   "#endif\n"
                   "/* #ifdef _WIN32 in a comment */\n"
                   'const char* text = "#ifdef _WIN32";\n')
        theirs = self.trio({"README.md": "x\n"}, {}, {added: content})
        status, _ = self.merge(theirs)
        self.assertEqual(status, 1)
        self.assertEqual(self.read(added), content)
        self.assertEqual(self.staged(added), content)
        self.assertEqual(self.section("Leftover removed-platform directives"),
                         [f"- `{added}:2`: `#if defined(__linux__) || defined(_WIN32)`"])
        self.assertEqual(self.summary_value("Leftover removed-platform directives"),
                         "1 in 1 files")
        self.assertIn(f"- `{added}` (strip): stripped", self.section("Files added upstream"))

    def test_vendored_code_and_other_files_are_not_stripped(self):
        vendored = "3rdparty/lib/vendored.h"
        script = "tools/build.sh"
        theirs = self.trio({"README.md": "x\n"}, {},
                           {vendored: THEIRS, script: "#!/bin/sh\necho _WIN32\n"})
        stripper = FakeStripper()
        status, _ = self.merge(theirs, stripper)
        self.assertEqual(status, 0, self.report_text())
        self.assertEqual(stripper.calls, [])
        self.assertEqual(self.read(vendored), THEIRS)
        self.assertEqual(self.staged(script), "#!/bin/sh\necho _WIN32\n")


class PathRules(Helper):
    def test_a_path_the_fork_deleted_stays_deleted(self):
        source, script = "src/common/old.cpp", "tools/old.sh"
        theirs = self.trio({source: BASE, script: "echo 1\n"}, {source: None, script: None},
                           {source: THEIRS, script: "echo 2\n"})
        status, _ = self.merge(theirs)
        self.assertEqual(status, 0, self.report_text())
        for path in (source, script):
            self.assertIsNone(self.read(path))
            self.assertIsNone(self.staged(path))
        self.assertEqual(self.unmerged(), {})
        self.assertEqual(self.section("Fork-deleted paths"),
                         [f"- `{source}`: changed upstream; deleted in the fork",
                          f"- `{script}`: changed upstream; deleted in the fork"])

    def test_upstream_additions_in_removed_areas_are_left_out(self):
        vendored = next(path for path in upstream_paths.DELETED_PATHS
                        if path.startswith("3rdparty/"))
        removed = ["src/launcher/src/newDialog.cpp", "src/launcher/forms/newDialog.ui",
                   "src/launcher/CMakeLists.txt", "tests/LauncherNewTests.cpp",
                   "src/common/platform/sysWindowsClock.cpp", f"{vendored}/include/sched.h"]
        theirs = self.trio({"README.md": "x\n"}, {},
                           {**{path: "int x;\n" for path in removed},
                            "src/common/kept.cpp": "int kept;\n"})
        status, _ = self.merge(theirs)
        self.assertEqual(status, 0, self.report_text())
        for path in removed:
            self.assertIsNone(self.read(path), path)
            self.assertIsNone(self.staged(path), path)
        self.assertEqual(self.staged("src/common/kept.cpp"), "int kept;\n")
        self.assertEqual(sorted(self.section("Removed areas")),
                         sorted(f"- `{path}`: added upstream; left out" for path in removed))
        self.assertEqual(self.summary_value("Removed areas (left out or kept deleted)"), "6")

    def test_fork_owned_files_keep_the_fork_side_and_list_the_upstream_commits(self):
        base = {"CMakeLists.txt": "project(x)\n", "README.md": "# x\n",
                "src/main.cpp": "int main() {}\n"}
        upstream = [
            ("build: add the jpeg library", {"CMakeLists.txt": "project(x)\nadd_library(jpeg)\n"}),
            ("docs: show a new screenshot", {"README.md": "# upstream\n"}),
            ("ci: add a job", {".github/workflows/upstream.yml": "name: x\n",
                               "docs/guide.md": "# guide\n"}),
            ("emulator: add --fast", {"src/main.cpp": "int main() { return 1; }\n",
                                      "CMakeLists.txt": "project(x)\nadd_library(fast)\n"}),
        ]
        theirs = self.trio(base, {"README.md": "# fork\n"}, None, upstream)
        status, _ = self.merge(theirs)
        self.assertEqual(status, 1)
        self.assertEqual(self.read("CMakeLists.txt"), "project(x)\n")
        self.assertEqual(self.read("README.md"), "# fork\n")
        self.assertEqual(self.read("src/main.cpp"), "int main() {}\n")
        self.assertIsNone(self.read(".github/workflows/upstream.yml"))
        self.assertIsNone(self.read("docs/guide.md"))
        self.assertEqual(self.unmerged(), {})
        self.assertEqual(self.git("diff", "--cached", "--name-only", "HEAD"), "")
        owned = self.section("Fork-owned paths")
        cmake = owned.index("### `CMakeLists.txt`")
        subjects = [line.split("` ", 1)[1] for line in owned[cmake + 2:cmake + 4]]
        self.assertEqual(subjects, ["emulator: add --fast", "build: add the jpeg library"])
        self.assertIn("### `src/main.cpp`", owned)
        self.assertIn("### `.github/workflows/upstream.yml`", owned)
        self.assertIn("Added upstream; left out.", owned)
        self.assertEqual(self.summary_value("Fork-owned paths (fork side kept)"),
                         "5, 5 with upstream commits to translate")

    def test_deleted_upstream_and_changed_in_the_fork_is_deleted_and_flagged(self):
        body = "".join(f"int line{number} = {number};\n" for number in range(20))
        changed, untouched, renamed = "src/common/a.cpp", "src/common/b.cpp", "src/common/c.cpp"
        theirs = self.trio({changed: body, untouched: "int b;\n"},
                           {changed: body + "int fork = 1;\n"},
                           {changed: None, untouched: None, renamed: body + "int up = 1;\n"})
        status, _ = self.merge(theirs)
        self.assertEqual(status, 1)
        for path in (changed, untouched):
            self.assertIsNone(self.read(path))
            self.assertIsNone(self.staged(path))
        self.assertEqual(self.read(renamed), body + "int up = 1;\n")
        self.assertEqual(self.unmerged(), {})
        self.assertEqual(self.section("Deleted upstream"), [
            f"- `{changed}`: deleted upstream; changed in the fork since the merge base; "
            f"upstream renamed it to {renamed}",
            f"- `{untouched}`: deleted upstream"])
        self.assertEqual(self.summary_value("Deleted upstream (strip class)"),
                         "2, 1 changed in the fork")

    def test_other_conflicts_are_left_to_git_and_listed(self):
        path = "src/utils.cmake"
        theirs = self.trio({path: "set(A 1)\n"}, {path: "set(A 2)\n"}, {path: "set(A 3)\n"})
        status, _ = self.merge(theirs)
        self.assertEqual(status, 1)
        self.assertEqual(self.unmerged(), {path: [1, 2, 3]})
        self.assertEqual(self.section("Other conflicts"),
                         [f"- `{path}`", f"  - CONFLICT (content): Merge conflict in {path}"])


class Refusals(Helper):
    def setUp(self):
        super().setUp()
        self.theirs = self.trio({PATH: BASE}, {PATH: OURS}, {PATH: THEIRS})
        self.unifdef = self.fake_unifdef()

    def assert_refused(self, fragment, *arguments):
        status, stdout, stderr = self.main("--unifdef", self.unifdef, *arguments, self.theirs)
        self.assertEqual(status, 2, stdout + stderr)
        self.assertIn(fragment, stderr)
        self.assertFalse(os.path.exists(self.report))
        return stderr

    def test_uncommitted_changes(self):
        self.write({PATH: OURS + "int dirty;\n"})
        self.assert_refused("has uncommitted changes or untracked files")
        self.assertFalse(self.in_merge())

    def test_untracked_files(self):
        self.write({"notes.txt": "x\n"})
        self.assert_refused("has uncommitted changes or untracked files")
        self.assertFalse(self.in_merge())

    def test_a_merge_in_progress(self):
        self.git("merge", "-q", "--no-ff", "--no-commit", self.theirs, check=False)
        self.assertTrue(self.in_merge())
        self.git("checkout", "-q", "--ours", "--", PATH)
        self.git("add", PATH)
        self.assert_refused("has a merge in progress")

    def test_an_upstream_commit_that_is_merged_already(self):
        self.git("merge", "-q", "--no-ff", "-X", "ours", "-m", "merge", self.theirs)
        self.assert_refused("is already merged into HEAD")

    def test_a_missing_unifdef(self):
        self.unifdef = os.path.join(self.temp.name, "absent", "unifdef")
        self.assert_refused("unifdef not found")
        self.assertFalse(self.in_merge())

    def test_an_auto_dir_that_is_not_empty(self):
        os.makedirs(self.auto)
        self.write({"x": "x\n"}, repo=self.auto)
        self.assert_refused("is not an empty directory")
        self.assertFalse(self.in_merge())

    def test_a_name_that_is_not_a_commit(self):
        self.theirs = "no-such-branch"
        self.assert_refused("is not a commit of")


class PathClasses(unittest.TestCase):
    def test_classes(self):
        cases = {
            "src/launcher/src/mainDialog.cpp": upstream_paths.REMOVED,
            "src/launcher/include/gameViews.h": upstream_paths.REMOVED,
            "src/launcher/CMakeLists.txt": upstream_paths.REMOVED,
            "tests/LauncherCheatTests.cpp": upstream_paths.REMOVED,
            "src/launcher/forms/a.ui": upstream_paths.REMOVED,
            "res/a.qrc": upstream_paths.REMOVED,
            "src/common/platform/sysWindowsDbg.cpp": upstream_paths.REMOVED,
            ".github/workflows/build.yml": upstream_paths.FORK_OWNED,
            ".vscode/launch.json": upstream_paths.FORK_OWNED,
            "CMakeLists.txt": upstream_paths.FORK_OWNED,
            "src/main.cpp": upstream_paths.FORK_OWNED,
            "docs/screens/guide.md": upstream_paths.FORK_OWNED,
            "packaging/flatpak/app.yml": upstream_paths.FORK_OWNED,
            "src/launcher/core/launch.cpp": upstream_paths.STRIP,
            "src/kernel/memoryAddressSpace.inc": upstream_paths.STRIP,
            "tests/launcher_gtk/LauncherWindowTests.cpp": upstream_paths.STRIP,
            "tests/ShaderRecompilerComputeTests.cpp": upstream_paths.STRIP,
            "3rdparty/fmt/include/fmt/core.h": upstream_paths.OTHER,
            "3rdparty/ui/form.ui": upstream_paths.OTHER,
            "src/utils.cmake": upstream_paths.OTHER,
            "src/graphics/host_gpu/shaders/a.comp": upstream_paths.OTHER,
            "docs/screenshots/a.png": upstream_paths.OTHER,
            "3rdparty/CMakeLists.txt": upstream_paths.OTHER,
        }
        for path, klass in cases.items():
            with self.subTest(path=path):
                self.assertEqual(upstream_paths.classify(path), klass)
        for path, _ in upstream_paths.content_checks.DELETED_PATHS:
            with self.subTest(deleted=path):
                self.assertEqual(upstream_paths.classify(path), upstream_paths.REMOVED)
                self.assertEqual(upstream_paths.classify(path + "/x.h"), upstream_paths.REMOVED)

    def test_areas(self):
        cases = {"CMakeLists.txt": "(root)", "src/main.cpp": "src(top)",
                 "src/common/platform/sysTimer.h": "src/common",
                 "src/graphics/host_gpu/renderer/render.h": "src/graphics/host_gpu",
                 "src/launcher/src/mainDialog.cpp": "src/launcher/src",
                 "src/launcher/CMakeLists.txt": "src/launcher",
                 "tests/BitArrayTests.cpp": "tests",
                 ".github/workflows/build.yml": ".github/workflows",
                 "3rdparty/CMakeLists.txt": "3rdparty/CMakeLists.txt",
                 "3rdparty/ffx-api/LICENSE.txt": "3rdparty/ffx-api", "docs/a/b.md": "docs"}
        for path, area in cases.items():
            with self.subTest(path=path):
                self.assertEqual(upstream_paths.area(path), area)

    def test_platform_directives(self):
        text = ("#if defined(_WIN32)\n"                       # 1: found
                "#endif\n"
                "#  ifdef __APPLE__ // a comment\n"           # 3: found
                "#endif\n"
                "# /* a comment */ elif _MSC_VER >= 1900\n"   # 5: found
                "#define WIN32 1\n"                           # 6: not a conditional
                "// #ifdef _WIN32\n"
                "/*\n"
                "#ifdef _WIN32\n"                             # 9: inside a block comment
                "*/\n"
                "#if FOO // _WIN32\n"                         # 11: only in a comment
                "#if X == \\\n"                               # 12: found, continued
                "    KYTY_PLATFORM_WINDOWS\n"
                "#if NOT_WIN32 || WIN32_LEAN\n"               # 14: other identifiers
                "int separators = 1'000; /* _WIN32 */\n"
                '#if defined(A) && B("_WIN32")\n'             # 16: only in a string
                "#elifdef __MINGW64__\n")                     # 17: found
        self.assertEqual(upstream_paths.platform_directives(text), [
            (1, "#if defined(_WIN32)"), (3, "# ifdef __APPLE__ // a comment"),
            (5, "# /* a comment */ elif _MSC_VER >= 1900"),
            (12, "#if X == KYTY_PLATFORM_WINDOWS"), (17, "#elifdef __MINGW64__")])


if __name__ == "__main__":
    unittest.main()
