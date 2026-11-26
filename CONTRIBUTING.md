# Contributing to KytyPS5-GTK

Testing games and submitting detailed bug reports are useful ways to contribute. Search the
[existing issues](https://github.com/MrG00gle/KytyPS5-GTK/issues) first, then use the
**Game Emulation Status Report** template. It asks for your Linux distribution and version and for
how you installed KytyPS5-GTK (Flatpak, release archive, or source build). Attach the complete log.
The emulator writes it only when the **Printf output** setting is `Console` or `File`, so set it
before you reproduce the problem as the [running guide](docs/running.md#the-log-view) describes,
then save the log from the launcher's log view with **Save log…**.

## Branches and pull requests

- Pull requests target the `dev` branch. Do not open pull requests against `main`.
- `dev` is merged into `main` only after explicit approval from the repository owner. Every push to
  `main` publishes a release. To re-run a run that has published its release, re-run only its
  `release` job, as the [packaging guide](docs/packaging.md#re-running-a-release) explains.
- A release's text lists the commit subjects since the previous release, unless notes were written
  by hand for it. To write them, replace any earlier notes below the comment in
  [`packaging/release-notes.md`](packaging/release-notes.md) with the new ones (all text outside
  the comment becomes the release's text) and commit the file to `dev` before the merge, as the
  [packaging guide](docs/packaging.md#release-notes) describes.
- Every CI check must pass before a pull request is merged: the builds on Ubuntu and Fedora, the full
  test suite in each of them, the coverage check, and the Flatpak build with its validations. The
  [testing guide](docs/testing.md#ci) describes the jobs.

Keep each pull request focused on one change with a clear purpose, and do not bundle unrelated
changes. Describe the exact problem, name the affected game when there is one, and provide
evidence showing what the change fixes. Linux is the target platform, so changes should not
introduce regressions on Linux. Because the emulator is still evolving quickly, consider opening an
issue before starting a large change.

## Following upstream KytyPS5

KytyPS5-GTK takes the work of [upstream KytyPS5](https://github.com/KytyPS5/KytyPS5) by merging
its `main` branch. The last upstream commit merged into the fork is the merge base of `dev` and
the upstream branch:

```bash
git merge-base dev refs/upstream/heads/main
```

Fetch upstream by URL into `refs/upstream/`, without tags and without adding a remote: the fork's
version names come from its own tags, so upstream's tags must stay out of `refs/tags`. The status
tool fetches that way with `--fetch` (`git fetch --no-tags` with the URL) and then shows the
upstream head, the last synced commit, the number of upstream commits not merged yet, and the size
of their changes:

```bash
python3 tools/upstream/sync_status.py --repo . --fetch --forecast
```

Without `--fetch` it only reads the repository. `--forecast` adds the paths that a plain merge and
the merge helper would leave in conflict, and `--tsv FILE` writes a draft table with a suggested
disposition for each upstream commit.

Merge upstream about every two weeks, so that each merge brings about one stage of upstream work.
Create a branch from `dev`, and merge one or two cut points (the upstream commits to merge up to)
on it with the merge helper. It needs `unifdef` and a work tree without changes, and it writes its
report and a copy of every file it computed to the paths you give it:

```bash
git switch -c upstream-sync dev
python3 tools/upstream/platform_merge.py --repo . --report _Build/upstream-merge/report.md \
    --auto-dir _Build/upstream-merge/auto <upstream commit>
```

The helper runs `git merge --no-ff --no-commit` and then:

- strips the conditionals for the removed platforms from upstream's side of every C and C++ file
  under `src` and `tests` before it merges the file, and lists the conditionals that are left;
- keeps the old launcher's sources and tests and the removed platforms' files deleted, and leaves
  out what upstream adds to them;
- keeps the fork's own `README.md`, `CONTRIBUTING.md`, documentation, CI workflow, top-level
  `CMakeLists.txt`, and `src/main.cpp`, and lists the upstream commits that change them, so that
  you can carry their intent over by hand (for example, upstream's new tests are registered in
  `tests/CMakeLists.txt`);
- leaves every remaining conflict unmerged, with its markers, and lists it in the report.

It exits with 0 when nothing is left to do by hand and with 1 otherwise. Resolve what the report
lists, build, and run the full test suite. Then conclude the merge with `git commit`, or drop it
with `git merge --abort`. Name the upstream range in the merge commit's message, for example
`emulator: merge upstream kytyps5 up to 0685bffb`. Generated release notes show such a merge as one
line, as the [packaging guide](docs/packaging.md#release-notes) describes.

Port upstream's changes to the old launcher to the GTK launcher in separate commits, and name the
upstream commit in the body of each commit message, for example
`Ports upstream KytyPS5 commit d2ec91b8.` Then open the pull request from the branch to `dev`.

## Building and testing

The [building guide](docs/building.md) lists the dependencies and the build commands, and the
[testing guide](docs/testing.md) shows how to run the test suite, the coverage report, and the
manual checklist. Run the full test suite before you open a pull request.

The repository has a Visual Studio Code setup in `.vscode/`: CMake Tools with the `Clang` kit
(`/usr/bin/clang` and `/usr/bin/clang++`) and Ninja, building in `_Build/vscode-clang`, and gdb
launch configurations for `kyty_launcher` and `kyty_emulator`.

## Formatting

Set up the clang-format hook after cloning. Install `pre-commit` with your distribution's package
manager or with pipx:

- **Fedora:** `sudo dnf install pre-commit`
- **Ubuntu and Debian:** `sudo apt install pre-commit`
- **Arch Linux and CachyOS:** `sudo pacman -S pre-commit`
- **Other distributions:** `pipx install pre-commit`

Then install the Git hook:

```bash
pre-commit install --install-hooks
```

The hook runs on every commit and formats the staged `.cpp`, `.h`, and `.inc` files in `src` with
the style of `src/.clang-format`. To run it on the staged files without committing:

```bash
pre-commit run
```

## Coverage rules

CI measures the line coverage of the first-party code under `src` in every run
([testing guide](docs/testing.md#coverage)). A pull request fails the coverage check when:

- less than 80 % of the lines it adds or changes under `src` are covered (the changed-line rule);
- the overall line coverage is more than 0.50 percentage points below the committed baseline in
  `.github/coverage-baseline.txt` (the regression limit);
- a coverage group falls below its threshold: 90 % lines and 95 % functions in `src/launcher/core`,
  90 % lines in `src/updater`, and 60 % lines in `src/launcher/gtk`.

Add tests with your change so that the new lines are covered.

## AI use policy

The AI use rules of the original README apply unchanged:

### AI Use

AI tools may be used for research, reverse engineering, and development assistance. Contributors
must fully understand, review, and test all code they submit and remain responsible for its
correctness. Repository communication, including pull-request descriptions, code comments, and
issue comments, must come from the human contributor rather than an autonomous AI agent.

Pull requests that include AI-assisted or AI-generated work should disclose the scope of the AI
involvement and describe the human review and testing performed before submission. Unverified or
untested generated changes may be closed without review.
