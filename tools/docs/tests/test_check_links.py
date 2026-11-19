"""Unit tests for tools/docs/check_links.py (task 29.7): GitHub's heading anchors, the link and
image checks of the Documentation_Set on scratch trees, and the line anchors of links to files
that are not Markdown."""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

DOCS_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, DOCS_TOOLS)

import check_links  # noqa: E402


def anchors_of(text):
    return check_links.anchors(textwrap.dedent(text).split("\n"))


class Slugs(unittest.TestCase):
    def test_punctuation_is_dropped_and_spaces_become_hyphens(self):
        self.assertEqual(check_links.slug("What's new? (v2.0)"), "whats-new-v20")
        self.assertEqual(check_links.slug("Build, test & install: the 3 steps!"),
                         "build-test--install-the-3-steps")
        self.assertEqual(check_links.slug("snake_case and kebab-case"),
                         "snake_case-and-kebab-case")

    def test_unicode_letters_stay_and_symbols_go(self):
        self.assertEqual(check_links.slug("Über Café 日本語"), "über-café-日本語")
        self.assertEqual(check_links.slug("Rocket \U0001F680 launch"), "rocket--launch")
        self.assertEqual(check_links.slug("Size × 2 → ½"), "size--2--")

    def test_duplicates_get_the_first_free_suffix(self):
        anchors = anchors_of("""\
            # Usage
            ## Usage
            ### Usage
            ## Usage-1
            ## Usage
            """)
        self.assertEqual(anchors, {"usage", "usage-1", "usage-2", "usage-1-1", "usage-3"})

    def test_code_spans_in_headings_keep_their_text(self):
        anchors = anchors_of("""\
            ## The `--game` option
            ## `_PipelineCache` and `_RenderDoc`
            ## ``a ` b``
            """)
        self.assertEqual(anchors, {"the---game-option", "_pipelinecache-and-_renderdoc", "a--b"})

    def test_inline_markup_shows_only_its_text(self):
        anchors = anchors_of("""\
            ## **Bold**, [a link](other.md), and _emphasis_
            ## Title <sup>beta</sup> ![icon](icon.png)
            ## Escaped \\_underscore\\_ and &amp; entity
            ## Closing hashes ##
            """)
        self.assertEqual(anchors, {"bold-a-link-and-emphasis", "title-beta-",
                                   "escaped-_underscore_-and--entity", "closing-hashes"})

    def test_setext_headings_html_anchors_and_code_blocks(self):
        anchors = anchors_of("""\
            Setext title
            ============

            Second level
            ------------

            <a id="custom-anchor"></a>
            <a name="legacy"></a>

            ```markdown
            # Not a heading
            <a id="not-an-anchor"></a>
            ```

            Inline `<a id="code"></a>` is code.
            #not-a-heading
            """)
        self.assertEqual(anchors, {"setext-title", "second-level", "custom-anchor", "legacy"})


class ScratchTree(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="kyty_check_links_")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.paths = []

    def write(self, path, text=""):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(textwrap.dedent(text))
        self.paths.append(path)
        return path

    def problems(self):
        problems, _, _ = check_links.check(self.root, sorted(self.paths))
        return problems


class Links(ScratchTree):
    def setUp(self):
        super().setUp()
        self.write("docs/guide.md", """\
            # Guide

            ## Data directory

            <a id="custom"></a>
            """)
        self.write("docs/screenshots/main.png")
        self.write("LICENSE")
        self.write("src/main.cpp", "int main() {}\n")

    def test_good_links_pass(self):
        self.write("README.md", """\
            # Project

            See the [guide](docs/guide.md), its [data](docs/guide.md#data-directory) section,
            the [custom anchor](docs/guide.md#custom), [this file](#project), the
            [license](./LICENSE "GPL"), the [sources](src/), [line 1](src/main.cpp#L1), and
            [the root](/docs/guide.md).

            ![Main window](docs/screenshots/main.png)
            [![Badge](https://example.org/badge.svg)](https://example.org/actions)
            <img src="docs/screenshots/main.png" alt="Main"> <a href="docs/guide.md#custom">x</a>

            [ref]: docs/guide.md#data-directory
            """)
        self.write("docs/running.md", "Back to the [README](../README.md#project).\n")
        problems, documents, relative = check_links.check(self.root, sorted(self.paths))
        self.assertEqual(problems, [])
        self.assertEqual(documents, 3)
        self.assertEqual(relative, 13)

    def test_a_broken_file_reports_file_line_and_target(self):
        self.write("README.md", "# Project\n\nRead\nthe [missing guide](docs/missing.md).\n")
        self.assertEqual(self.problems(), [
            "README.md:4: docs/missing.md: no file or directory docs/missing.md in the repository"])

    def test_a_broken_anchor(self):
        self.write("README.md", "[data](docs/guide.md#data-folder) and [self](#nothing)\n")
        self.assertEqual(self.problems(), [
            "README.md:1: docs/guide.md#data-folder: docs/guide.md has no heading or HTML anchor "
            "#data-folder",
            "README.md:1: #nothing: README.md has no heading or HTML anchor #nothing"])

    def test_a_missing_image_and_a_missing_badge_image(self):
        self.write("README.md", """\
            ![Main](docs/screenshots/missing.png)
            [![Badge](docs/badge.svg)](https://example.org/actions)
            <img src="docs/screenshots/gone.png" alt="Gone">
            """)
        problems = self.problems()
        self.assertEqual([problem.split(": ")[0] for problem in problems],
                         ["README.md:1", "README.md:2", "README.md:3"])
        self.assertIn("docs/screenshots/missing.png: no file", problems[0])
        self.assertIn("docs/badge.svg: no file", problems[1])
        self.assertIn("docs/screenshots/gone.png: no file", problems[2])

    def test_html_anchors_and_hrefs(self):
        self.write("README.md", """\
            <a href="docs/guide.md#custom">ok</a>
            <a href='docs/guide.md#missing'>bad anchor</a>
            <a href="docs/nothing.md">bad file</a>
            """)
        self.assertEqual([problem.split(": ")[0] for problem in self.problems()],
                         ["README.md:2", "README.md:3"])

    def test_code_and_comments_are_not_links(self):
        self.write("README.md", """\
            ```markdown
            [broken](docs/missing.md)
            ```
            Inline `[broken](docs/missing.md)` and <!-- [broken](docs/missing.md) -->.
            ~~~
            <img src="missing.png">
            ~~~
            """)
        self.assertEqual(self.problems(), [])

    def test_external_targets_are_not_checked(self):
        self.write("README.md", """\
            [web](https://example.org/missing.md#x) [mail](mailto:someone@example.org)
            [scheme-relative](//example.org/x) [ftp](ftp://example.org/x)
            """)
        self.assertEqual(self.problems(), [])

    def test_targets_outside_the_repository_and_empty_targets(self):
        self.write("docs/running.md", "[up](../../outside.md) [empty]()\n")
        self.assertEqual(self.problems(), [
            "docs/running.md:1: ../../outside.md: ../outside.md is outside the repository",
            "docs/running.md:1: : the link has no target"])

    def test_reference_definitions_escapes_and_queries(self):
        self.write("docs/my file.md", "# Title\n")
        self.write("README.md", """\
            [a] [b] [c]

            [a]: docs/my%20file.md#title
            [b]: <docs/my file.md>
            [c]: docs/guide.md?plain=1#missing
            """)
        self.assertEqual(self.problems(), [
            "README.md:5: docs/guide.md?plain=1#missing: docs/guide.md has no heading or HTML "
            "anchor #missing"])

    def test_only_the_documentation_set_is_scanned(self):
        self.write("src/notes.md", "[broken](missing.md)\n")
        self.write("docs/nested/deep.md", "[broken](missing.md)\n")
        self.write("CONTRIBUTING.md", "[broken](missing.md)\n")
        self.assertEqual([problem.split(": ")[0] for problem in self.problems()],
                         ["CONTRIBUTING.md:1", "docs/nested/deep.md:1"])


class LineAnchors(ScratchTree):
    """The fragment of a link to a file that is not Markdown must be a line anchor within it."""

    def setUp(self):
        super().setUp()
        self.write("docs/guide.md", "# Guide\n")
        self.write("src/a.cpp", "x\ny\nz\n")
        self.write("src/b.txt", "x\ny")
        self.write("src/one.txt", "x")
        self.write("src/empty.h")
        with open(os.path.join(self.root, "docs/shot.png"), "wb") as handle:
            handle.write(b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR\n")
        self.paths.append("docs/shot.png")

    def link_problems(self, *targets):
        """The problems of a README.md that links to each target on a line of its own."""
        self.write("README.md", "".join(f"[link]({target})\n" for target in targets))
        return self.problems()

    def test_line_anchors_within_the_file_pass(self):
        self.assertEqual(self.link_problems("src/a.cpp#L2", "src/a.cpp#L1-L3", "src/a.cpp#L3",
                                            "src/a.cpp#L2-L2", "src/b.txt#L2", "src/one.txt#L1",
                                            "src/a.cpp", "docs/shot.png"), [])

    def test_lines_outside_the_file_fail(self):
        self.assertEqual(self.link_problems("src/a.cpp#L4", "src/a.cpp#L2-L4", "src/a.cpp#L3-L2",
                                            "src/a.cpp#L0", "src/b.txt#L3", "src/one.txt#L2",
                                            "src/empty.h#L1"), [
            "README.md:1: src/a.cpp#L4: src/a.cpp has 3 lines, so #L4 names no line",
            "README.md:2: src/a.cpp#L2-L4: src/a.cpp has 3 lines, so #L2-L4 names no line",
            "README.md:3: src/a.cpp#L3-L2: src/a.cpp has 3 lines, so #L3-L2 names no line",
            "README.md:4: src/a.cpp#L0: src/a.cpp has 3 lines, so #L0 names no line",
            "README.md:5: src/b.txt#L3: src/b.txt has 2 lines, so #L3 names no line",
            "README.md:6: src/one.txt#L2: src/one.txt has 1 line, so #L2 names no line",
            "README.md:7: src/empty.h#L1: src/empty.h has 0 lines, so #L1 names no line"])

    def test_other_fragments_fail(self):
        targets = ("src/a.cpp#L02", "src/a.cpp#l2", "src/a.cpp#L2C1", "src/a.cpp#foo",
                   "src/a.cpp#L1-2", "src/a.cpp#L1-L2-L3")
        problems = self.link_problems(*targets)
        self.assertEqual(problems, [
            f"README.md:{number}: {target}: src/a.cpp is not a Markdown file; only a line anchor "
            "#L<n> or #L<n>-L<m> can name a place in it"
            for number, target in enumerate(targets, 1)])

    def test_a_binary_file_takes_no_fragment(self):
        self.assertEqual(self.link_problems("docs/shot.png#L1"), [
            "README.md:1: docs/shot.png#L1: docs/shot.png is a binary file; a link fragment "
            "cannot name a place in it"])

    def test_markdown_and_directory_fragments_are_unchanged(self):
        self.assertEqual(self.link_problems("docs/guide.md#guide", "docs/guide.md#L1", "src/#L1",
                                            "src#anything", "/#L9"), [
            "README.md:2: docs/guide.md#L1: docs/guide.md has no heading or HTML anchor #L1"])


class CommandLine(ScratchTree):
    def run_main(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = check_links.main(list(arguments))
        return status, stdout.getvalue().splitlines(), stderr.getvalue()

    def test_a_scratch_repository(self):
        subprocess.run(["git", "init", "-q", self.root], check=True)
        self.write("README.md", "# Project\n\nSee the [guide](docs/guide.md#usage).\n")
        self.write("docs/guide.md", "# Guide\n")
        status, lines, _ = self.run_main("--repo", self.root)
        self.assertEqual(status, 1)
        self.assertEqual(lines, [
            "README.md:3: docs/guide.md#usage: docs/guide.md has no heading or HTML anchor #usage",
            "FAILED: docs_links: 1 broken link(s) in 2 files (Requirements 20.10, 20.13)"])
        self.write("docs/guide.md", "# Guide\n\n## Usage\n")
        status, lines, _ = self.run_main("--repo", self.root)
        self.assertEqual(status, 0)
        self.assertEqual(lines, ["PASS: docs_links: 1 relative links and images in 2 files "
                                 "resolve"])

    def test_ignored_files_do_not_count(self):
        subprocess.run(["git", "init", "-q", self.root], check=True)
        self.write(".gitignore", "build/\n")
        self.write("build/out.txt")
        self.write("README.md", "[output](build/out.txt)\n")
        status, lines, _ = self.run_main("--repo", self.root)
        self.assertEqual(status, 1)
        self.assertIn("no file or directory build/out.txt", lines[0])

    def test_no_documentation_fails(self):
        subprocess.run(["git", "init", "-q", self.root], check=True)
        status, lines, _ = self.run_main("--repo", self.root)
        self.assertEqual(status, 1)
        self.assertTrue(lines[-1].startswith("FAILED: docs_links: no Documentation_Set file"))

    def test_input_errors(self):
        status, lines, stderr = self.run_main("--repo", os.path.join(self.root, "missing"))
        self.assertEqual((status, lines), (2, []))
        self.assertIn("is not a directory", stderr)
        status, lines, stderr = self.run_main("--repo", self.root)
        self.assertEqual((status, lines), (2, []))
        self.assertIn("git ls-files failed", stderr)


if __name__ == "__main__":
    unittest.main()
