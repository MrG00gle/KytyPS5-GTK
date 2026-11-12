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
  `main` publishes a release.
- Every CI check must pass before a pull request is merged: the builds on Ubuntu and Fedora, the full
  test suite in each of them, the coverage check, and the Flatpak build with its validations. The
  [testing guide](docs/testing.md#ci) describes the jobs.

Pull requests for general fixes, architectural changes, or refactoring that do not address a concrete
bug or launch problem in a specific game will be automatically closed.

Large, unfocused commits or pull requests that bundle unrelated changes without a clear purpose or
game-related benefit will also be automatically closed.

Keep each pull request focused. Name the affected game, describe the exact problem, and provide
evidence showing what the change fixes. Linux is the target platform, so changes should not
introduce regressions on Linux. Because the emulator is still evolving quickly, consider opening an
issue before starting a large change.

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
