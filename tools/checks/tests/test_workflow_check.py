"""Unit tests for tools/checks/workflow_check.py (task 28.3).

The committed workflow passes, and mutated copies of it fail with one line that names the problem,
the job, and the step. PyYAML is required: without it this module fails to import with the
checker's message.
"""

import contextlib
import copy
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CHECKS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = Path(CHECKS_DIR).parent.parent
SCRIPT = os.path.join(CHECKS_DIR, "workflow_check.py")
WORKFLOW = REPO / ".github" / "workflows" / "build.yml"
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CHECKS_DIR)

import workflow_check  # noqa: E402

try:
    import yaml  # noqa: E402
except ImportError:
    raise ImportError(workflow_check.YAML_MISSING) from None

# A commit SHA for the actions that tests add: every action must be pinned to one (class Pins).
FAKE_SHA = "0123456789abcdef0123456789abcdef01234567"
DEPENDABOT = ".github/dependabot.yml"
# Built from pieces, so that this file names no Qt package or action.
QT_UBUNTU = "q" "t6-base-dev"
QT_FEDORA = "q" "t6-qtbase-devel"
QT_ACTION = "jurplel/install-q" f"t-action@{FAKE_SHA}"
QT_INSTALLER = "aqt" "install"
RELEASE_IF = ("github.event_name == 'push' && github.ref == 'refs/heads/main' && "
              "github.repository == 'MrG00gle/KytyPS5-GTK'")


def find_step(data, job, key):
    """The step of job whose id or name is key."""
    steps = [step for step in data["jobs"][job]["steps"]
             if step.get("id") == key or step.get("name") == key]
    if len(steps) != 1:
        raise AssertionError(f"{len(steps)} steps of job {job} have the id or name {key!r}")
    return steps[0]


def step_index(data, job, key):
    return data["jobs"][job]["steps"].index(find_step(data, job, key))


def replace_in_run(data, job, key, old, new):
    step = find_step(data, job, key)
    if old not in step["run"]:
        raise AssertionError(f"{old!r} is not in the run of {job}/{key}")
    step["run"] = step["run"].replace(old, new, 1)


def move_step(data, job, key, before=None, after=None):
    steps = data["jobs"][job]["steps"]
    step = steps.pop(step_index(data, job, key))
    target = step_index(data, job, before) if before else step_index(data, job, after) + 1
    steps.insert(target, step)


# A uses: line with a value and, optionally, its comment.
USES_LINE = re.compile(r"^(\s*(?:-\s+)?uses:\s+)(\S+)(\s+#.*)?$")
PINNED = re.compile(r"\S+@[0-9a-f]{40}")


class Workflow(unittest.TestCase):
    """The committed workflow, a temporary directory for mutated copies, and a runner."""

    @classmethod
    def setUpClass(cls):
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.data = yaml.safe_load(cls.text)
        # uses: value -> its version comment in the committed workflow, which dump() puts back.
        cls.comments = {}
        for line in cls.text.splitlines():
            match = USES_LINE.match(line)
            if match and match.group(3):
                cls.comments[match.group(2)] = match.group(3)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "build.yml"

    def tearDown(self):
        self.temp.cleanup()

    def run_check(self, text):
        self.path.write_text(text, encoding="utf-8")
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = workflow_check.main(["--workflow", str(self.path)])
        return status, stdout.getvalue().splitlines(), stderr.getvalue()

    @classmethod
    def dump(cls, data):
        """The data as YAML. yaml.safe_dump drops comments, so each action pinned to a SHA gets
        its committed version comment back (# v1 for an action that a test added)."""
        text = yaml.safe_dump(data, sort_keys=False, allow_unicode=True, width=1 << 20)
        lines = []
        for line in text.splitlines():
            match = USES_LINE.match(line)
            if match and not match.group(3) and PINNED.fullmatch(match.group(2)):
                line += cls.comments.get(match.group(2), " # v1")
            lines.append(line)
        return "\n".join(lines) + "\n"

    def mutated(self, change):
        data = copy.deepcopy(self.data)
        change(data)
        return self.dump(data)

    def assert_passes(self, text):
        status, lines, stderr = self.run_check(text)
        self.assertEqual(status, 0, (lines, stderr))
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith("PASS: workflow_structure: 6 jobs, "), lines)

    def assert_fails(self, change, *fragments, count=1):
        """The mutated copy exits 1 with count problem lines, one of them holding every
        fragment."""
        status, lines, stderr = self.run_check(self.mutated(change))
        self.assertEqual(status, 1, (lines, stderr))
        self.assertEqual(lines[-1], f"FAILED: workflow_structure: {count} problem(s)", lines)
        matching = [line for line in lines[:-1] if all(f in line for f in fragments)]
        self.assertTrue(matching, f"no line with {fragments} in {lines}")
        return lines[:-1]

    def assert_input_error(self, text, fragment):
        status, lines, stderr = self.run_check(text)
        self.assertEqual(status, 2, (lines, stderr))
        self.assertEqual(lines, [])
        self.assertIn(fragment, stderr)


class CommittedWorkflow(Workflow):
    def test_the_committed_workflow_passes(self):
        self.assert_passes(self.text)

    def test_a_yaml_round_trip_passes(self):
        self.assert_passes(self.dump(self.data))

    def test_on_as_a_string_key_passes(self):
        self.assertIn("\non:\n", self.text)
        self.assert_passes(self.text.replace("\non:\n", '\n"on":\n', 1))

    def test_the_summary_counts_the_test_runs_and_uploads(self):
        status, lines, _ = self.run_check(self.text)
        self.assertEqual(status, 0)
        # ubuntu: full run and rerun; fedora: stage A, stage B, and the stage B rerun.
        self.assertIn(" 5 ctest test runs, 5 uploads", lines[0])

    def test_quoted_and_commented_ctest_words_are_not_calls(self):
        def change(data):
            step = find_step(data, "ubuntu", "tests")
            step["run"] += 'echo "ctest -R x"\n# ctest -L gpu\nprintf %s \'ctest -E y\'\n'
        self.assert_passes(self.mutated(change))

    def test_the_command_line(self):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, "-B", SCRIPT, "--workflow", str(WORKFLOW)],
                                capture_output=True, text=True, env=environment,
                                stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
        self.assertTrue(result.stdout.startswith("PASS: workflow_structure: "), result.stdout)
        missing = Path(self.temp.name) / "missing.yml"
        result = subprocess.run([sys.executable, "-B", SCRIPT, "--workflow", str(missing)],
                                capture_output=True, text=True, env=environment,
                                stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 2, (result.stdout, result.stderr))
        self.assertIn("cannot read", result.stderr)


class InputErrors(Workflow):
    def test_invalid_yaml(self):
        self.assert_input_error("jobs: [unclosed\n", "is not valid YAML")

    def test_top_level_not_a_mapping(self):
        self.assert_input_error("- push\n", "the top level is not a mapping")

    def test_no_jobs(self):
        self.assert_input_error("on: push\n", "jobs is not a mapping of jobs")

    def test_steps_not_a_list(self):
        self.assert_input_error("on: push\njobs:\n  ubuntu:\n    steps: run\n",
                                "the steps of job ubuntu are not a list of mappings")


class Triggers(Workflow):
    def test_missing_trigger(self):
        self.assert_fails(lambda data: data[True].pop("workflow_dispatch"),
                          "workflow: on: lacks the workflow_dispatch trigger")

    def test_missing_branch(self):
        self.assert_fails(lambda data: data[True]["push"].update(branches=["main"]),
                          "on.push.branches is ['main'], not main and dev")

    def test_wrong_pull_request_type(self):
        self.assert_fails(
            lambda data: data[True]["pull_request"].update(types=["opened", "reopened", "edited"]),
            "on.pull_request.types", "not exactly opened, reopened, and synchronize")

    def test_path_filter(self):
        self.assert_fails(lambda data: data[True]["push"].update(paths=["src/**"]),
                          "on.push: the paths filter can skip runs")

    def test_other_trigger(self):
        self.assert_fails(lambda data: data[True].update(schedule=[{"cron": "0 3 * * *"}]),
                          "on: has the schedule trigger")


class Runners(Workflow):
    def test_windows_latest_runner(self):
        def change(data):
            data["jobs"]["metadata"]["runs-on"] = "windows-latest"
        self.assert_fails(change, "job metadata: uses the runner windows-latest",
                          "Requirements 3.5, 4.4")

    def test_macos_14_runner(self):
        self.assert_fails(lambda data: data["jobs"]["ubuntu"].update({"runs-on": "macos-14"}),
                          "job ubuntu: uses the runner macos-14")

    def test_macos_runner_in_a_matrix(self):
        def change(data):
            job = data["jobs"]["coverage"]
            job["runs-on"] = "${{ matrix.os }}"
            job["strategy"] = {"matrix": {"os": ["ubuntu-24.04", "macos-14"]}}
        self.assert_fails(change, "job coverage: uses the runner macos-14")

    def test_other_ubuntu_image(self):
        self.assert_fails(lambda data: data["jobs"]["fedora"].update({"runs-on": "ubuntu-latest"}),
                          "job fedora: runs on 'ubuntu-latest', not ubuntu-24.04")

    def test_wrong_container_image_tag(self):
        def change(data):
            data["jobs"]["flatpak"]["container"]["image"] = (
                "ghcr.io/flathub-infra/flatpak-github-actions:gnome-48")
        self.assert_fails(change, "job flatpak: has the container", ":gnome-48'",
                          "not the container ghcr.io/flathub-infra/flatpak-github-actions:gnome-49")

    def test_fedora_container_removed(self):
        self.assert_fails(lambda data: data["jobs"]["fedora"].pop("container"),
                          "job fedora: has no container, not the container "
                          "registry.fedoraproject.org/fedora:latest")

    def test_container_on_another_job(self):
        self.assert_fails(lambda data: data["jobs"]["ubuntu"].update(container="ubuntu:24.04"),
                          "job ubuntu: has the container 'ubuntu:24.04', not no container")

    def test_extra_job(self):
        def change(data):
            data["jobs"]["lint"] = {"runs-on": "ubuntu-24.04", "timeout-minutes": 10,
                                    "steps": [{"run": "true"}]}
        lines = self.assert_fails(change, "job lint: is not one of the design's jobs", count=2)
        self.assertIn("job release: needs lacks lint; the release job needs every other job "
                      "(Requirement 16.10)", lines)

    def test_missing_job(self):
        def change(data):
            data["jobs"].pop("flatpak")
            data["jobs"]["release"]["needs"].remove("flatpak")
        self.assert_fails(change, "workflow: the design's job flatpak is missing")


class QtPackages(Workflow):
    def test_qt_package_in_an_apt_line(self):
        def change(data):
            replace_in_run(data, "release", "Check the bundle's metainfo release entry",
                           "--yes flatpak ostree", f"--yes flatpak ostree {QT_UBUNTU}")
        self.assert_fails(change, 'job release, step "Check the bundle\'s metainfo release entry"',
                          f"installs {QT_UBUNTU}, a Qt package (Requirement 16.3)")

    def test_qt_package_in_the_package_variable(self):
        def change(data):
            data["env"]["UBUNTU_PACKAGES"] += f" {QT_UBUNTU}"
        lines = self.assert_fails(change, f"installs {QT_UBUNTU} (from $UBUNTU_PACKAGES)",
                                  count=2)
        self.assertTrue(lines[0].startswith('job ubuntu, step "Install packages"'), lines)
        self.assertTrue(lines[1].startswith('job coverage, step "Install packages"'), lines)

    def test_qt_package_in_a_dnf_variable(self):
        def change(data):
            data["jobs"]["fedora"]["env"]["FEDORA_PACKAGES_B"] += f" {QT_FEDORA}"
        self.assert_fails(change, 'job fedora, step "Install the stage B packages"',
                          f"installs {QT_FEDORA} (from $FEDORA_PACKAGES_B)")

    def test_qt_word_in_a_pip_line(self):
        def change(data):
            step = find_step(data, "ubuntu", "Install packages")
            step["run"] += "pip3 install --user PyQ" "t6\n"
        self.assert_fails(change, 'job ubuntu, step "Install packages"', "installs PyQ" "t6")

    def test_qt_installation_action(self):
        def change(data):
            data["jobs"]["ubuntu"]["steps"].insert(1, {"name": "Install the toolkit",
                                                       "uses": QT_ACTION})
        self.assert_fails(change, 'job ubuntu, step "Install the toolkit"',
                          f"uses {QT_ACTION}, a Qt installation action")

    @staticmethod
    def add_action(job, uses, inputs):
        def change(data):
            data["jobs"][job]["steps"].insert(1, {"name": "Install the build tools",
                                                  "uses": uses, "with": inputs})
        return change

    def test_qt_package_in_an_action_input(self):
        change = self.add_action("ubuntu", f"some/apt-action@{FAKE_SHA}",
                                 {"packages": f"cmake {QT_UBUNTU} ninja-build"})
        self.assert_fails(change, 'job ubuntu, step "Install the build tools"',
                          f"passes {QT_UBUNTU} in with:, a Qt package (Requirement 16.3)")

    def test_qt_package_in_a_multi_line_action_input(self):
        change = self.add_action("fedora", f"some/dnf-action@{FAKE_SHA}",
                                 {"packages": f"cmake\nninja-build\n{QT_FEDORA}\n"})
        self.assert_fails(change, 'job fedora, step "Install the build tools"',
                          f"passes {QT_FEDORA} in with:, a Qt package (Requirement 16.3)")

    def test_qt_installer_in_an_action_input(self):
        change = self.add_action("coverage", f"some/pip-action@{FAKE_SHA}",
                                 {"packages": f"{QT_INSTALLER}==3.1.21"})
        self.assert_fails(change, 'job coverage, step "Install the build tools"',
                          f"passes a Qt build or installation word in with: {QT_INSTALLER}==3.1.21")


class CtestFilters(Workflow):
    MAIN_RUN = 'ctest --test-dir "$BUILD_DIR" --output-on-failure'

    def test_name_filter_in_the_main_run(self):
        def change(data):
            replace_in_run(data, "ubuntu", "tests", self.MAIN_RUN,
                           'ctest --test-dir "$BUILD_DIR" -R x --output-on-failure')
        self.assert_fails(change, 'job ubuntu, step "Run every test": runs ctest with -R;',
                          "Requirement 16.4")

    def test_label_filter_in_the_main_run(self):
        def change(data):
            replace_in_run(data, "fedora", "tests", self.MAIN_RUN,
                           'ctest --test-dir "$BUILD_DIR" -L gpu --output-on-failure')
        self.assert_fails(change, 'job fedora, step "Stage B - run every test": runs ctest '
                          "with -L;")

    def test_long_exclusion_filter(self):
        def change(data):
            replace_in_run(data, "fedora", "Stage A - run every test", self.MAIN_RUN,
                           'ctest --test-dir "$BUILD_DIR" --exclude-regex=ui --output-on-failure')
        self.assert_fails(change, 'step "Stage A - run every test": runs ctest with '
                          "--exclude-regex=ui;")

    def test_fixture_filter_and_union(self):
        def change(data):
            replace_in_run(data, "ubuntu", "tests", self.MAIN_RUN,
                           'ctest --test-dir "$BUILD_DIR" -FA setup -U --output-on-failure')
        self.assert_fails(change, "runs ctest with -FA -U;")

    def test_filter_behind_env_and_a_path(self):
        def change(data):
            step = find_step(data, "ubuntu", "tests")
            step["run"] += 'env -u DISPLAY X=1 /usr/bin/ctest --test-dir "$BUILD_DIR" -E foo\n'
        self.assert_fails(change, 'step "Run every test": runs ctest with -E;')

    def test_filtered_test_list(self):
        def change(data):
            replace_in_run(data, "ubuntu", "tests", "--show-only=json-v1",
                           "--show-only=json-v1 -LE ui")
        self.assert_fails(change, 'step "Run every test": lists the tests with -LE;')

    def test_rerun_step_before_the_full_run(self):
        self.assert_fails(lambda data: move_step(data, "ubuntu", "timing-rerun", before="tests"),
                          'job ubuntu, step "Rerun the listed timing tests that failed": comes '
                          "before the job's test run")

    def test_rerun_step_after_the_result_check(self):
        def change(data):
            move_step(data, "fedora", "timing-rerun", after="Stage B - check the test results")
        self.assert_fails(change, 'job fedora, step "Stage B - run every test": no later step '
                          "runs junit_check.py --registered after its timing-rerun step")

    def test_rerun_step_without_the_candidates(self):
        def change(data):
            replace_in_run(data, "ubuntu", "timing-rerun", " --print-retry-candidates", "")
        self.assert_fails(change, 'step "Rerun the listed timing tests that failed": reruns '
                          "tests without taking their names from junit_check.py")

    def test_rerun_filters_in_a_step_with_another_id(self):
        def change(data):
            find_step(data, "ubuntu", "timing-rerun")["id"] = "rerun"
        # The step is now a filtered test run of its own, right after the full run.
        lines = self.assert_fails(change, 'step "Rerun the listed timing tests that failed": '
                                  "runs ctest with -R --repeat;", count=2)
        self.assertTrue(any('step "Run every test": no later step runs junit_check.py' in line
                            for line in lines), lines)

    # The ubuntu main run as one line, for the forms that wrap all of it.
    FULL_RUN = ('ctest --test-dir "$BUILD_DIR" --output-on-failure --test-output-size-passed '
                '262144 \\\n  --output-junit "$PWD/$BUILD_DIR/ctest.xml"')
    ONE_LINE_RUN = ('ctest --test-dir "$BUILD_DIR" --output-on-failure --test-output-size-passed '
                    '262144 --output-junit "$PWD/$BUILD_DIR/ctest.xml"')
    FILTERED_RUN = 'ctest --test-dir "$BUILD_DIR" -R x --output-on-failure'
    # Commands that run the command after them, with their options, operands, and "--". In an
    # option cluster (-Eu, -vk), a last letter that takes a value takes the next word; one that
    # is not last takes the rest of the word (-uroot, -k5).
    WRAPPERS = ("timeout --signal=KILL -k 10s 2h", "timeout -- 2h", "nice -n 10", "nice -5",
                "nohup", "stdbuf -oL -e L", "sudo -E -u runner", "sudo VAR=1",
                'xvfb-run -a -s "-screen 0 1x1x24"', "dbus-run-session --config-file=/x --",
                "cmake -E env --unset=DISPLAY X=1 --", "env -C /tmp", "exec -a name", "time -p",
                "sudo -Eu runner", "timeout -vk 5 2h", "sudo -uroot", "timeout -k5 2h",
                "env -iu DISPLAY")

    def assert_passes_with_runs(self, change, runs=5):
        status, lines, stderr = self.run_check(self.mutated(change))
        self.assertEqual(status, 0, (lines, stderr))
        self.assertEqual(len(lines), 1, lines)
        self.assertIn(f" {runs} ctest test runs, ", lines[0])

    def test_filter_behind_timeout_in_fedora_stage_b(self):
        def change(data):
            replace_in_run(data, "fedora", "tests", self.MAIN_RUN,
                           'timeout 2h ctest --test-dir "$BUILD_DIR" -E gpu --output-on-failure')
        self.assert_fails(change, 'job fedora, step "Stage B - run every test": runs ctest with '
                          "-E;", "Requirement 16.4")

    def test_filter_behind_a_wrapper(self):
        for wrapper in self.WRAPPERS:
            with self.subTest(wrapper=wrapper):
                def change(data, wrapper=wrapper):
                    replace_in_run(data, "ubuntu", "tests", self.MAIN_RUN,
                                   f"{wrapper} {self.FILTERED_RUN}")
                self.assert_fails(change, 'job ubuntu, step "Run every test": runs ctest with -R;')

    def test_filter_in_a_shell_script(self):
        quoted = self.FILTERED_RUN.replace('"', '\\"')
        for form in (f"bash -c '{self.FILTERED_RUN}'", f'sh -ec "{quoted}"',
                     f"bash -o pipefail -c '{self.FILTERED_RUN}'"):
            with self.subTest(form=form):
                def change(data, form=form):
                    replace_in_run(data, "ubuntu", "tests", self.FULL_RUN, form)
                self.assert_fails(change, 'job ubuntu, step "Run every test": runs ctest with -R;')

    def test_filtered_test_list_in_a_shell_script(self):
        listing = 'ctest --test-dir "$BUILD_DIR" --show-only=json-v1'

        def change(data):
            replace_in_run(data, "ubuntu", "tests", listing, f"bash -c '{listing} -LE gpu'")
        self.assert_fails(change, 'step "Run every test": lists the tests with -LE;')

    def test_wrapped_unfiltered_runs_pass(self):
        for name, new in (("timeout", f"timeout 6h {self.MAIN_RUN}"),
                          ("timeout -vk", f"timeout -vk 5 6h {self.MAIN_RUN}"),
                          ("bash -c", f"bash -c '{self.ONE_LINE_RUN}'")):
            with self.subTest(name):
                old = self.FULL_RUN if name == "bash -c" else self.MAIN_RUN

                def change(data, old=old, new=new):
                    replace_in_run(data, "ubuntu", "tests", old, new)
                self.assert_passes_with_runs(change)

    def test_a_command_lookup_is_no_run(self):
        def change(data):
            find_step(data, "ubuntu", "tests")["run"] += "command -v ctest\ncommand -V ctest\n"
        self.assert_passes_with_runs(change)


class TestResults(Workflow):
    def test_missing_junit_check(self):
        def change(data):
            steps = data["jobs"]["ubuntu"]["steps"]
            steps.remove(find_step(data, "ubuntu", "Check the test results"))
        self.assert_fails(change, 'job ubuntu, step "Run every test": no later step runs '
                          "junit_check.py --registered")

    def test_junit_check_without_registered(self):
        def change(data):
            replace_in_run(data, "fedora", "Stage A - check the test results",
                           '--registered "$BUILD_DIR/ctest-tests-stage-a.json" ', "")
        self.assert_fails(change, 'job fedora, step "Stage A - run every test": no later step '
                          "runs junit_check.py --registered after this test run")

    def test_coverage_target_without_junit_check(self):
        def change(data):
            steps = data["jobs"]["coverage"]["steps"]
            steps.remove(find_step(data, "coverage", "junit"))
        self.assert_fails(change, 'job coverage, step "Build the coverage target": no later '
                          "step runs junit_check.py --registered")

    def test_test_job_without_tests(self):
        def change(data):
            steps = data["jobs"]["coverage"]["steps"]
            steps.remove(find_step(data, "coverage", "coverage"))
        lines = self.assert_fails(change, "job coverage: runs no tests", count=2)
        # The result check's if names the removed step, so it would always be skipped.
        self.assertTrue(any(line.startswith('job coverage, step "Check the test results": if: ')
                            and "steps.coverage.outcome" in line for line in lines), lines)

    def test_missing_vk_driver_files(self):
        self.assert_fails(lambda data: data["jobs"]["coverage"]["env"].pop("VK_DRIVER_FILES"),
                          "job coverage: sets VK_DRIVER_FILES to None", "(Requirement 16.5)")

    def test_other_vk_driver_on_the_test_step(self):
        def change(data):
            step = find_step(data, "ubuntu", "tests")
            step["env"] = {"VK_DRIVER_FILES": "/usr/share/vulkan/icd.d/intel_icd.x86_64.json"}
        self.assert_fails(change, 'job ubuntu, step "Run every test": sets VK_DRIVER_FILES to')

    def test_the_other_distributions_lavapipe_manifest(self):
        # Ubuntu 24.04's Mesa has no lvp_icd.x86_64.json, and Fedora's no lvp_icd.json.
        def change(data):
            data["jobs"]["ubuntu"]["env"]["VK_DRIVER_FILES"] = (
                "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json")
            data["jobs"]["fedora"]["env"]["VK_DRIVER_FILES"] = (
                "/usr/share/vulkan/icd.d/lvp_icd.json")
        lines = self.assert_fails(change, "job ubuntu: sets VK_DRIVER_FILES to "
                                  "'/usr/share/vulkan/icd.d/lvp_icd.x86_64.json', not "
                                  "/usr/share/vulkan/icd.d/lvp_icd.json", count=2)
        self.assertIn("job fedora: sets VK_DRIVER_FILES to '/usr/share/vulkan/icd.d/lvp_icd.json', "
                      "not /usr/share/vulkan/icd.d/lvp_icd.x86_64.json (Requirement 16.5)", lines)

    def test_continue_on_error(self):
        def change(data):
            find_step(data, "ubuntu", "tests")["continue-on-error"] = True
        self.assert_fails(change, 'job ubuntu, step "Run every test": sets continue-on-error')

    def test_missing_workflow_check_step(self):
        def change(data):
            steps = data["jobs"]["ubuntu"]["steps"]
            steps.remove(find_step(data, "ubuntu", "Check the workflow structure"))
        self.assert_fails(change, "job ubuntu: has no step that runs the workflow structure check")

    def test_missing_documentation_link_step(self):
        def change(data):
            steps = data["jobs"]["ubuntu"]["steps"]
            steps.remove(find_step(data, "ubuntu", "Check the documentation links"))
        self.assert_fails(change, "job ubuntu: has no step that runs the documentation link check")

    def test_missing_help_option_step(self):
        def change(data):
            steps = data["jobs"]["ubuntu"]["steps"]
            steps.remove(find_step(data, "ubuntu",
                                   "Check the emulator options in the running guide"))
        self.assert_fails(change, "job ubuntu: has no step that runs the running guide's option "
                          "check")

    def test_help_options_against_another_guide(self):
        def change(data):
            replace_in_run(data, "ubuntu", "Check the emulator options in the running guide",
                           "--doc docs/running.md", "--doc README.md")
        self.assert_fails(change, "the running guide's option check")

    def test_help_options_of_an_installed_emulator(self):
        def change(data):
            replace_in_run(data, "ubuntu", "Check the emulator options in the running guide",
                           '"$BUILD_DIR/kyty_emulator"', "/usr/bin/kyty_emulator")
        self.assert_fails(change, "the running guide's option check")

    def test_documentation_steps_only_in_another_job(self):
        def change(data):
            steps = data["jobs"]["ubuntu"]["steps"]
            step = find_step(data, "ubuntu", "Check the documentation links")
            steps.remove(step)
            data["jobs"]["fedora"]["steps"].append(step)
        self.assert_fails(change, "job ubuntu: has no step that runs the documentation link check")


class Conditions(Workflow):
    """No if: may skip a test job, a test step, its timing rerun, its result check, or a required
    step while the job goes on."""

    LINKS = "Check the documentation links"
    OPTIONS = "Check the emulator options in the running guide"
    STRUCTURE = "Check the workflow structure"

    @staticmethod
    def set_if(job, key, value):
        def change(data):
            find_step(data, job, key)["if"] = value
        return change

    def test_documentation_link_check_off(self):
        self.assert_fails(self.set_if("ubuntu", self.LINKS, False),
                          f'job ubuntu, step "{self.LINKS}": if: False can skip the documentation '
                          "link check; such a step may have no if")

    def test_option_check_only_on_main(self):
        self.assert_fails(self.set_if("ubuntu", self.OPTIONS,
                                      "${{ github.ref == 'refs/heads/main' }}"),
                          f'job ubuntu, step "{self.OPTIONS}": if: "${{{{ github.ref == '
                          "'refs/heads/main' }}\" can skip the running guide's option check")

    def test_job_level_if_on_a_test_job(self):
        for job in ("ubuntu", "coverage"):
            with self.subTest(job=job):
                def change(data, job=job):
                    data["jobs"][job]["if"] = "github.event_name == 'push'"
                self.assert_fails(change, f"job {job}: if: \"github.event_name == 'push'\" can "
                                  "skip the whole job; only the release job may have a "
                                  "job-level if")

    def test_failure_on_a_result_check(self):
        self.assert_fails(self.set_if("fedora", "Stage B - check the test results", "failure()"),
                          'job fedora, step "Stage B - check the test results": if: \'failure()\' '
                          "can skip this result check")

    def test_a_later_step_id(self):
        self.assert_fails(
            self.set_if("ubuntu", self.OPTIONS,
                        "${{ !cancelled() && steps.install.outcome == 'success' }}"),
            f'step "{self.OPTIONS}": if: ', "steps.install.outcome",
            "can skip the running guide's option check")

    def test_an_unknown_step_id(self):
        self.assert_fails(
            self.set_if("coverage", "junit",
                        "${{ !cancelled() && steps.compile.outcome == 'success' }}"),
            'job coverage, step "Check the test results": if: ', "steps.compile.outcome",
            "can skip this result check")

    def test_an_earlier_step_with_an_if_of_its_own(self):
        # The option check needs the build step, which a push-only if would skip on pull requests.
        self.assert_fails(self.set_if("ubuntu", "build", "github.event_name == 'push'"),
                          f'job ubuntu, step "{self.OPTIONS}": if: ', "steps.build.outcome",
                          "can skip the running guide's option check")

    def test_a_test_run_and_a_timing_rerun_off(self):
        for job, key, what in (("fedora", "Stage A - run every test", "this test run"),
                               ("ubuntu", "tests", "this test run"),
                               ("ubuntu", "timing-rerun", "this timing rerun")):
            with self.subTest(key=key):
                self.assert_fails(self.set_if(job, key, "${{ false }}"),
                                  f"job {job}, step ", "if: '${{ false }}' can skip " + what)

    def test_the_coverage_target_off_also_skips_its_result_check(self):
        # The result check runs only when the coverage target step succeeded.
        lines = self.assert_fails(self.set_if("coverage", "coverage", "${{ false }}"),
                                  'job coverage, step "Build the coverage target": if: '
                                  "'${{ false }}' can skip this test run", count=2)
        self.assertTrue(lines[1].startswith('job coverage, step "Check the test results": if: '),
                        lines)

    def test_other_conditions_on_a_required_step(self):
        for value in ("${{ false }}", "false", "true", "failure()", "cancelled()",
                      "github.event_name == 'push'", "${{ env.CHECK_DOCS == 'true' }}",
                      "needs.metadata.outputs.official == 'true'", "!cancelled() || failure()",
                      "always() && github.event_name == 'push'",
                      "!cancelled() && steps.build.outcome == 'success' && "
                      "github.event_name == 'push'",
                      "!cancelled() && steps.build.outcome != 'success'",
                      "steps.build.outcome == 'success'"):
            with self.subTest(value=value):
                self.assert_fails(self.set_if("ubuntu", self.STRUCTURE, value),
                                  f'step "{self.STRUCTURE}": if: ',
                                  "can skip the workflow structure check")

    def test_allowed_conditions_pass(self):
        for value in (None, "always()", "${{ always() }}", "!cancelled()", "${{ !cancelled() }}",
                      "success()", "${{ success() }}",
                      "!cancelled() && steps.build.outcome == 'success'",
                      "${{ !cancelled()&&steps.tests.outcome=='success' }}"):
            for key in (self.LINKS, "timing-rerun", "Check the test results"):
                with self.subTest(value=value, key=key):
                    self.assert_passes(self.mutated(self.set_if("ubuntu", key, value)))

    def test_the_committed_conditions_pass(self):
        # The committed doc steps and coverage steps use !cancelled() and steps.<id>.outcome.
        self.assertEqual(find_step(self.data, "ubuntu", self.LINKS)["if"], "${{ !cancelled() }}")
        self.assertEqual(find_step(self.data, "coverage", "junit")["if"],
                         "${{ !cancelled() && steps.coverage.outcome == 'success' }}")
        self.assert_passes(self.text)


class CoreDumpPrints(Workflow):
    """Each test job prints core_pattern and ulimit -c, in a step that no if can skip."""

    STEP = "Show the tool versions"
    # The prints are items of the environment fingerprint, which annotate.py prints to the log.
    PRINTS = (('item core_pattern "$(cat /proc/sys/kernel/core_pattern)"\n',
               "the core dump pattern print"),
              ('item "ulimit -c" "$(ulimit -c)"\n', "the core size limit print"))

    def test_the_committed_jobs_print_both(self):
        for job in ("ubuntu", "fedora", "coverage"):
            for line, _ in self.PRINTS:
                with self.subTest(job=job, line=line):
                    self.assertIn(line, find_step(self.data, job, self.STEP)["run"])

    def test_a_job_without_a_print(self):
        for job in ("ubuntu", "fedora", "coverage"):
            for line, what in self.PRINTS:
                with self.subTest(job=job, what=what):
                    def change(data, job=job, line=line):
                        replace_in_run(data, job, self.STEP, line, "")
                    self.assert_fails(change, f"job {job}: has no step that runs {what}")

    def test_both_prints_only_in_another_job(self):
        def change(data):
            step = find_step(data, "coverage", self.STEP)
            for line, _ in self.PRINTS:
                step["run"] = step["run"].replace(line, "")
            find_step(data, "ubuntu", self.STEP)["run"] += "".join(line for line, _ in self.PRINTS)
        lines = self.assert_fails(change, "job coverage: has no step that runs the core dump "
                                  "pattern print", count=2)
        self.assertIn("job coverage: has no step that runs the core size limit print", lines)

    def test_a_label_or_comment_without_the_command(self):
        # Only the command counts: a label or a comment that names it is not a print.
        cases = ((0, 'echo "core_pattern: see /proc/sys/kernel/core_pattern"\n'),
                 (0, "# cat /proc/sys/kernel/core_pattern\n"),
                 (1, 'echo "ulimit -c: unknown"\n'),
                 (1, "# echo $(ulimit -c)\n"),
                 (1, "ulimit -c 0\n"))
        for index, new in cases:
            line, what = self.PRINTS[index]
            with self.subTest(new=new):
                def change(data, line=line, new=new):
                    replace_in_run(data, "fedora", self.STEP, line, new)
                self.assert_fails(change, f"job fedora: has no step that runs {what}")

    def test_the_plain_commands_pass(self):
        cases = ((0, "cat /proc/sys/kernel/core_pattern\n"),
                 (1, "ulimit -c\n"),
                 (1, 'echo "core size limit $( ulimit -c ) blocks"\n'))
        for index, new in cases:
            line, _ = self.PRINTS[index]
            with self.subTest(new=new):
                self.assert_passes(self.mutated(
                    lambda data, line=line, new=new: replace_in_run(data, "fedora", self.STEP,
                                                                    line, new)))

    def test_a_print_step_only_on_main(self):
        # One step holds both prints, so its if is reported once.
        def change(data):
            find_step(data, "coverage", self.STEP)["if"] = "${{ github.ref == 'refs/heads/main' }}"
        self.assert_fails(change, f'job coverage, step "{self.STEP}": if: "${{{{ github.ref == '
                          "'refs/heads/main' }}\" can skip the core dump pattern print; such a "
                          "step may have no if")


class EnvironmentFingerprint(Workflow):
    """The ubuntu, fedora, coverage, and flatpak jobs put their environment in the job summary and
    a notice (tools/ci/annotate.py environment), in a step that no if can skip."""

    JOBS = {"ubuntu": ["Show the tool versions"],
            "fedora": ["Show the tool versions", "Show the stage B versions"],
            "coverage": ["Show the tool versions"],
            "flatpak": ["Show the image's tools"]}
    MODE = "tools/ci/annotate.py environment"

    def test_the_committed_jobs_have_it(self):
        for job, keys in self.JOBS.items():
            for key in keys:
                with self.subTest(job=job, key=key):
                    self.assertIn(self.MODE, find_step(self.data, job, key)["run"])

    def test_a_job_without_it(self):
        for job, keys in self.JOBS.items():
            with self.subTest(job=job):
                def change(data, job=job, keys=keys):
                    for key in keys:
                        replace_in_run(data, job, key, self.MODE, "tools/ci/annotate.py log")
                self.assert_fails(change, f"job {job}: has no step that runs the environment "
                                  "fingerprint")

    def test_a_fingerprint_step_that_can_be_skipped(self):
        def change(data):
            find_step(data, "flatpak", "Show the image's tools")["if"] = "failure()"
        self.assert_fails(change, "job flatpak, step \"Show the image's tools\": if: 'failure()' "
                          "can skip the environment fingerprint; such a step may have no if")

    def test_another_script_does_not_count(self):
        def change(data):
            replace_in_run(data, "coverage", "Show the tool versions", self.MODE,
                           "tools/ci/annotate.py.bak environment")
        self.assert_fails(change, "job coverage: has no step that runs the environment "
                          "fingerprint")


class Ccache(Workflow):
    """The ubuntu, fedora, and coverage jobs restore and save _Build/ccache under a key of their
    own, point CCACHE_DIR at it, and never turn ccache off with an empty compiler launcher."""

    JOBS = ("ubuntu", "fedora", "coverage")
    CACHE = "Restore and save ccache"
    DIRECTORY = "Set the ccache directory"
    CONFIGURE = "Configure with coverage"
    COVERAGE_ON = "-DKYTY_ENABLE_COVERAGE=ON"

    def test_the_committed_jobs_cache(self):
        for job in self.JOBS:
            with self.subTest(job=job):
                step = find_step(self.data, job, self.CACHE)
                self.assertTrue(step["uses"].startswith("actions/cache@"), step)
                self.assertEqual(step["with"]["path"], "_Build/ccache")
                self.assertEqual(step["with"]["key"], f"ccache-{job}-${{{{ github.sha }}}}")
                self.assertEqual(step["with"]["restore-keys"], f"ccache-{job}-")

    def test_a_job_without_its_cache_step(self):
        for job in self.JOBS:
            with self.subTest(job=job):
                def change(data, job=job):
                    data["jobs"][job]["steps"].remove(find_step(data, job, self.CACHE))
                self.assert_fails(change, f"job {job}: has no step that restores and saves the "
                                  "compiler cache (actions/cache with path: _Build/ccache and a "
                                  f"key that starts with ccache-{job}-)")

    def test_another_jobs_key_or_another_path(self):
        cases = {"the ubuntu key": ("key", "ccache-ubuntu-${{ github.sha }}"),
                 "a shared key": ("key", "ccache-${{ github.sha }}"),
                 "another path": ("path", "~/.cache/ccache")}
        for name, (key, value) in cases.items():
            with self.subTest(name):
                def change(data, key=key, value=value):
                    find_step(data, "coverage", self.CACHE)["with"][key] = value
                self.assert_fails(change, "job coverage: has no step that restores and saves "
                                  "the compiler cache")

    def test_a_restore_only_action(self):
        # actions/cache/restore never saves, so the next run would start cold again.
        def change(data):
            find_step(data, "coverage", self.CACHE)["uses"] = f"actions/cache/restore@{FAKE_SHA}"
        self.assert_fails(change, "job coverage: has no step that restores and saves the "
                          "compiler cache")

    def test_a_job_without_the_ccache_directory(self):
        for job in self.JOBS:
            with self.subTest(job=job):
                def change(data, job=job):
                    data["jobs"][job]["steps"].remove(find_step(data, job, self.DIRECTORY))
                self.assert_fails(change, f"job {job}: has no step that writes "
                                  "CCACHE_DIR=$GITHUB_WORKSPACE/_Build/ccache to $GITHUB_ENV")

    def test_an_empty_launcher(self):
        for option in ("-DCMAKE_C_COMPILER_LAUNCHER=", '-DCMAKE_CXX_COMPILER_LAUNCHER=""',
                       "-DCMAKE_CXX_COMPILER_LAUNCHER=''", "-DCMAKE_C_COMPILER_LAUNCHER:STRING=",
                       "-D CMAKE_CXX_COMPILER_LAUNCHER="):
            with self.subTest(option=option):
                def change(data, option=option):
                    replace_in_run(data, "coverage", self.CONFIGURE, self.COVERAGE_ON,
                                   f"{self.COVERAGE_ON} {option}")
                lines = self.assert_fails(change, f'job coverage, step "{self.CONFIGURE}": sets ',
                                          "an empty compiler launcher, which turns ccache off; "
                                          "the coverage job builds with ccache")
                self.assertIn("COMPILER_LAUNCHER", lines[0])

    def test_both_empty_launchers_in_one_line(self):
        def change(data):
            replace_in_run(data, "coverage", self.CONFIGURE, self.COVERAGE_ON,
                           f"{self.COVERAGE_ON} -DCMAKE_C_COMPILER_LAUNCHER= "
                           "-DCMAKE_CXX_COMPILER_LAUNCHER=")
        self.assert_fails(change, "sets -DCMAKE_C_COMPILER_LAUNCHER= "
                          "-DCMAKE_CXX_COMPILER_LAUNCHER=, an empty compiler launcher")

    def test_a_launcher_or_a_comment_passes(self):
        for option in ("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
                       "-DCMAKE_C_COMPILER_LAUNCHER=/usr/bin/ccache"):
            with self.subTest(option=option):
                def change(data, option=option):
                    replace_in_run(data, "coverage", self.CONFIGURE, self.COVERAGE_ON,
                                   f"{self.COVERAGE_ON} {option}")
                self.assert_passes(self.mutated(change))

        def comment(data):
            step = find_step(data, "coverage", self.CONFIGURE)
            step["run"] = "# -DCMAKE_C_COMPILER_LAUNCHER= would turn ccache off\n" + step["run"]
        self.assert_passes(self.mutated(comment))


class Limits(Workflow):
    """Every job has a time limit, no step's limit is above its job's, and every step that
    downloads packages has its own and makes apt or dnf retry failed downloads."""

    JOB_MINUTES = {"metadata": 10, "ubuntu": 120, "fedora": 120, "coverage": 120, "flatpak": 60,
                   "release": 60}
    INSTALLS = (("ubuntu", "Install packages", "apt-get update"),
                ("coverage", "Install packages", "apt-get update"),
                ("fedora", "Install git and Python", "dnf install"),
                ("fedora", "Install the stage A packages (no GTK)", "dnf install"),
                ("fedora", "Install the stage B packages", "dnf install"),
                ("release", "Check the bundle's metainfo release entry", "apt-get update"))
    APT = "apt-get -o Acquire::Retries=5 "
    DNF = "--setopt=retries=10 "
    STAGE_B = "Install the stage B packages"
    RELEASE = "Check the bundle's metainfo release entry"

    def test_the_committed_limits(self):
        for job, minutes in self.JOB_MINUTES.items():
            with self.subTest(job=job):
                self.assertEqual(self.data["jobs"][job]["timeout-minutes"], minutes)
        for job, key, _ in self.INSTALLS:
            with self.subTest(job=job, key=key):
                self.assertEqual(find_step(self.data, job, key)["timeout-minutes"], 45)

    def test_a_job_without_timeout_minutes(self):
        for job in self.JOB_MINUTES:
            with self.subTest(job=job):
                self.assert_fails(lambda data, job=job: data["jobs"][job].pop("timeout-minutes"),
                                  f"job {job}: has no timeout-minutes; each job needs one from 1 "
                                  "to 120, so that a hung step cannot run for GitHub's default "
                                  "of 360 minutes")

    def test_a_job_timeout_out_of_range_or_not_a_number(self):
        for value in (360, 121, 0, -5, "60", "${{ 60 }}", True, 60.5):
            with self.subTest(value=value):
                def change(data, value=value):
                    data["jobs"]["ubuntu"]["timeout-minutes"] = value
                self.assert_fails(change, f"job ubuntu: timeout-minutes is {value!r}, not a whole "
                                  "number from 1 to 120")

    def test_no_committed_step_limit_is_above_its_job_limit(self):
        for job_id, job in self.data["jobs"].items():
            for step in job["steps"]:
                if "timeout-minutes" in step:
                    with self.subTest(job=job_id, step=step.get("name")):
                        self.assertLessEqual(step["timeout-minutes"], job["timeout-minutes"])

    def test_a_step_limit_above_its_job_limit(self):
        cases = (("release", 30, self.RELEASE, None, 45),
                 ("release", 44, self.RELEASE, None, 45),
                 ("ubuntu", 120, "Lint the workflow", 121, 121),
                 ("metadata", 10, "Compute the tag", 11, 11))
        for job, job_minutes, key, step_minutes, shown in cases:
            with self.subTest(job=job, job_minutes=job_minutes, step_minutes=step_minutes):
                def change(data, job=job, job_minutes=job_minutes, key=key,
                           step_minutes=step_minutes):
                    data["jobs"][job]["timeout-minutes"] = job_minutes
                    if step_minutes is not None:
                        find_step(data, job, key)["timeout-minutes"] = step_minutes
                self.assert_fails(change, f'job {job}, step "{key}": timeout-minutes is {shown}, '
                                  f"above the job's {job_minutes}; the job's limit ends the step "
                                  "first, so the step's limit has no effect")

    def test_a_step_limit_equal_to_or_below_its_job_limit_passes(self):
        for minutes in (45, 46, 120):
            with self.subTest(minutes=minutes):
                def change(data, minutes=minutes):
                    data["jobs"]["release"]["timeout-minutes"] = minutes
                self.assert_passes(self.mutated(change))

        def an_expression(data):
            # Only whole numbers are compared: GitHub reads an expression at run time.
            find_step(data, "ubuntu", "Lint the workflow")["timeout-minutes"] = "${{ 500 }}"
        self.assert_passes(self.mutated(an_expression))

    def test_an_invalid_job_limit_is_reported_once(self):
        for value in (0, 20.5, "30"):
            with self.subTest(value=value):
                def change(data, value=value):
                    data["jobs"]["release"]["timeout-minutes"] = value
                self.assert_fails(change, f"job release: timeout-minutes is {value!r}, not a "
                                  "whole number from 1 to 120")

    def test_an_install_step_without_its_timeout(self):
        for job, key, command in self.INSTALLS:
            with self.subTest(job=job, key=key):
                def change(data, job=job, key=key):
                    find_step(data, job, key).pop("timeout-minutes")
                self.assert_fails(change, f'job {job}, step "{key}": downloads packages '
                                  f"({command}) with no timeout-minutes; such a step needs one "
                                  "from 1 to 60")

    def test_an_install_timeout_out_of_range(self):
        for value in (61, 120, 0, "45"):
            with self.subTest(value=value):
                def change(data, value=value):
                    find_step(data, "fedora", self.STAGE_B)["timeout-minutes"] = value
                self.assert_fails(change, f'job fedora, step "{self.STAGE_B}": timeout-minutes is '
                                  f"{value!r}, not a whole number from 1 to 60; the step "
                                  "downloads packages (dnf install)")

    def test_apt_without_retries(self):
        for job, key, old, new, command in (
                ("ubuntu", "Install packages", f"sudo {self.APT}update", "sudo apt-get update",
                 "apt-get update"),
                ("coverage", "Install packages", f"sudo {self.APT}install",
                 "sudo apt-get install", "apt-get install"),
                ("release", self.RELEASE, f"sudo {self.APT}install", "sudo apt install",
                 "apt install")):
            with self.subTest(job=job, command=command):
                def change(data, job=job, key=key, old=old, new=new):
                    replace_in_run(data, job, key, old, new)
                self.assert_fails(change, f'job {job}, step "{key}": runs {command} without -o '
                                  "Acquire::Retries=<n> (n at least 1)")

    def test_dnf_without_retries(self):
        for key in ("Install git and Python", "Install the stage A packages (no GTK)",
                    self.STAGE_B):
            with self.subTest(key=key):
                def change(data, key=key):
                    replace_in_run(data, "fedora", key, self.DNF, "")
                self.assert_fails(change, f'job fedora, step "{key}": runs dnf install without '
                                  "--setopt=retries=<n> (n at least 1)")

    def test_zero_or_no_number_of_retries(self):
        cases = (("release", self.RELEASE, "Acquire::Retries=5 update", "Acquire::Retries=0 update",
                  "apt-get update"),
                 ("release", self.RELEASE, "Acquire::Retries=5 update", "Acquire::Retries= update",
                  "apt-get update"),
                 ("fedora", self.STAGE_B, self.DNF, "--setopt=retries=0 ", "dnf install"),
                 ("fedora", self.STAGE_B, self.DNF, "--setopt=timeout=60 ", "dnf install"))
        for job, key, old, new, command in cases:
            with self.subTest(new=new):
                def change(data, job=job, key=key, old=old, new=new):
                    replace_in_run(data, job, key, old, new)
                self.assert_fails(change, f'job {job}, step "{key}": runs {command} without ')

    def test_other_forms_of_the_retry_options_pass(self):
        cases = (("release", self.RELEASE, self.APT, "apt-get -oAcquire::Retries=3 "),
                 ("release", self.RELEASE, self.APT, "apt-get --option Acquire::Retries=3 "),
                 ("release", self.RELEASE, self.APT, "apt-get --option=Acquire::Retries=3 "),
                 ("release", self.RELEASE, self.APT, "apt-get -o acquire::retries=3 "),
                 ("fedora", self.STAGE_B, self.DNF, "--setopt retries=3 "))
        for job, key, old, new in cases:
            with self.subTest(new=new):
                self.assert_passes(self.mutated(
                    lambda data, job=job, key=key, old=old, new=new: replace_in_run(
                        data, job, key, old, new)))

    def test_a_new_install_step_needs_both(self):
        def change(data):
            data["jobs"]["metadata"]["steps"].append(
                {"name": "Install jq", "run": "sudo -E apt install --yes jq\n"})
        lines = self.assert_fails(change, 'job metadata, step "Install jq": downloads packages '
                                  "(apt install) with no timeout-minutes", count=2)
        self.assertIn('job metadata, step "Install jq": runs apt install without -o '
                      "Acquire::Retries=<n> (n at least 1)", lines)

    def test_commands_that_download_nothing_pass(self):
        def change(data):
            data["jobs"]["metadata"]["steps"].append(
                {"name": "Show the package tools",
                 "run": "apt-get --version\ndnf --version\necho apt-get install x\n"
                        "# dnf install y\napt-cache policy weston\n"})
        self.assert_passes(self.mutated(change))


class Pins(Workflow):
    """Every action is pinned to a full commit SHA with its version as a comment, no checkout
    keeps the token, and Dependabot updates the actions with pull requests to dev."""

    CHECKOUTS = ("metadata", "ubuntu", "fedora", "coverage", "flatpak", "release")
    DEPENDABOT_TEXT = (REPO / DEPENDABOT).read_text(encoding="utf-8")

    def committed_line(self, action):
        """The first uses: line of the committed workflow for the action, and its value."""
        for line in self.text.splitlines():
            match = USES_LINE.match(line)
            if match and match.group(2).split("@", 1)[0] == action:
                return line, match
        raise AssertionError(f"no uses: line for {action}")

    def test_the_committed_actions_are_pinned_with_versions(self):
        uses = [step["uses"] for job in self.data["jobs"].values() for step in job["steps"]
                if "uses" in step]
        self.assertTrue(uses)
        for value in uses:
            with self.subTest(uses=value):
                self.assertRegex(value, r"^[^@\s]+@[0-9a-f]{40}$")
                self.assertRegex(self.comments.get(value, ""), r"^ # v[0-9]+(\.[0-9]+)*$")

    def test_the_committed_checkouts_keep_no_token(self):
        for job in self.CHECKOUTS:
            with self.subTest(job=job):
                step = find_step(self.data, job, "Check out")
                self.assertTrue(step["uses"].startswith("actions/checkout@"), step)
                self.assertIs(step["with"]["persist-credentials"], False)

    def test_a_tag_branch_or_short_sha(self):
        for job, key, value in (("metadata", "Check out", "actions/checkout@v7"),
                                ("ubuntu", "Restore and save ccache", "actions/cache@main"),
                                ("fedora", "Upload the test results",
                                 "actions/upload-artifact@cf430e0"),
                                ("release", "Download the bundle",
                                 "actions/download-artifact@" + FAKE_SHA.upper()),
                                ("flatpak", "build", "flatpak/flatpak-github-actions/"
                                 "flatpak-builder@" + FAKE_SHA + "0"),
                                ("ubuntu", "Check out", "actions/checkout"),
                                ("coverage", "Check out", "docker://alpine:3.20")):
            with self.subTest(uses=value):
                def change(data, job=job, key=key, value=value):
                    find_step(data, job, key)["uses"] = value
                self.assert_fails(change, f'job {job}, step "', f"uses {value}, which is pinned "
                                  "by tag or branch, not by a full commit SHA")

    def test_a_pin_without_its_version_comment(self):
        line, match = self.committed_line("actions/checkout")
        for comment in ("", " # latest", " # 7.0.1", " # v7.0.1 and more", " # tag=v7.0.1"):
            with self.subTest(comment=comment):
                text = self.text.replace(line, match.group(1) + match.group(2) + comment, 1)
                status, lines, stderr = self.run_check(text)
                self.assertEqual(status, 1, (lines, stderr))
                self.assertEqual(lines, ['job metadata, step "Check out": uses '
                                         f"{match.group(2)}, which has no version comment "
                                         "(# v<version> after the SHA on the uses: line)",
                                         "FAILED: workflow_structure: 1 problem(s)"])

    def test_a_comment_on_another_line_does_not_count(self):
        line, match = self.committed_line("actions/cache")
        indent = match.group(1)[:len(match.group(1)) - len(match.group(1).lstrip())]
        text = self.text.replace(line, f"{indent}{match.group(3).strip()}\n"
                                 f"{match.group(1)}{match.group(2)}", 1)
        status, lines, _ = self.run_check(text)
        self.assertEqual(status, 1, lines)
        self.assertIn('job ubuntu, step "Restore and save ccache": uses '
                      f"{match.group(2)}, which has no version comment", lines[0])

    def test_other_comment_and_quoting_forms_pass(self):
        line, match = self.committed_line("actions/upload-artifact")
        version = match.group(3).split("#", 1)[1].strip()
        for new in (f'{match.group(1)}"{match.group(2)}" # {version}',
                    f"{match.group(1)}'{match.group(2)}'  #{version}",
                    f"{match.group(1)}{match.group(2)} # v7"):
            with self.subTest(new=new):
                self.assert_passes(self.text.replace(line, new, 1))

    def test_a_local_action_needs_no_pin(self):
        def change(data):
            data["jobs"]["metadata"]["steps"].append({"name": "Local action",
                                                      "uses": "./.github/actions/local"})
        self.assert_passes(self.mutated(change))

    def test_a_checkout_that_keeps_the_token(self):
        for job in self.CHECKOUTS:
            for value in (None, True, "true"):
                with self.subTest(job=job, value=value):
                    def change(data, job=job, value=value):
                        options = find_step(data, job, "Check out")["with"]
                        if value is None:
                            options.pop("persist-credentials")
                        else:
                            options["persist-credentials"] = value
                    self.assert_fails(change, f'job {job}, step "Check out": checks out with '
                                      f"persist-credentials: {value!r}, not false; the token "
                                      "would stay in .git/config")

    def test_persist_credentials_as_a_string_passes(self):
        def change(data):
            find_step(data, "flatpak", "Check out")["with"]["persist-credentials"] = "false"
        self.assert_passes(self.mutated(change))

    def run_with_dependabot(self, text):
        """The committed workflow checked against a --repo checkout with the committed Ubuntu
        list and text as its .github/dependabot.yml (none when text is None)."""
        repo = Path(tempfile.mkdtemp(dir=self.temp.name))
        (repo / "tools" / "checks").mkdir(parents=True)
        (repo / ".github").mkdir()
        shutil.copyfile(REPO / UBUNTU_LIST, repo / UBUNTU_LIST)
        if text is not None:
            (repo / DEPENDABOT).write_text(text, encoding="utf-8")
        self.path.write_text(self.text, encoding="utf-8")
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = workflow_check.main(["--workflow", str(self.path), "--repo", str(repo)])
        return status, stdout.getvalue().splitlines()

    def test_the_committed_dependabot_configuration(self):
        config = yaml.safe_load(self.DEPENDABOT_TEXT)
        self.assertEqual(config["version"], 2)
        self.assertEqual(config["updates"], [{
            "package-ecosystem": "github-actions", "directory": "/", "target-branch": "dev",
            "schedule": {"interval": "weekly"}, "groups": {"github-actions": {"patterns": ["*"]}},
            "commit-message": {"prefix": "ci"}}])
        status, lines = self.run_with_dependabot(self.DEPENDABOT_TEXT)
        self.assertEqual(status, 0, lines)

    def assert_dependabot_fails(self, text, *fragments):
        status, lines = self.run_with_dependabot(text)
        self.assertEqual(status, 1, lines)
        self.assertEqual(lines[-1], "FAILED: workflow_structure: 1 problem(s)", lines)
        self.assertTrue(lines[0].startswith("dependabot: "), lines)
        for fragment in fragments:
            self.assertIn(fragment, lines[0])

    def test_a_missing_or_broken_dependabot_file(self):
        for text, fragment in ((None, "cannot read .github/dependabot.yml"),
                               ("updates: [unclosed\n", "cannot read .github/dependabot.yml"),
                               ("- version: 2\n", "has version: None, not 2"),
                               (self.DEPENDABOT_TEXT.replace("version: 2", "version: 1"),
                                "has version: 1, not 2")):
            with self.subTest(text=text):
                self.assert_dependabot_fails(text, fragment)

    def test_no_github_actions_update(self):
        text = self.DEPENDABOT_TEXT.replace("package-ecosystem: github-actions",
                                            "package-ecosystem: pip")
        self.assert_dependabot_fails(text, "has no update with package-ecosystem: "
                                     "github-actions")

    def test_a_github_actions_update_for_another_branch_or_without_one(self):
        cases = {"main": ("target-branch: dev", "target-branch: main",
                          "target-branch: 'main', not dev (without it, the pull requests go to "
                          "the default branch, main)"),
                 "none": ("    target-branch: dev\n", "", "target-branch: None, not dev"),
                 "directory": ("directory: /\n", "directory: /.github\n",
                               "directory: '/.github', not /"),
                 "schedule": ("      interval: weekly\n", "      day: monday\n",
                              "schedule.interval: None")}
        for name, (old, new, fragment) in cases.items():
            with self.subTest(name):
                self.assertIn(old, self.DEPENDABOT_TEXT)
                self.assert_dependabot_fails(self.DEPENDABOT_TEXT.replace(old, new, 1),
                                             "the github-actions update has ", fragment)

    def test_one_valid_github_actions_update_is_enough(self):
        text = self.DEPENDABOT_TEXT + ("  - package-ecosystem: github-actions\n"
                                       "    directory: /\n"
                                       "    schedule:\n"
                                       "      interval: monthly\n")
        status, lines = self.run_with_dependabot(text)
        self.assertEqual(status, 0, lines)


class Actionlint(Workflow):
    """The ubuntu job runs a pinned actionlint release on every workflow file after checking its
    SHA-256, in a step that no if can skip."""

    STEP = "Lint the workflow"
    CHECK = 'echo "$ACTIONLINT_SHA256  $RUNNER_TEMP/$archive" | sha256sum -c -\n'
    # No file argument: actionlint lints every workflow file in .github/workflows/.
    RUN = '"$RUNNER_TEMP/actionlint" -color\n'
    MISSING = 'job ubuntu: has no step that runs the workflow lint ("$RUNNER_TEMP/actionlint") '

    def test_the_committed_step(self):
        step = find_step(self.data, "ubuntu", self.STEP)
        self.assertRegex(step["env"]["ACTIONLINT_VERSION"], r"^[0-9]+\.[0-9]+\.[0-9]+$")
        self.assertRegex(step["env"]["ACTIONLINT_SHA256"], r"^[0-9a-f]{64}$")
        self.assertIn(self.CHECK, step["run"])
        self.assertIn(self.RUN, step["run"])
        self.assertLess(step["run"].index(self.CHECK), step["run"].index(self.RUN))

    def test_no_such_step(self):
        cases = {"removed": lambda data: data["jobs"]["ubuntu"]["steps"].remove(
                     find_step(data, "ubuntu", self.STEP)),
                 "not run": lambda data: replace_in_run(data, "ubuntu", self.STEP, self.RUN,
                                                        "ls \"$RUNNER_TEMP\"\n"),
                 "a comment": lambda data: replace_in_run(data, "ubuntu", self.STEP, self.RUN,
                                                          f"# {self.RUN}"),
                 "only in fedora": self.only_in_fedora}
        for name, change in cases.items():
            with self.subTest(name):
                self.assert_fails(change, self.MISSING + "on the workflow")

    def only_in_fedora(self, data):
        step = find_step(data, "ubuntu", self.STEP)
        data["jobs"]["ubuntu"]["steps"].remove(step)
        data["jobs"]["fedora"]["steps"].append(step)

    def test_no_check_before_the_run(self):
        cases = {"none": (self.CHECK, ""),
                 "after the run": (self.CHECK + "tar", "tar"),
                 "another file": (self.CHECK, "sha256sum -c actionlint_checksums.txt\n"),
                 "no check option": (self.CHECK, self.CHECK.replace(" -c -", " -")),
                 "a comment": (self.CHECK, f"# {self.CHECK}")}
        for name, (old, new) in cases.items():
            with self.subTest(name):
                def change(data, name=name, old=old, new=new):
                    replace_in_run(data, "ubuntu", self.STEP, old, new)
                    if name == "after the run":
                        find_step(data, "ubuntu", self.STEP)["run"] += self.CHECK
                self.assert_fails(change, f'job ubuntu, step "{self.STEP}": runs actionlint '
                                  "without checking the download first; a line before the run "
                                  "needs sha256sum -c with $ACTIONLINT_SHA256")

    def test_a_version_or_hash_that_pins_nothing(self):
        cases = (("ACTIONLINT_VERSION", "latest", "ACTIONLINT_VERSION is 'latest', not a release "
                  "version such as 1.7.12"),
                 ("ACTIONLINT_VERSION", 1.7, "ACTIONLINT_VERSION is 1.7, not a release version"),
                 ("ACTIONLINT_VERSION", None, "ACTIONLINT_VERSION is None, not a release"),
                 ("ACTIONLINT_SHA256", "8aca8db9", "ACTIONLINT_SHA256 is '8aca8db9', not 64 "
                  "lowercase hex digits"),
                 ("ACTIONLINT_SHA256", "A" * 64, "ACTIONLINT_SHA256 is 'AAAA"),
                 ("ACTIONLINT_SHA256", None, "ACTIONLINT_SHA256 is None, not 64"))
        for name, value, fragment in cases:
            with self.subTest(name=name, value=value):
                def change(data, name=name, value=value):
                    env = find_step(data, "ubuntu", self.STEP)["env"]
                    if value is None:
                        env.pop(name)
                    else:
                        env[name] = value
                self.assert_fails(change, f'job ubuntu, step "{self.STEP}": {fragment}')

    def test_a_step_that_can_be_skipped(self):
        def change(data):
            find_step(data, "ubuntu", self.STEP)["if"] = "failure()"
        self.assert_fails(change, f'job ubuntu, step "{self.STEP}": if: \'failure()\' can skip '
                          'the workflow lint ("$RUNNER_TEMP/actionlint"); such a step may have '
                          "no if")

    def test_a_run_on_named_files(self):
        cases = {"one file": ("-color .github/workflows/build.yml",
                              ".github/workflows/build.yml"),
                 "a glob": ("-color .github/workflows/*.yml", ".github/workflows/*.yml"),
                 "standard input": ("-color - < .github/workflows/build.yml",
                                    "- < .github/workflows/build.yml"),
                 "after --": ("-color -- build.yml", "build.yml"),
                 "after an option's value": ("-ignore SC2086 -format '{{json .}}' build.yml",
                                             "build.yml"),
                 "before an option": ("build.yml -color", "build.yml -color")}
        for name, (arguments, shown) in cases.items():
            with self.subTest(name):
                def change(data, arguments=arguments):
                    replace_in_run(data, "ubuntu", self.STEP, self.RUN,
                                   f'"$RUNNER_TEMP/actionlint" {arguments}\n')
                self.assert_fails(change, f'job ubuntu, step "{self.STEP}": runs actionlint on '
                                  f"{shown} only; with options only, it lints every workflow "
                                  "file in .github/workflows/")

    def test_other_forms_pass(self):
        cases = ((self.RUN, '"${RUNNER_TEMP}/actionlint"\n'),
                 (self.RUN, '"$RUNNER_TEMP/actionlint" -color -oneline -ignore SC2086 '
                  "-format '{{json .}}' -config-file=.github/actionlint.yaml\n"),
                 (self.RUN, '"$RUNNER_TEMP/actionlint" -color --\n'),
                 (" -c -", " --check -"),
                 (" -c -", " --strict -c -"),
                 ('echo "$ACTIONLINT_SHA256 ', 'echo "${ACTIONLINT_SHA256} '))
        for old, new in cases:
            with self.subTest(new=new):
                self.assert_passes(self.mutated(
                    lambda data, old=old, new=new: replace_in_run(data, "ubuntu", self.STEP,
                                                                  old, new)))

        def from_the_job(data):
            step = find_step(data, "ubuntu", self.STEP)
            data["jobs"]["ubuntu"]["env"].update(step.pop("env"))
        self.assert_passes(self.mutated(from_the_job))


class Release(Workflow):
    def test_release_if_without_the_repository_condition(self):
        def change(data):
            data["jobs"]["release"]["if"] = ("github.event_name == 'push' && "
                                              "github.ref == 'refs/heads/main'")
        self.assert_fails(change, "job release: if: ", "is not exactly",
                          "(Requirements 16.10, 16.13)")

    def test_release_if_with_always(self):
        def change(data):
            data["jobs"]["release"]["if"] = f"always() && {RELEASE_IF}"
        self.assert_fails(change, 'job release: if: "always() && ')

    def test_release_if_with_or(self):
        def change(data):
            data["jobs"]["release"]["if"] = (f"{RELEASE_IF} || "
                                              "github.event_name == 'workflow_dispatch'")
        self.assert_fails(change, "job release: if: ")

    def test_release_if_grouped_with_success_passes(self):
        def change(data):
            data["jobs"]["release"]["if"] = (
                "${{ success() && (github.event_name == 'push' && "
                "github.ref=='refs/heads/main') && github.repository == 'MrG00gle/KytyPS5-GTK' }}")
        self.assert_passes(self.mutated(change))

    def test_needs_missing_flatpak(self):
        self.assert_fails(lambda data: data["jobs"]["release"]["needs"].remove("flatpak"),
                          "job release: needs lacks flatpak")

    def test_release_without_write_permission(self):
        self.assert_fails(lambda data: data["jobs"]["release"].pop("permissions"),
                          "job release: permissions is None, not contents: write")

    def test_write_permission_on_another_job(self):
        self.assert_fails(
            lambda data: data["jobs"]["flatpak"].update(permissions={"contents": "write"}),
            "job flatpak: grants write access")

    def test_top_level_write_permission(self):
        self.assert_fails(lambda data: data.update(permissions="write-all"),
                          "workflow: permissions is 'write-all', not contents: read")


class ReleaseNotes(Workflow):
    """The release job writes the release's text with tools/ci/release_notes.py from the whole
    history, and publishes the release with that text (docs/packaging.md, "Release notes")."""

    NOTES = "Write the release notes"
    NOTES_OPTION = "--notes-file _Build/release-notes.md "

    def test_the_committed_release_job(self):
        checkout = find_step(self.data, "release", "Check out")
        self.assertEqual(checkout["with"]["fetch-depth"], 0)
        self.assertIn("tools/ci/release_notes.py ", find_step(self.data, "release",
                                                              self.NOTES)["run"])
        self.assertIn(self.NOTES_OPTION, find_step(self.data, "release", "Publish")["run"])
        self.assertLess(step_index(self.data, "release", self.NOTES),
                        step_index(self.data, "release", "Publish"))

    def test_no_release_notes_step(self):
        def change(data):
            data["jobs"]["release"]["steps"].remove(find_step(data, "release", self.NOTES))
        self.assert_fails(change, "job release: has no step that runs the release notes")

    def test_another_script_does_not_count(self):
        def change(data):
            replace_in_run(data, "release", self.NOTES, "tools/ci/release_notes.py ",
                           "tools/ci/release_notes.py.bak ")
        self.assert_fails(change, "job release: has no step that runs the release notes")

    def test_publish_without_the_notes(self):
        def change(data):
            replace_in_run(data, "release", "Publish", self.NOTES_OPTION, "")
        self.assert_fails(change, "job release: has no step that runs the publish with those "
                          "notes")

    def test_a_notes_step_that_can_be_skipped(self):
        def change(data):
            find_step(data, "release", self.NOTES)["if"] = "failure()"
        self.assert_fails(change, f'job release, step "{self.NOTES}": if: \'failure()\' can skip '
                          "the release notes; such a step may have no if")

    def test_a_checkout_without_the_whole_history(self):
        for depth in (None, 1, 2, "1", False):
            with self.subTest(depth=depth):
                def change(data, depth=depth):
                    options = find_step(data, "release", "Check out")["with"]
                    if depth is None:
                        options.pop("fetch-depth")
                    else:
                        options["fetch-depth"] = depth
                self.assert_fails(change, f'job release, step "Check out": checks out with '
                                  f"fetch-depth: {depth!r}, not 0; the release notes need the "
                                  "history and the tags since the previous release")

    def test_no_checkout_in_the_release_job(self):
        def change(data):
            data["jobs"]["release"]["steps"].remove(find_step(data, "release", "Check out"))
        self.assert_fails(change, "job release: has no actions/checkout step; the release notes "
                          "need the history and the tags (fetch-depth: 0)")

    def test_fetch_depth_as_a_string_passes(self):
        def change(data):
            find_step(data, "release", "Check out")["with"]["fetch-depth"] = "0"
        self.assert_passes(self.mutated(change))


UBUNTU_LIST = "tools/checks/ci-expected-failures-ubuntu.txt"
UBUNTU_TEXT = ("llvmpipe (LLVM 20.1.2, 256 bits): image view minLod is not supported; "
               "depthBounds is not supported")


class UbuntuList(Workflow):
    """The Ubuntu-only expected failure (docs/testing.md): used only by the ubuntu and coverage
    jobs, and the list holds exactly the approved entry."""

    def run_with_list(self, text):
        repo = Path(self.temp.name) / "repo"
        (repo / "tools" / "checks").mkdir(parents=True, exist_ok=True)
        # The --repo checkout also holds the committed Dependabot configuration (class Pins).
        (repo / ".github").mkdir(exist_ok=True)
        shutil.copyfile(REPO / DEPENDABOT, repo / DEPENDABOT)
        if text is not None:
            (repo / UBUNTU_LIST).write_text(text, encoding="utf-8")
        self.path.write_text(self.text, encoding="utf-8")
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = workflow_check.main(["--workflow", str(self.path), "--repo", str(repo)])
        return status, stdout.getvalue().splitlines()

    def test_the_committed_list_holds_the_approved_entry(self):
        self.assertEqual(workflow_check.UBUNTU_EXPECTED_FAILURES, {"kernel_file_system":
                                                                   UBUNTU_TEXT})
        status, lines = self.run_with_list(f"# header\nkernel_file_system {UBUNTU_TEXT}\n")
        self.assertEqual(status, 0, lines)

    def test_the_ubuntu_jobs_pass_the_list(self):
        for job in ("ubuntu", "coverage"):
            with self.subTest(job=job):
                self.assertEqual(self.data["jobs"][job]["env"]["UBUNTU_EXPECTED_FAILURES"],
                                 UBUNTU_LIST)
                step = find_step(self.data, job, "Check the test results")
                self.assertIn('--expected-failures "$UBUNTU_EXPECTED_FAILURES"', step["run"])

    def test_the_list_in_the_fedora_job(self):
        def change(data):
            option = '--expected-failures "$EXPECTED_FAILURES"'
            replace_in_run(data, "fedora", "Stage B - check the test results", option,
                           f"{option} --expected-failures {UBUNTU_LIST}")
        self.assert_fails(change, f"job fedora: uses {UBUNTU_LIST}, which only the ubuntu and "
                          "coverage jobs may use")

    def test_the_list_in_the_fedora_env(self):
        def change(data):
            data["jobs"]["fedora"]["env"]["UBUNTU_EXPECTED_FAILURES"] = UBUNTU_LIST
        self.assert_fails(change, f"job fedora: uses {UBUNTU_LIST}")

    def test_the_list_in_the_workflow_env(self):
        self.assert_fails(lambda data: data["env"].update(UBUNTU_EXPECTED_FAILURES=UBUNTU_LIST),
                          f"workflow: names {UBUNTU_LIST} outside the jobs; only the ubuntu and "
                          "coverage jobs may use it")

    def test_a_list_with_another_entry_or_text_fails(self):
        other = ("ShaderRecompilerComputeTests: VulkanHarness failed at dispatch: no Vulkan "
                 "graphics+compute device")
        for text in (f"kernel_file_system {UBUNTU_TEXT}\nlld_linked_executables x\n",
                     "kernel_file_system image view minLod is not supported\n",
                     f"gpu_tiler {other}\n"):
            with self.subTest(text=text):
                status, lines = self.run_with_list(text)
                self.assertEqual(status, 1, lines)
                self.assertTrue(any(line.startswith(f"lists: {UBUNTU_LIST} lists ") and
                                    "not exactly the approved entry" in line for line in lines),
                                lines)

    def test_a_missing_or_empty_list_fails(self):
        for text, fragment in ((None, "cannot read"), ("# only a comment\n", "lists no tests")):
            with self.subTest(text=text):
                status, lines = self.run_with_list(text)
                self.assertEqual(status, 1, lines)
                self.assertTrue(any(line.startswith(f"lists: {UBUNTU_LIST}: ") and fragment in line
                                    for line in lines), lines)


class Uploads(Workflow):
    def test_upload_without_if_always(self):
        def change(data):
            find_step(data, "fedora", "Upload the test results").pop("if")
        self.assert_fails(change, 'job fedora, step "Upload the test results": uploads with '
                          "if: None, not if: always() (Requirement 16.8)")

    def test_upload_only_on_success(self):
        def change(data):
            find_step(data, "coverage", "Upload the coverage report")["if"] = "success()"
        self.assert_fails(change, 'step "Upload the coverage report": uploads with if: '
                          "'success()'")

    def test_upload_with_if_always_in_braces_passes(self):
        def change(data):
            find_step(data, "ubuntu", "Upload the release archive")["if"] = "${{ always() }}"
        self.assert_passes(self.mutated(change))

    def test_flatpak_builder_uploads_on_its_own(self):
        def change(data):
            find_step(data, "flatpak", "build")["with"]["upload-artifact"] = True
        self.assert_fails(change, 'job flatpak, step "Build the bundle": lets the flatpak-builder '
                          "action upload the bundle")

    def test_upload_without_if_no_files_found(self):
        def change(data):
            find_step(data, "ubuntu", "Upload the release archive")["with"].pop("if-no-files-found")
        self.assert_fails(change, 'job ubuntu, step "Upload the release archive": uploads with '
                          "if-no-files-found: None, not error; a wrong path would upload nothing "
                          "and leave the run green (Requirements 16.8, 16.10)")

    def test_upload_that_only_warns_or_ignores(self):
        for value in ("warn", "ignore"):
            with self.subTest(value=value):
                def change(data, value=value):
                    find_step(data, "flatpak", "Upload the bundle")["with"]["if-no-files-found"] = (
                        value)
                self.assert_fails(change, 'job flatpak, step "Upload the bundle": uploads with '
                                  f"if-no-files-found: '{value}', not error;")


class FlatpakInstallation(Workflow):
    """Before the flatpak-builder action, the flatpak job copies the image's Flatpak installation
    to the runner's file system and points FLATPAK_SYSTEM_DIR at the copy (docs/packaging.md)."""

    MOVE = "Move the Flatpak installation off the image's overlay"
    COPY = 'cp -a /var/lib/flatpak "$RUNNER_TEMP/flatpak-system"\n'
    EXPORT = 'echo "FLATPAK_SYSTEM_DIR=$RUNNER_TEMP/flatpak-system" >> "$GITHUB_ENV"\n'
    MISSING = ("job flatpak: has no step before the flatpak-builder action that runs the copy of "
               "the image's Flatpak installation")

    def test_the_committed_step_moves_the_installation(self):
        step = find_step(self.data, "flatpak", self.MOVE)
        self.assertTrue(workflow_check.moves_flatpak_installation(step))
        self.assertLess(step_index(self.data, "flatpak", self.MOVE),
                        step_index(self.data, "flatpak", "build"))

    def test_missing_step(self):
        def change(data):
            data["jobs"]["flatpak"]["steps"].remove(find_step(data, "flatpak", self.MOVE))
        self.assert_fails(change, self.MISSING)

    def test_step_after_the_action(self):
        def change(data):
            move_step(data, "flatpak", self.MOVE, after="build")
        self.assert_fails(change, self.MISSING)

    def test_copy_or_export_missing_or_reversed(self):
        cases = {
            "no copy": lambda data: replace_in_run(data, "flatpak", self.MOVE, self.COPY, ""),
            "no export": lambda data: replace_in_run(data, "flatpak", self.MOVE, self.EXPORT, ""),
            "export before the copy": lambda data: replace_in_run(
                data, "flatpak", self.MOVE, self.COPY + self.EXPORT, self.EXPORT + self.COPY),
            "another directory": lambda data: replace_in_run(
                data, "flatpak", self.MOVE, "/var/lib/flatpak ", "/var/tmp/flatpak "),
        }
        for name, change in cases.items():
            with self.subTest(name):
                self.assert_fails(change, self.MISSING)

    def test_if_that_can_skip_the_step(self):
        def change(data):
            find_step(data, "flatpak", self.MOVE)["if"] = "github.ref == 'refs/heads/main'"
        self.assert_fails(change, f'job flatpak, step "{self.MOVE}": if: '
                          "\"github.ref == 'refs/heads/main'\" can skip the copy of the image's "
                          "Flatpak installation")

    def test_allowed_if_passes(self):
        def change(data):
            find_step(data, "flatpak", self.MOVE)["if"] = "${{ !cancelled() }}"
        self.assert_passes(self.mutated(change))

    def test_braced_variables_pass(self):
        def change(data):
            replace_in_run(data, "flatpak", self.MOVE, self.COPY + self.EXPORT,
                           'cp -a /var/lib/flatpak "${RUNNER_TEMP}/flatpak-system"\n'
                           'echo "FLATPAK_SYSTEM_DIR=${RUNNER_TEMP}/flatpak-system" >> '
                           '"${GITHUB_ENV}"\n')
        self.assert_passes(self.mutated(change))

    def test_env_that_overrides_the_copy(self):
        def on_the_action(data):
            find_step(data, "flatpak", "build")["env"] = {"FLATPAK_SYSTEM_DIR": "/var/lib/flatpak"}

        def on_the_job(data):
            data["jobs"]["flatpak"]["env"]["FLATPAK_SYSTEM_DIR"] = "/var/lib/flatpak"

        def on_the_workflow(data):
            data["env"]["FLATPAK_SYSTEM_DIR"] = "/var/lib/flatpak"

        for change, where in ((on_the_action, 'job flatpak, step "Build the bundle"'),
                              (on_the_job, "job flatpak"), (on_the_workflow, "workflow")):
            with self.subTest(where):
                self.assert_fails(change, f"{where}: sets FLATPAK_SYSTEM_DIR to "
                                  "'/var/lib/flatpak' in env:")

    NO_ACTION = ("job flatpak: has no step that uses the flatpak-builder action "
                 "(flatpak/flatpak-github-actions/flatpak-builder); the flatpak job builds the "
                 "bundle with it, and the copy of the image's Flatpak installation must come "
                 "before it (docs/packaging.md)")

    def test_the_action_replaced_by_a_run_step(self):
        def change(data):
            step = find_step(data, "flatpak", "build")
            del step["uses"], step["with"]
            step["run"] = ('flatpak-builder --install-deps-from=flathub --repo="$FLATPAK_REPO_DIR" '
                           '"$FLATPAK_BUILD_DIR" "$MANIFEST"\n')
        self.assert_fails(change, self.NO_ACTION)

    def test_the_action_at_another_path(self):
        def change(data):
            find_step(data, "flatpak", "build")["uses"] = (
                f"someone/flatpak-github-actions/flatpak-builder@{FAKE_SHA}")
        self.assert_fails(change, self.NO_ACTION)

    def test_another_version_of_the_action_passes(self):
        def change(data):
            find_step(data, "flatpak", "build")["uses"] = (
                f"flatpak/flatpak-github-actions/flatpak-builder@{FAKE_SHA}")
        self.assert_passes(self.mutated(change))


if __name__ == "__main__":
    unittest.main()
