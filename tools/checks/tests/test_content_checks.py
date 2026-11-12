"""Unit tests for tools/checks/content_checks.py on scratch trees: the core_text check (phase 9
V1, Requirement 6.12) and the documentation checks readme_structure, docs_terms, issue_template,
and packaging_lint_exceptions_documented (task 29.7).

core_text keeps the GTK launcher from showing a Launcher_Core or Updater message as it is: a read of
a .message, .reason, or .error member (also through ->), .error().c_str(), .Error(),
RefusalMessage(, or a core::k...Message / updater::k...Message constant in src/launcher/gtk/
fails unless it is inside the arguments of a log call or its line ends with "// core text: <why>".
coreMessageText.cpp (the mapping) and services/ (the library adapters) are not scanned.

The documentation checks start from small valid fixtures, which pass, and each mutation breaks one
rule, which fails with a message naming it. Removed platform terms and Qt words are built from
pieces, so that this file contains none of them.
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


# ---- The documentation checks

WIN = "Win" "dows"
MAC = "mac" "OS"
NIXOS = "Nix" "OS"
QT = "Q" "t"
MSVC = "MS" "VC"
MOLTEN = "Molten" "VK"
FORK = "https://github.com/MrG00gle/KytyPS5-GTK"
BADGE = (f"[![Build]({FORK}/actions/workflows/build.yml/badge.svg?branch=main)]"
         f"({FORK}/actions/workflows/build.yml?query=branch%3Amain)")
README = f"""\
# KytyPS5-GTK

{BADGE}

KytyPS5-GTK is a fork of [upstream KytyPS5](https://github.com/KytyPS5/KytyPS5), which is based on
[Kyty](https://github.com/InoriRus/Kyty).

> [!IMPORTANT]
> KytyPS5-GTK is not affiliated with Sony Interactive Entertainment. You must use only legally
> obtained game files.

## Fork changes

How KytyPS5-GTK differs from upstream KytyPS5:

- **Linux x86-64 only.** Nothing else.
- **GTK launcher.** A GTK 4 launcher replaces the {QT} 6 launcher of upstream KytyPS5.
- **Flatpak package.** The Flatpak `io.github.MrG00gle.KytyPS5_GTK`.
- **No {WIN} support.** No {MSVC} either.
- **No {MAC} support.** No {MOLTEN}.
- **No Nix or {NIXOS} support.** No flakes.
- **Coverage targets.** 90 % of the lines and 95 % of the functions of the core, 90 % of the
  Updater, 60 % of the GTK code, and at most 0.50 points below the baseline.

## Project status

Upstream KytyPS5 is in development. Releases are on the
[releases page]({FORK}/releases); report problems in the [issue tracker]({FORK}/issues). See the
[upstream KytyPS5 compatibility list](https://kytyps5.github.io/) and the
[upstream KytyPS5 Discord](https://discord.gg/example).

<img src="docs/shot.png" alt="A game running in upstream KytyPS5">

## System requirements

- The only supported platform is Linux on an x86-64 processor.
- A GPU with Vulkan 1.3 support is required.

## Install with Flatpak

```bash
flatpak install --user ./KytyPS5-GTK-<date>-<commit>-x86_64.flatpak
flatpak run io.github.MrG00gle.KytyPS5_GTK
```

## Building from source

See the [building guide](docs/building.md).

## Running

Run `kyty_launcher`.

## Documentation

- [Building](docs/building.md)
- [Packaging](docs/packaging.md)
- [Contributing](CONTRIBUTING.md)

## Contributing

Pull requests target `dev`.

## License

KytyPS5-GTK is licensed under the [GNU General Public License version 2](LICENSE), SPDX identifier
`GPL-2.0-only`. This project is based on the original [Kyty](https://github.com/InoriRus/Kyty),
which was released under the MIT License ([`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt)).
Third-party components remain subject to their own licenses.

## Acknowledgments

- [Upstream KytyPS5](https://github.com/KytyPS5/KytyPS5): the emulator.
- [Kyty](https://github.com/InoriRus/Kyty): the original project.
- [shadPS4](https://github.com/shadps4-emu/shadPS4): a reference.
"""
ISSUE_FORM = """\
name: Report
description: A report.
body:
  - type: markdown
    attributes:
      value: Thanks.
  - type: input
    id: distribution
    attributes:
      label: Linux distribution and version
      placeholder: "Example: Fedora 43 Workstation"
    validations:
      required: true
  - type: dropdown
    id: installation-type
    attributes:
      label: Installation type
      options:
        - Flatpak
        - release archive
        - source build
    validations:
      required: true
  - type: textarea
    id: log
    attributes:
      label: Log
      placeholder: "Attach ~/kyty/_kyty.txt"
"""
LINT_EXCEPTIONS = ('{"io.github.MrG00gle.KytyPS5_GTK": ["appid-url-not-reachable", '
                   '"appstream-screenshots-not-mirrored-in-ostree"]}\n')
PACKAGING = """\
# Packaging

| Lint error | Justification |
|---|---|
| `appid-url-not-reachable` | The linter derives a URL from the App_ID that cannot exist. |
| `appstream-screenshots-not-mirrored-in-ostree` | A bundle has no screenshot mirror at all. |

Off `main`, CI also excepts `appstream-missing-screenshots`: the screenshot URL resolves only
after the merge.
"""
WORKFLOW = """\
jobs:
  flatpak:
    steps:
      - run: |
          python3 -B tools/ci/lint_exceptions.py \\
            --exceptions packaging/flatpak/lint-exceptions.json --app-id "$APP_ID" \\
            --add appstream-missing-screenshots --output "$OUT"
"""


class Documentation(ScratchTree):
    """The fixtures of the documentation checks in a scratch tree; files lists them."""

    def setUp(self):
        super().setUp()
        self.files = set()
        for path, text in (("README.md", README), ("CONTRIBUTING.md", "# Contributing\n"),
                           ("docs/building.md", "# Building\n"), ("docs/shot.png", ""),
                           ("LICENSE", ""), ("LICENSES/Kyty-MIT.txt", ""),
                           (".github/workflows/build.yml", WORKFLOW),
                           (".github/ISSUE_TEMPLATE/report.yaml", ISSUE_FORM),
                           ("packaging/flatpak/lint-exceptions.json", LINT_EXCEPTIONS),
                           ("docs/packaging.md", PACKAGING)):
            self.put(path, text)

    def put(self, path, text):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(text)
        self.files.add(path)
        return path

    def edit(self, path, old, new):
        with open(os.path.join(self.root, path), encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn(old, text, f"{old!r} is not in {path}")
        self.put(path, text.replace(old, new, 1))

    def run_function(self, check):
        violations, _ = content_checks.FUNCTION_CHECKS[check][2](self.root, sorted(self.files))
        return violations

    def assert_passes(self, check):
        self.assertEqual(self.run_function(check), [])

    def assert_fails(self, check, *fragments):
        """The check reports exactly one violation, holding every fragment."""
        violations = self.run_function(check)
        self.assertEqual(len(violations), 1, violations)
        for fragment in fragments:
            self.assertIn(fragment, violations[0])
        return violations[0]


class ReadmeStructure(Documentation):
    def test_the_fixture_passes(self):
        self.assert_passes("readme_structure")

    def test_a_missing_readme(self):
        os.remove(os.path.join(self.root, "README.md"))
        self.assert_fails("readme_structure", "README.md: missing (Requirement 19.1)")

    def test_a_second_first_level_heading(self):
        self.edit("README.md", "## Running\n", "# Running\n")
        violations = self.run_function("readme_structure")
        self.assertIn("exactly one first-level heading", violations[0])
        self.assertIn("'# KytyPS5-GTK', '# Running'", violations[0])

    def test_headings_out_of_design_order(self):
        self.edit("README.md", "## Project status\n", "## Status\n")
        self.assert_fails("readme_structure", "not design.md's", "(Requirements 19.2, 19.5)")

    def test_a_third_level_heading_is_allowed(self):
        self.edit("README.md", "Run `kyty_launcher`.\n", "### Launcher\n\nRun `kyty_launcher`.\n")
        self.assert_passes("readme_structure")

    def test_headings_in_code_blocks_do_not_count(self):
        self.edit("README.md", "flatpak run", "# a comment\nflatpak run")
        self.assert_passes("readme_structure")

    def test_the_fork_statement_needs_both_links_in_one_sentence(self):
        self.edit("README.md", "which is based on\n[Kyty](https://github.com/InoriRus/Kyty).",
                  "a translation layer. It is based on\n[Kyty](https://github.com/InoriRus/Kyty).")
        self.assert_fails("readme_structure", "is a fork of upstream KytyPS5",
                          "(Requirement 19.1)")

    def test_the_sony_statement(self):
        self.edit("README.md", "not affiliated with Sony", "independent of Sony")
        self.assert_fails("readme_structure", "Sony Interactive Entertainment",
                          "(Requirement 19.6)")

    def test_the_legal_files_statement(self):
        self.edit("README.md", "use only legally", "use legally")
        self.assert_fails("readme_structure", "only legally obtained", "(Requirement 19.6)")

    def test_a_missing_fork_change_item(self):
        self.edit("README.md", f"- **No {MAC} support.** No Molten" "VK.\n", "")
        self.assert_fails("readme_structure", f"no list item for the removal of {MAC} support",
                          "(Requirement 19.2)")

    def test_two_topics_in_one_item(self):
        self.edit("README.md", f"- **No {MAC} support.** No Molten" "VK.\n", "")
        self.edit("README.md", f"- **No {WIN} support.**", f"- **No {WIN} or {MAC} support.**")
        self.assert_fails("readme_structure", "no list item of its own for the removal of",
                          "(Requirement 19.2)")

    def test_a_missing_coverage_threshold(self):
        self.edit("README.md", "60 % of the GTK code", "most of the GTK code")
        self.assert_fails("readme_structure", "the coverage targets", "(Requirement 19.2)")

    def test_an_empty_topic_section(self):
        self.edit("README.md", "Pull requests target `dev`.\n", "")
        self.assert_fails("readme_structure", "## Contributing has no paragraph, list, or code",
                          "(Requirement 19.5)")

    def test_no_build_badge(self):
        self.edit("README.md", BADGE, "")
        self.assert_fails("readme_structure", "no build status badge", "(Requirement 19.3)")

    def test_a_badge_that_links_elsewhere(self):
        self.edit("README.md", "build.yml?query=branch%3Amain", "../../releases")
        self.assert_fails("readme_structure", "links to", "not "
                          f"{FORK}/actions/workflows/build.yml", "(Requirement 19.3)")

    def test_a_badge_of_another_repository(self):
        self.edit("README.md", f"({FORK}/actions/workflows/build.yml/badge.svg",
                  "(https://github.com/KytyPS5/KytyPS5/actions/workflows/build.yml/badge.svg")
        self.assert_fails("readme_structure", "names no workflow of MrG00gle/KytyPS5-GTK",
                          "(Requirement 19.3)")

    def test_a_badge_of_a_missing_workflow(self):
        self.edit("README.md", "build.yml/badge.svg", "release.yml/badge.svg")
        self.assert_fails("readme_structure", "names no workflow", "(Requirement 19.3)")

    def test_an_upstream_releases_link(self):
        self.edit("README.md", f"[releases page]({FORK}/releases)",
                  "[releases page](https://github.com/KytyPS5/KytyPS5/releases/latest)")
        self.assert_fails("readme_structure", "the releases link", f"not under {FORK}/releases",
                          "(Requirement 19.3)")

    def test_an_upstream_issues_link(self):
        self.edit("README.md", f"[issue tracker]({FORK}/issues)",
                  "[issue tracker](https://github.com/KytyPS5/KytyPS5/issues/new)")
        self.assert_fails("readme_structure", f"not under {FORK}/issues", "(Requirement 19.3)")

    DISCORD = " and the\n[upstream KytyPS5 Discord](https://discord.gg/example)."

    def test_an_unlabeled_community_link(self):
        self.edit("README.md", self.DISCORD, ". Chat on [Discord](https://discord.gg/example).")
        self.assert_fails("readme_structure", "https://discord.gg/example", '"upstream KytyPS5"',
                          "(Requirement 19.4)")

    def test_a_community_link_labeled_in_its_sentence(self):
        self.edit("README.md", self.DISCORD,
                  ". Chat on the [Discord](https://discord.gg/example) of upstream KytyPS5.")
        self.assert_passes("readme_structure")

    def test_another_sentence_does_not_label_a_link(self):
        self.edit("README.md", self.DISCORD, ". Chat on [Discord](https://discord.gg/example). "
                  "It is the upstream KytyPS5 chat.")
        self.assert_fails("readme_structure", "https://discord.gg/example", "(Requirement 19.4)")

    def test_a_bare_kytyps5_without_upstream(self):
        self.edit("README.md", "Upstream KytyPS5 is in development.", "KytyPS5 is in development.")
        self.assert_fails("readme_structure", "KytyPS5 without \"upstream\"",
                          "KytyPS5 is in development.", "(Requirement 19.10)")

    def test_an_alternative_text_without_upstream(self):
        self.edit("README.md", "running in upstream KytyPS5", "running in KytyPS5")
        self.assert_fails("readme_structure", "KytyPS5 without \"upstream\"",
                          "(Requirement 19.10)")

    def test_kytyps5_gtk_and_urls_need_no_label(self):
        self.edit("README.md", "Run `kyty_launcher`.",
                  "Run KytyPS5-GTK, see https://github.com/KytyPS5/KytyPS5 and the "
                  "`io.github.MrG00gle.KytyPS5_GTK` App_ID.")
        self.assert_passes("readme_structure")

    def test_the_system_requirements(self):
        self.edit("README.md", "The only supported platform is Linux",
                  "The supported platform is Linux")
        self.edit("README.md", "Vulkan 1.3 support is required", "Vulkan support is required")
        violations = self.run_function("readme_structure")
        self.assertEqual(len(violations), 2, violations)
        self.assertIn("only supported platform is Linux on an x86-64", violations[0])
        self.assertIn("Vulkan 1.3 support is required (Requirement 19.8)", violations[1])

    def test_the_flatpak_commands(self):
        self.edit("README.md", "flatpak run io.github.MrG00gle.KytyPS5_GTK",
                  "flatpak run io.github.MrG00gle.KytyPS5_GTK.Devel")
        self.assert_fails("readme_structure", "no flatpak run io.github.MrG00gle.KytyPS5_GTK",
                          "(Requirement 19.9)")
        self.edit("README.md", "flatpak install --user", "flatpak-install --user")
        self.assertEqual(len(self.run_function("readme_structure")), 2)

    def test_a_documentation_file_without_a_link(self):
        self.put("docs/testing.md", "# Testing\n")
        self.assert_fails("readme_structure", "no relative link to docs/testing.md",
                          "(Requirement 20.9)")

    def test_an_absolute_link_to_a_guide_does_not_count(self):
        self.edit("README.md", "- [Building](docs/building.md)\n",
                  f"- [Building]({FORK}/blob/main/docs/building.md)\n")
        self.edit("README.md", "[building guide](docs/building.md)",
                  f"[building guide]({FORK}/blob/main/docs/building.md)")
        self.assert_fails("readme_structure", "no relative link to docs/building.md")

    def test_the_license_statements(self):
        self.edit("README.md", "`GPL-2.0-only`", "GPL 2")
        self.assert_fails("readme_structure", "the SPDX identifier GPL-2.0-only",
                          "(Requirement 21.2)")

    def test_the_license_links(self):
        self.edit("README.md", "](LICENSE)", "](https://www.gnu.org/licenses/gpl-2.0.html)")
        self.assert_fails("readme_structure", "no relative link to LICENSE", "(Requirement 21.2)")

    def test_the_kyty_license_notice(self):
        self.edit("README.md", "Third-party components remain", "Other code remains")
        self.assert_fails("readme_structure", "third-party components remain subject",
                          "(Requirement 21.5)")

    def test_a_missing_acknowledgment(self):
        self.edit("README.md",
                  "- [shadPS4](https://github.com/shadps4-emu/shadPS4): a reference.\n", "")
        self.assert_fails("readme_structure", "0 list items that link to "
                          "https://github.com/shadps4-emu/shadPS4", "(Requirement 21.3)")


class DocsTerms(Documentation):
    def test_the_fixture_passes_with_the_fork_changes_terms(self):
        self.assert_passes("docs_terms")

    def test_a_qt_word_outside_fork_changes(self):
        self.edit("README.md", "Run `kyty_launcher`.", f"Run `kyty_launcher` ({QT.lower()}6-free).")
        violation = self.assert_fails("docs_terms", "README.md:", "word that begins with qt",
                                      "Requirement 19.7")
        self.assertIn(f"'{QT.lower()}'", violation)

    def test_a_qt_word_in_any_case_and_in_a_guide(self):
        self.put("docs/building.md", f"# Building\n\nUnset {QT.upper()}_PLUGIN_PATH first.\n")
        self.assert_fails("docs_terms", "docs/building.md:3:", "word that begins with qt")

    def test_words_that_only_contain_qt_pass(self):
        self.put("docs/building.md", "# Building\n\nThe fake_qt_name variable; Q-tips.\n")
        self.assert_passes("docs_terms")

    def test_a_removed_platform_term_in_contributing(self):
        self.put("CONTRIBUTING.md", f"# Contributing\n\n{WIN} builds are gone.\n")
        self.assert_fails("docs_terms", "CONTRIBUTING.md:3:", "Removed_Platform_Term",
                          f"'{WIN}'", "Requirement 19.7")

    def test_terms_are_case_sensitive_whole_words(self):
        self.put("docs/building.md", f"# Building\n\nThe launcher {WIN.lower()} and "
                 f"{WIN}Manager.\n")
        self.assert_passes("docs_terms")

    def test_an_exe_file_name(self):
        self.put("docs/building.md", "# Building\n\nRun `kyty_emulator.e" "xe`.\n")
        self.assert_fails("docs_terms", ".exe file name", "Requirement 3.6")

    def test_an_apple_term(self):
        self.put("docs/building.md", "# Building\n\nInstall Home" "brew first.\n")
        self.assert_fails("docs_terms", "Apple term", "Requirement 4.5")

    def test_a_nix_term(self):
        self.put("docs/building.md", "# Building\n\nRun nix" "-shell.\n")
        self.assert_fails("docs_terms", "Removed_Platform_Term")

    def test_only_the_documentation_set_is_scanned(self):
        self.put("src/notes.md", f"{WIN} {QT}\n")
        self.put("docs/screens/readme.txt", f"{WIN} {QT}\n")
        self.assert_passes("docs_terms")

    def test_a_qt_word_after_the_fork_changes_section(self):
        self.edit("README.md", "Pull requests target `dev`.",
                  f"Pull requests target `dev`, not {QT}.")
        self.assert_fails("docs_terms", "README.md:", "word that begins with qt")


class IssueTemplate(Documentation):
    PATH = ".github/ISSUE_TEMPLATE/report.yaml"

    def test_the_fixture_passes(self):
        self.assert_passes("issue_template")

    def test_a_missing_dropdown_option(self):
        self.edit(self.PATH, "        - source build\n", "")
        self.assert_fails("issue_template", "the options of installation-type are",
                          "not exactly ['Flatpak', 'release archive', 'source build']")

    def test_an_extra_or_renamed_dropdown_option(self):
        self.edit(self.PATH, "- release archive", "- Release archive")
        self.assert_fails("issue_template", "the options of installation-type")

    def test_an_optional_distribution(self):
        self.edit(self.PATH, "      placeholder: \"Example: Fedora 43 Workstation\"\n"
                  "    validations:\n      required: true\n",
                  "      placeholder: \"Example: Fedora 43 Workstation\"\n")
        self.assert_fails("issue_template", "the field distribution is a input (not required), "
                          "not a required input")

    def test_a_missing_distribution_field(self):
        self.edit(self.PATH, "id: distribution", "id: os")
        self.assert_fails("issue_template", "0 fields with the id distribution")

    def test_a_distribution_label_without_the_version(self):
        self.edit(self.PATH, "label: Linux distribution and version", "label: Linux distribution")
        self.assert_fails("issue_template", "does not ask for the Linux distribution and version")

    def test_a_dropdown_as_text_input(self):
        self.edit(self.PATH, "type: dropdown", "type: input")
        self.assert_fails("issue_template", "the field installation-type is a input, not a "
                          "required dropdown")

    def test_a_removed_platform_term(self):
        self.edit(self.PATH, "Example: Fedora 43 Workstation", f"Example: {WIN} 11")
        self.assert_fails("issue_template", "report.yaml:", "Removed_Platform_Term",
                          "Requirement 20.14")

    def test_a_non_linux_placeholder(self):
        self.edit(self.PATH, "Attach ~/kyty/_kyty.txt", "Attach C:\\\\Kyty\\\\_kyty.txt")
        self.assert_fails("issue_template", "the placeholder of log", "is not a Linux example")

    def test_every_template_is_checked(self):
        self.put(".github/ISSUE_TEMPLATE/config.yml", "blank_issues_enabled: false\n")
        self.put(".github/ISSUE_TEMPLATE/other.md", "---\nname: Other\n---\nText.\n")
        self.assert_fails("issue_template", "other.md: a Markdown template cannot require fields")

    def test_invalid_yaml(self):
        self.put(self.PATH, "body: [unclosed\n")
        self.assert_fails("issue_template", "report.yaml: not valid YAML")

    def test_no_templates(self):
        os.remove(os.path.join(self.root, self.PATH))
        self.files.discard(self.PATH)
        self.assert_fails("issue_template", "no issue template")


class LintExceptionsDocumented(Documentation):
    def test_the_fixture_passes(self):
        self.assert_passes("packaging_lint_exceptions_documented")

    def test_a_missing_lint_id(self):
        self.edit("docs/packaging.md", "| `appid-url-not-reachable` | The linter derives a URL "
                  "from the App_ID that cannot exist. |\n", "")
        self.assert_fails("packaging_lint_exceptions_documented", "does not name the lint "
                          "exception appid-url-not-reachable",
                          "listed in packaging/flatpak/lint-exceptions.json")

    def test_a_lint_id_without_a_justification(self):
        self.edit("docs/packaging.md", "A bundle has no screenshot mirror at all.", "repository")
        self.assert_fails("packaging_lint_exceptions_documented", "docs/packaging.md:6: names "
                          "the lint exception appstream-screenshots-not-mirrored-in-ostree "
                          "without a justification sentence")

    def test_a_lint_id_only_in_a_code_block(self):
        self.edit("docs/packaging.md", "Off `main`, CI also excepts `appstream-missing-screenshots`"
                  ": the screenshot URL resolves only\nafter the merge.\n",
                  "```\nappstream-missing-screenshots\n```\n")
        self.assert_fails("packaging_lint_exceptions_documented", "does not name the lint "
                          "exception appstream-missing-screenshots",
                          "listed in .github/workflows/build.yml:")

    def test_a_longer_id_does_not_count(self):
        self.edit("docs/packaging.md", "`appid-url-not-reachable`",
                  "`appid-url-not-reachable-at-all`")
        self.assert_fails("packaging_lint_exceptions_documented", "appid-url-not-reachable ")

    def test_a_new_exception_must_be_documented(self):
        self.put("packaging/flatpak/lint-exceptions.json",
                 LINT_EXCEPTIONS.replace('"]}', '", "finish-args-flatpak-spawn-access"]}'))
        self.assert_fails("packaging_lint_exceptions_documented",
                          "finish-args-flatpak-spawn-access")

    def test_a_missing_guide(self):
        os.remove(os.path.join(self.root, "docs/packaging.md"))
        self.assert_fails("packaging_lint_exceptions_documented", "docs/packaging.md: missing")

    def test_a_malformed_exceptions_file(self):
        self.put("packaging/flatpak/lint-exceptions.json", '{"io.github.x": "one-id"}\n')
        self.assert_fails("packaging_lint_exceptions_documented",
                          "the value of io.github.x is not a list of lint IDs")


class DocumentationCommandLine(Documentation):
    def test_the_checks_run_on_a_scratch_repository(self):
        subprocess.run(["git", "init", "-q", self.root], check=True)
        for check in ("readme_structure", "docs_terms", "issue_template",
                      "packaging_lint_exceptions_documented"):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = content_checks.main(["--repo", self.root, "--check", check])
            lines = stdout.getvalue().splitlines()
            self.assertEqual(status, 0, lines)
            self.assertTrue(lines[-1].startswith(f"content_check {check}: PASS"), lines)
        self.edit("README.md", "## Running\n", "## Usage\n")
        self.put("docs/testing.md", "# Testing\n")
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = content_checks.main(["--repo", self.root, "--check", "readme_structure"])
        lines = stdout.getvalue().splitlines()
        self.assertEqual(status, 1)
        self.assertEqual(len(lines), 3, lines)
        self.assertTrue(lines[0].startswith("README.md: the first- and second-level headings"))
        self.assertTrue(lines[1].startswith("README.md: no relative link to docs/testing.md"))
        self.assertTrue(lines[-1].startswith("content_check readme_structure: FAIL, 2 "
                                             "violation(s) of Requirements 19.1-19.6"), lines)


if __name__ == "__main__":
    unittest.main()
