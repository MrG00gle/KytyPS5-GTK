"""Unit tests for tools/checks/workflow_check.py (task 28.3).

The committed workflow passes, and mutated copies of it fail with one line that names the problem,
the job, and the step. PyYAML is required: without it this module fails to import with the
checker's message.
"""

import contextlib
import copy
import io
import os
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

# Built from pieces, so that this file names no Qt package or action.
QT_UBUNTU = "q" "t6-base-dev"
QT_FEDORA = "q" "t6-qtbase-devel"
QT_ACTION = "jurplel/install-q" "t-action@v4"
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


class Workflow(unittest.TestCase):
    """The committed workflow, a temporary directory for mutated copies, and a runner."""

    @classmethod
    def setUpClass(cls):
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.data = yaml.safe_load(cls.text)

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

    @staticmethod
    def dump(data):
        return yaml.safe_dump(data, sort_keys=False, allow_unicode=True, width=1 << 20)

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
            data["jobs"]["lint"] = {"runs-on": "ubuntu-24.04", "steps": [{"run": "true"}]}
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
        self.assert_fails(change, "job coverage: runs no tests")

    def test_missing_vk_driver_files(self):
        self.assert_fails(lambda data: data["jobs"]["coverage"]["env"].pop("VK_DRIVER_FILES"),
                          "job coverage: sets VK_DRIVER_FILES to None", "(Requirement 16.5)")

    def test_other_vk_driver_on_the_test_step(self):
        def change(data):
            step = find_step(data, "ubuntu", "tests")
            step["env"] = {"VK_DRIVER_FILES": "/usr/share/vulkan/icd.d/intel_icd.x86_64.json"}
        self.assert_fails(change, 'job ubuntu, step "Run every test": sets VK_DRIVER_FILES to')

    def test_continue_on_error(self):
        def change(data):
            find_step(data, "ubuntu", "tests")["continue-on-error"] = True
        self.assert_fails(change, 'job ubuntu, step "Run every test": sets continue-on-error')

    def test_missing_workflow_check_step(self):
        def change(data):
            steps = data["jobs"]["ubuntu"]["steps"]
            steps.remove(find_step(data, "ubuntu", "Check the workflow structure"))
        self.assert_fails(change, "job ubuntu: has no step that runs the workflow structure check")


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


if __name__ == "__main__":
    unittest.main()
