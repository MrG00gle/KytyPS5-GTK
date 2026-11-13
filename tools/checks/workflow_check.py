#!/usr/bin/env python3
"""Check the structure of the CI workflow, .github/workflows/build.yml (task 28.3).

Requirements 3.5, 4.4, 16.1-16.4, 16.8, 16.10, and 16.13, as design.md's "CI workflow" section
lays them out. The CTest workflow_structure and the ubuntu job run this script on the workflow.
It reads the file with PyYAML (yaml.safe_load, which reads the key "on" as the boolean True, so
both are accepted) and checks:

  triggers   push to main and dev; pull_request to main and dev with exactly the types opened,
             reopened, and synchronize; workflow_dispatch; no other event, and no path, tag, or
             branches-ignore filter that would skip a run.
  jobs       exactly metadata, ubuntu, fedora, coverage, flatpak, and release, each on
             ubuntu-24.04; fedora in registry.fedoraproject.org/fedora:latest and flatpak in
             ghcr.io/flathub-infra/flatpak-github-actions:gnome-49 (RUNNERS); no other container;
             and no windows-* or macos-* runner label in runs-on or a matrix.
  packages   no install command (apt-get, apt, dnf, yum, pacman, zypper, pip) names a Qt package:
             no word on that line, and none in an env variable that the line expands, matches
             content_checks.py's Qt package pattern or contains "qt" in any case. No run line and
             no action matches content_checks.py's Qt build and installation pattern.
  ctest      no ctest call has any of -R, -E, -L, -LE, -I, -U, -FA, -FS, -FC, --tests-regex,
             --exclude-regex, --label-regex, --label-exclude, --tests-information, --union,
             --fixture-exclude-any, --fixture-exclude-setup, --fixture-exclude-cleanup,
             --rerun-failed, --tests-from-file, --exclude-from-file, --stop-on-failure, --repeat,
             or --preset: neither a call that runs tests nor one that lists them (-N,
             --show-only, --print-labels, --version, --help), whose list junit_check.py
             --registered compares the results with. The one exception is a step with id
             timing-rerun (the timing policy in tools/checks/ci-timing-retries.txt,
             docs/testing.md): it takes its test names from junit_check.py --retry-list ...
             --print-retry-candidates and comes after the job's test run.
  results    the ubuntu, fedora, and coverage jobs each run tests (an unfiltered ctest call, or
             the coverage target, which runs ctest itself) with VK_DRIVER_FILES set to the
             manifest of Mesa's software Vulkan driver on the job's distribution
             (VK_DRIVER_FILES below); after every test step, and after its timing-rerun step if
             there is one, a later step runs junit_check.py with --registered before the next test
             step. No job or step sets continue-on-error.
  release    the release job's if is exactly the push, refs/heads/main, and MrG00gle/KytyPS5-GTK
             conditions joined by && (success() may be added; no other status function and no ||);
             it needs every other job; it alone has write permission (contents: write), and the
             workflow's permissions are contents: read.
  uploads    every actions/upload-artifact step has if: always(), and the flatpak-builder action
             does not upload on its own (upload-artifact: false).
  steps      the steps the design requires in a job (REQUIRED_STEPS): the ubuntu job runs this
             check on the workflow, tools/docs/check_links.py on the checkout, and
             tools/docs/check_help_options.py on the kyty_emulator it built, against
             docs/running.md (task 29.6).
  lists      tools/checks/ci-expected-failures-ubuntu.txt, the expected failure that the
             repository owner approved for Ubuntu 24.04's lavapipe only (docs/testing.md), is
             named only inside the ubuntu and coverage jobs (not in the workflow's own env, and
             in no other job), and the file in the repository holds exactly the one approved
             entry (UBUNTU_EXPECTED_FAILURES below).

Usage:
    workflow_check.py --workflow FILE [--repo DIR]

--repo is the checkout whose list files are read (default: the one that holds this script).

It prints one line per problem, naming the job and the step, then "FAILED: workflow_structure:
<n> problem(s)", or one "PASS: workflow_structure: ..." line.

Exit status: 0 when the workflow passes, 1 when it has a problem or PyYAML is missing (the check
never passes without checking), 2 on an input error (an unreadable file, invalid YAML, or a
workflow whose top level, jobs, or steps do not have the shape of a workflow).
"""

import argparse
import os
import re
import shlex
import sys

# The Qt patterns of the qt_remnants content check, and junit_check.py's list reader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import content_checks  # noqa: E402
import junit_check  # noqa: E402

DEFAULT_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

NAME = "workflow_structure"
YAML_MISSING = "workflow_check.py needs PyYAML (python3-pyyaml or python3-yaml)"

BRANCHES = {"main", "dev"}
PULL_REQUEST_TYPES = {"opened", "reopened", "synchronize"}
EVENTS = {"push", "pull_request", "workflow_dispatch"}
# Job id -> (runs-on, container image or None); design.md "CI workflow" and plan D10.
RUNNER = "ubuntu-24.04"
RUNNERS = {
    "metadata": (RUNNER, None),
    "ubuntu": (RUNNER, None),
    "fedora": (RUNNER, "registry.fedoraproject.org/fedora:latest"),
    "coverage": (RUNNER, None),
    "flatpak": (RUNNER, "ghcr.io/flathub-infra/flatpak-github-actions:gnome-49"),
    "release": (RUNNER, None),
}
REMOVED_RUNNER = re.compile(r"(?i)^\s*(?:windows|macos)")
TEST_JOBS = ("ubuntu", "fedora", "coverage")
# Test job -> the lavapipe manifest of its distribution: Ubuntu 24.04's Mesa names it without the
# architecture, Fedora's with it.
VK_DRIVER_FILES = {
    "ubuntu": "/usr/share/vulkan/icd.d/lvp_icd.json",
    "fedora": "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
    "coverage": "/usr/share/vulkan/icd.d/lvp_icd.json",
}
# The expected failure that the repository owner approved for Ubuntu 24.04's lavapipe (Mesa 25.2.8)
# only, on 2026-11-13: the jobs that may use its list, and the list's only entry.
UBUNTU_LIST = "tools/checks/ci-expected-failures-ubuntu.txt"
UBUNTU_LIST_JOBS = ("ubuntu", "coverage")
UBUNTU_EXPECTED_FAILURES = {
    "kernel_file_system": "llvmpipe (LLVM 20.1.2, 256 bits): image view minLod is not supported; "
                          "depthBounds is not supported",
}
RELEASE_JOB = "release"
RELEASE_CONDITIONS = {"github.event_name == 'push'", "github.ref == 'refs/heads/main'",
                      "github.repository == 'MrG00gle/KytyPS5-GTK'"}
TIMING_RERUN = "timing-rerun"
# Job id -> [(what the step does, a regex that one logical line of its run must match)].
REQUIRED_STEPS = {
    "ubuntu": [
        ("the workflow structure check",
         re.compile(r"(?<![\w./-])tools/checks/workflow_check\.py\s+--workflow\s+"
                    r"\.github/workflows/build\.yml(?![\w./-])")),
        # Requirements 20.10 and 20.13.
        ("the documentation link check",
         re.compile(r"(?<![\w./-])tools/docs/check_links\.py\s+--repo\s+\.(?![\w./-])")),
        # Requirements 20.3 and 20.12: the Emulator of this run.
        ("the running guide's option check",
         re.compile(r"(?<![\w./-])tools/docs/check_help_options\.py\s+--emulator\s+"
                    r"(?:\"\$(?:BUILD_DIR|\{BUILD_DIR\})/kyty_emulator\"|"
                    r"\$(?:BUILD_DIR|\{BUILD_DIR\})/kyty_emulator)\s+--doc\s+docs/running\.md"
                    r"(?![\w./-])")),
    ],
}

INSTALL_COMMAND = re.compile(content_checks.QT_PACKAGE_MANAGER.pattern
                             + r"|(?<![\w-])pip3?(?![\w-])")
QT_IN_WORD = re.compile(r"(?i)qt")
ENV_REFERENCE = re.compile(r"\$\{\{\s*env\.([A-Za-z_]\w*)\s*\}\}|\$\{?([A-Za-z_]\w*)")
JUNIT_CHECK = re.compile(r"(?<![\w./-])(?:\./)?tools/checks/junit_check\.py(?![\w.-])")
COVERAGE_TARGET = re.compile(r"(?<![\w-])cmake\s.*--build\s.*--target(?:\s+[\w-]+)*?\s+coverage"
                             r"(?![\w-])")
# ctest options that list tests instead of running them.
LISTING = ("-N", "--show-only", "--print-labels", "--version", "--help")
# ctest options that select a subset of the tests, stop early, or retry failures.
SHORT_FILTER = re.compile(r"^-(?:R|E|LE?|I|U|F[ASC])")
LONG_FILTERS = ("--tests-regex", "--exclude-regex", "--label-regex", "--label-exclude",
                "--tests-information", "--union", "--fixture-exclude-any",
                "--fixture-exclude-setup", "--fixture-exclude-cleanup", "--rerun-failed",
                "--tests-from-file", "--exclude-from-file", "--stop-on-failure", "--repeat",
                "--preset")
# Words that keep the next word in command position.
PREFIXES = {"if", "then", "else", "elif", "do", "while", "until", "!", "{", "time", "sudo",
            "exec", "command", "nice"}
ASSIGNMENT = re.compile(r"^[A-Za-z_]\w*=")
STATUS_FUNCTION = re.compile(r"\b(?:always|failure|cancelled|success)\s*\(")


class MissingYaml(Exception):
    """PyYAML is not installed."""


class InputError(Exception):
    """The workflow file cannot be read as a workflow."""


def load_workflow(path):
    try:
        import yaml
    except ImportError:
        raise MissingYaml() from None
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from None
    try:
        workflow = yaml.safe_load(text)
    except yaml.YAMLError as error:
        raise InputError(f"{path} is not valid YAML: {error}") from None
    if not isinstance(workflow, dict):
        raise InputError(f"{path}: the top level is not a mapping")
    jobs = workflow.get("jobs")
    if not isinstance(jobs, dict) or not jobs:
        raise InputError(f"{path}: jobs is not a mapping of jobs")
    for job_id, job in jobs.items():
        if not isinstance(job, dict):
            raise InputError(f"{path}: job {job_id} is not a mapping")
        steps = job.get("steps", [])
        if not isinstance(steps, list) or not all(isinstance(step, dict) for step in steps):
            raise InputError(f"{path}: the steps of job {job_id} are not a list of mappings")
    return workflow


def expression(value):
    """An if: value without its ${{ }} and with single spaces."""
    text = " ".join(str(value).split())
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    return text


def strings_in(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from strings_in(item)
    elif isinstance(value, list):
        for item in value:
            yield from strings_in(item)


def logical_lines(script):
    """The lines of a run: script with backslash continuations joined."""
    return [text for _, text in content_checks.continued_lines(script.splitlines())]


def words(line):
    """The shell words and operators of one line; comments dropped."""
    lexer = shlex.shlex(line, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        return list(lexer)
    except ValueError:
        return line.split("#", 1)[0].split()


def is_operator(word):
    return bool(word) and all(character in ";&|()" for character in word)


def commands(line):
    """(command word, its arguments) for each simple command of one line."""
    found = []
    tokens = words(line)
    index = 0
    at_command = True
    while index < len(tokens):
        word = tokens[index]
        index += 1
        if is_operator(word):
            at_command = True
            continue
        if not at_command:
            continue
        if word in PREFIXES or ASSIGNMENT.match(word):
            continue
        if word == "env":
            while index < len(tokens) and (tokens[index].startswith("-")
                                           or ASSIGNMENT.match(tokens[index])):
                index += 2 if tokens[index] == "-u" else 1
            continue
        arguments = []
        while index < len(tokens) and not is_operator(tokens[index]):
            arguments.append(tokens[index])
            index += 1
        found.append((word, arguments))
        at_command = False
    return found


def is_option(argument, options):
    return any(argument == option or argument.startswith(option + "=") for option in options)


def ctest_calls(script):
    """(arguments, filters, whether it runs tests) of every ctest call."""
    calls = []
    for line in logical_lines(script):
        for word, arguments in commands(line):
            if word.rsplit("/", 1)[-1] != "ctest":
                continue
            filters = [argument for argument in arguments
                       if SHORT_FILTER.match(argument) or is_option(argument, LONG_FILTERS)]
            runs = not any(is_option(argument, LISTING) for argument in arguments)
            calls.append((arguments, filters, runs))
    return calls


def junit_check_lines(script):
    return [line for line in logical_lines(script) if JUNIT_CHECK.search(line)]


def has_option(line, option):
    """Whether line has the option as a word (also --option=value, or inside $( ))."""
    return re.search(rf"(?<![\w-]){re.escape(option)}(?![\w-])", line) is not None


def is_result_check(step):
    """A step that runs junit_check.py on a full run's JUnit file, with --registered."""
    return any(has_option(line, "--registered") and not has_option(line, "--print-retry-candidates")
               for line in junit_check_lines(step.get("run") or ""))


def names_retry_candidates(step):
    return any(has_option(line, "--print-retry-candidates") and has_option(line, "--retry-list")
               for line in junit_check_lines(step.get("run") or ""))


class Checker:
    def __init__(self, workflow, repo=DEFAULT_REPO):
        self.workflow = workflow
        self.repo = repo
        self.jobs = workflow["jobs"]
        self.problems = []
        self.ctest_runs = 0
        self.uploads = 0

    def report(self, where, text):
        self.problems.append(f"{where}: {text}")

    @staticmethod
    def job_label(job_id):
        return f"job {job_id}"

    @staticmethod
    def step_label(job_id, number, step):
        name = step.get("name") or step.get("id") or f"step {number}"
        return f'job {job_id}, step "{name}"'

    def steps(self, job_id):
        for number, step in enumerate(self.jobs[job_id].get("steps", []), 1):
            yield number, step, self.step_label(job_id, number, step)

    def run(self):
        self.check_triggers()
        self.check_permissions()
        for job_id in self.jobs:
            self.check_runner(job_id)
            self.check_job_steps(job_id)
        for job_id in RUNNERS:
            if job_id not in self.jobs:
                self.report("workflow", f"the design's job {job_id} is missing")
        for job_id in TEST_JOBS:
            if job_id in self.jobs:
                self.check_test_job(job_id)
        if RELEASE_JOB in self.jobs:
            self.check_release()
        for job_id, required in REQUIRED_STEPS.items():
            if job_id in self.jobs:
                self.check_required_steps(job_id, required)
        self.check_ubuntu_list()
        return self.problems

    # ---- triggers
    def check_triggers(self):
        if "on" in self.workflow:
            triggers = self.workflow["on"]
        else:
            triggers = self.workflow.get(True)
        if isinstance(triggers, str):
            triggers = {triggers: None}
        elif isinstance(triggers, list):
            triggers = {event: None for event in triggers}
        elif not isinstance(triggers, dict):
            self.report("workflow", "on: has no triggers")
            return
        for event in sorted(EVENTS - set(triggers)):
            self.report("workflow", f"on: lacks the {event} trigger")
        for event in sorted(set(triggers) - EVENTS, key=str):
            self.report("workflow", f"on: has the {event} trigger, which the design does not")
        for event, allowed in (("push", {"branches"}), ("pull_request", {"branches", "types"})):
            if event not in triggers:
                continue
            config = triggers[event] if isinstance(triggers[event], dict) else {}
            for key in sorted(set(config) - allowed):
                self.report("workflow", f"on.{event}: the {key} filter can skip runs")
            branches = config.get("branches")
            if not isinstance(branches, list) or set(branches) != BRANCHES:
                self.report("workflow", f"on.{event}.branches is {branches!r}, not main and dev")
        if "pull_request" in triggers:
            config = triggers["pull_request"]
            types = config.get("types") if isinstance(config, dict) else None
            if not isinstance(types, list) or set(types) != PULL_REQUEST_TYPES:
                self.report("workflow", f"on.pull_request.types is {types!r}, not exactly "
                            "opened, reopened, and synchronize")

    # ---- permissions
    def check_permissions(self):
        permissions = self.workflow.get("permissions")
        if not isinstance(permissions, dict) or permissions.get("contents") != "read":
            self.report("workflow", f"permissions is {permissions!r}, not contents: read")
        elif "write" in permissions.values():
            self.report("workflow", "permissions grants write access")
        for job_id, job in self.jobs.items():
            if job_id == RELEASE_JOB or "permissions" not in job:
                continue
            permissions = job["permissions"]
            if permissions == "write-all" or (isinstance(permissions, dict)
                                              and "write" in permissions.values()):
                self.report(self.job_label(job_id), "grants write access; only the release job "
                            "may (contents: write)")

    # ---- runners and containers
    def check_runner(self, job_id):
        job = self.jobs[job_id]
        where = self.job_label(job_id)
        strategy = job.get("strategy")
        labels = list(strings_in(job.get("runs-on")))
        labels += list(strings_in(strategy.get("matrix") if isinstance(strategy, dict) else None))
        removed = [label for label in labels if REMOVED_RUNNER.match(label)]
        for label in removed:
            self.report(where, f"uses the runner {label}: no windows-* or macos-* runner "
                        "(Requirements 3.5, 4.4)")
        if job_id not in RUNNERS:
            self.report(where, "is not one of the design's jobs "
                        f"({', '.join(RUNNERS)})")
            return
        runner, image = RUNNERS[job_id]
        if job.get("runs-on") != runner and not removed:
            self.report(where, f"runs on {job.get('runs-on')!r}, not {runner}")
        container = job.get("container")
        found = container.get("image") if isinstance(container, dict) else container
        if found != image:
            wanted = f"the container {image}" if image else "no container"
            self.report(where, f"has the container {found!r}, not {wanted}"
                        if found else f"has no container, not {wanted}")

    # ---- every step: Qt, ctest filters, uploads, continue-on-error
    def environment(self, job_id, step):
        env = {}
        for scope in (self.workflow, self.jobs[job_id], step):
            if isinstance(scope.get("env"), dict):
                env.update({str(key): value for key, value in scope["env"].items()})
        return env

    def check_job_steps(self, job_id):
        job = self.jobs[job_id]
        if job.get("continue-on-error"):
            self.report(self.job_label(job_id), "sets continue-on-error, so its failures would "
                        "not fail the run (Requirement 16.10)")
        for number, step, where in self.steps(job_id):
            if step.get("continue-on-error"):
                self.report(where, "sets continue-on-error, so its failure would not fail the "
                            "run (Requirement 16.10)")
            uses = step.get("uses")
            if isinstance(uses, str):
                self.check_action(where, step, uses)
            script = step.get("run")
            if isinstance(script, str):
                self.check_qt(where, script, self.environment(job_id, step))
                self.check_ctest(where, step, script)

    def check_action(self, where, step, uses):
        if content_checks.QT_BUILD_PATTERN.search(uses):
            self.report(where, f"uses {uses}, a Qt installation action")
        action = uses.split("@", 1)[0]
        with_ = step.get("with") if isinstance(step.get("with"), dict) else {}
        if action == "actions/upload-artifact":
            self.uploads += 1
            if expression(step.get("if", "")) != "always()":
                self.report(where, f"uploads with if: {step.get('if')!r}, not if: always() "
                            "(Requirement 16.8)")
        if action == "flatpak/flatpak-github-actions/flatpak-builder":
            if str(with_.get("upload-artifact")).lower() != "false":
                self.report(where, "lets the flatpak-builder action upload the bundle; set "
                            "upload-artifact: false and upload it with if: always()")

    def check_qt(self, where, script, env):
        for line in logical_lines(script):
            if content_checks.QT_BUILD_PATTERN.search(line):
                self.report(where, f"runs a Qt build or installation tool: {line}")
            if not INSTALL_COMMAND.search(line):
                continue
            packages = [(word, None) for word in line.split()]
            for match in ENV_REFERENCE.finditer(line):
                name = match.group(1) or match.group(2)
                if isinstance(env.get(name), str):
                    packages += [(word, name) for word in env[name].split()]
            for word, variable in packages:
                if content_checks.QT_PACKAGE.search(word) or QT_IN_WORD.search(word):
                    source = f" (from ${variable})" if variable else ""
                    self.report(where, f"installs {word}{source}, a Qt package "
                                "(Requirement 16.3)")

    def check_ctest(self, where, step, script):
        rerun = step.get("id") == TIMING_RERUN
        for arguments, filters, runs in ctest_calls(script):
            self.ctest_runs += runs
            if not filters or rerun:
                continue
            if runs:
                self.report(where, f"runs ctest with {' '.join(filters)}; every test run must "
                            f"be unfiltered (Requirement 16.4): ctest {' '.join(arguments)}")
            else:
                self.report(where, f"lists the tests with {' '.join(filters)}; the list for "
                            f"junit_check.py --registered must hold every test: ctest "
                            f"{' '.join(arguments)}")
        if rerun and not names_retry_candidates(step):
            self.report(where, "reruns tests without taking their names from junit_check.py "
                        "--retry-list ... --print-retry-candidates")

    # ---- test jobs: tests, VK_DRIVER_FILES, the result check after each test step
    def check_test_job(self, job_id):
        steps = list(self.steps(job_id))
        tests = [index for index, (_, step, _) in enumerate(steps)
                 if step.get("id") != TIMING_RERUN and self.runs_tests(step)]
        reruns = [index for index, (_, step, _) in enumerate(steps)
                  if step.get("id") == TIMING_RERUN]
        if not tests:
            self.report(self.job_label(job_id), "runs no tests (an unfiltered ctest call or the "
                        "coverage target; Requirement 16.4)")
        env = {}
        for scope in (self.workflow, self.jobs[job_id]):
            if isinstance(scope.get("env"), dict):
                env.update(scope["env"])
        manifest = VK_DRIVER_FILES[job_id]
        if env.get("VK_DRIVER_FILES") != manifest:
            self.report(self.job_label(job_id), f"sets VK_DRIVER_FILES to "
                        f"{env.get('VK_DRIVER_FILES')!r}, not {manifest} "
                        "(Requirement 16.5)")
        for index in tests:
            _, step, where = steps[index]
            value = (step.get("env") or {}).get("VK_DRIVER_FILES", manifest)
            if value != manifest:
                self.report(where, f"sets VK_DRIVER_FILES to {value!r}, not {manifest}")
        for index in reruns:
            if not any(test < index for test in tests):
                self.report(steps[index][2], "comes before the job's test run; it may only "
                            "rerun tests after the unfiltered run")
        for position, index in enumerate(tests):
            end = tests[position + 1] if position + 1 < len(tests) else len(steps)
            start = index
            for rerun in reruns:
                if index < rerun < end:
                    start = rerun + 1
            if not any(is_result_check(steps[k][1]) for k in range(start, end)):
                after = "this test run" if start == index else "its timing-rerun step"
                self.report(steps[index][2], f"no later step runs junit_check.py --registered "
                            f"after {after} (Requirements 16.4-16.6)")

    @staticmethod
    def runs_tests(step):
        script = step.get("run") or ""
        return (any(runs for _, _, runs in ctest_calls(script))
                or any(COVERAGE_TARGET.search(line) for line in logical_lines(script)))

    # ---- the release job
    def check_release(self):
        job = self.jobs[RELEASE_JOB]
        where = self.job_label(RELEASE_JOB)
        condition = expression(job.get("if", ""))
        # Without ||, grouping parentheses do not change a chain of && terms.
        conjunction = condition.replace("success()", "SUCCESS")
        other_status = STATUS_FUNCTION.search(conjunction)
        conjunction = conjunction.replace("(", " ").replace(")", " ")
        terms = {re.sub(r"\s*==\s*", " == ", " ".join(term.split()))
                 for term in conjunction.split("&&")}
        extra = terms - RELEASE_CONDITIONS - {"SUCCESS"}
        if "||" in condition or not RELEASE_CONDITIONS <= terms or extra or other_status:
            self.report(where, f"if: {job.get('if')!r} is not exactly "
                        f"{' && '.join(sorted(RELEASE_CONDITIONS))} (Requirements 16.10, 16.13)")
        needs = job.get("needs", [])
        needs = {needs} if isinstance(needs, str) else set(needs or [])
        for job_id in sorted(set(self.jobs) - {RELEASE_JOB} - needs):
            self.report(where, f"needs lacks {job_id}; the release job needs every other job "
                        "(Requirement 16.10)")
        permissions = job.get("permissions")
        if not isinstance(permissions, dict) or permissions.get("contents") != "write":
            self.report(where, f"permissions is {permissions!r}, not contents: write")

    # ---- required steps
    def check_required_steps(self, job_id, required):
        lines = [line for _, step, _ in self.steps(job_id)
                 for line in logical_lines(step.get("run") or "")]
        for description, pattern in required:
            if not any(pattern.search(line) for line in lines):
                self.report(self.job_label(job_id), f"has no step that runs {description}")

    # ---- the Ubuntu-only expected-failure list
    def check_ubuntu_list(self):
        name = os.path.basename(UBUNTU_LIST)
        allowed = " and ".join(UBUNTU_LIST_JOBS)
        outside_jobs = {key: value for key, value in self.workflow.items() if key != "jobs"}
        if any(name in text for text in strings_in(outside_jobs)):
            self.report("workflow", f"names {UBUNTU_LIST} outside the jobs; only the {allowed} "
                        "jobs may use it")
        for job_id, job in self.jobs.items():
            if job_id not in UBUNTU_LIST_JOBS and any(name in text for text in strings_in(job)):
                self.report(self.job_label(job_id), f"uses {UBUNTU_LIST}, which only the "
                            f"{allowed} jobs may use (Ubuntu 24.04's lavapipe)")
        path = os.path.join(self.repo, UBUNTU_LIST)
        try:
            entries = junit_check.read_expected_failures(path)
        except junit_check.InputError as error:
            self.report("lists", f"{UBUNTU_LIST}: {error}")
            return
        if entries != UBUNTU_EXPECTED_FAILURES:
            listed = ", ".join(sorted(entries)) or "nothing"
            self.report("lists", f"{UBUNTU_LIST} lists {listed}, not exactly the approved entry "
                        "for kernel_file_system with its text")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check the structure of the CI workflow; print each problem and exit 1 if "
        "there is one.")
    parser.add_argument("--workflow", required=True, help="the workflow file (build.yml)")
    parser.add_argument("--repo", default=DEFAULT_REPO,
                        help="the checkout whose list files are read (default: %(default)s)")
    args = parser.parse_args(argv)
    try:
        workflow = load_workflow(args.workflow)
    except MissingYaml:
        print(YAML_MISSING, file=sys.stderr)
        return 1
    except InputError as error:
        print(f"error: {NAME}: {error}", file=sys.stderr)
        return 2
    checker = Checker(workflow, args.repo)
    problems = checker.run()
    for problem in problems:
        print(problem)
    if problems:
        print(f"FAILED: {NAME}: {len(problems)} problem(s)")
        return 1
    steps = sum(len(job.get("steps", [])) for job in checker.jobs.values())
    print(f"PASS: {NAME}: {len(checker.jobs)} jobs, {steps} steps, {checker.ctest_runs} ctest "
          f"test runs, {checker.uploads} uploads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
