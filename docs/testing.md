# Testing

The test suite is every test registered with CTest. The `kyty_tests` target builds all test
executables. It covers the launcher core, the Updater, the GTK launcher (UI tests in headless
display sessions), the emulator's testable parts, the build system, the packaging, the CI workflow,
and repository content rules. The commands below use the build directory of the
[building guide](building.md).

## Build and run the tests

```bash
cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build _Build/linux --target all kyty_tests
ctest --test-dir _Build/linux --output-on-failure                     # the full suite
ctest --test-dir _Build/linux -L property --output-on-failure         # only the tests with one label
ctest --test-dir _Build/linux -R '^emulator_help$' --output-on-failure  # one named test
ctest --test-dir _Build/linux -N                                      # list the tests
```

Run the suite serially, as above: the timing tests (for example
`launcher_ui_library_search_timing_wayland`, which requires a search to repaint the game list within
100 ms in a Release build) need a machine without other heavy load. The UI tests start their own
display sessions, so the suite also runs without a desktop. To run it the way a CI machine does,
without your desktop session's variables, use
`env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS ctest --test-dir _Build/linux --output-on-failure`.

Every test has a CTest `TIMEOUT` from 1 to 600 seconds. With `-DKYTY_BUILD_LAUNCHER=OFF` the suite
has no launcher, Updater, or UI tests.

## Labels

| Label | Tests | What they need |
|---|---|---|
| `gpu` | Tests that create a Vulkan device: the shader recompiler's Vulkan harness modes and `kernel_file_system`. | A Vulkan 1.3 device. Without one they exit with code 77, which CTest reports as skipped. |
| `ui` | Tests that drive the GTK launcher's windows, one CTest per case and display backend (`_wayland` and `_x11` suffixes). | weston, Xvfb, and `dbus-daemon`. Without them they exit with code 77 (skipped). |
| `property` | RapidCheck property tests, one per correctness property (`<executable>_p<NN>`). Each run checks at least 100 generated inputs. | Nothing special. |
| `source-check` | Checks of the repository content: removed platform terms and deleted paths, leftovers of the old launcher's toolkit, the layering rule, translatable strings and the launcher's own texts for core errors, the dialog registry, the Flatpak manifest, its pins, and the metainfo, the CI workflow structure, and the preserved license files and copyright lines. | Python 3 and PyYAML. |

Tests without a label need nothing beyond the build.

### GPU tests

A GPU test first creates a Vulkan instance. When that fails or finds no physical device, the test
prints `no Vulkan device available` and exits with 77, its `SKIP_RETURN_CODE`, so CTest reports it
as skipped instead of failed. `gpu_skip_without_device` checks that path with an empty Vulkan driver
list.

To run the GPU tests on Mesa's software Vulkan driver (lavapipe), as CI does, point the Vulkan loader
at it:

```bash
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ctest --test-dir _Build/linux -L gpu --output-on-failure
```

On Arch Linux the file is `/usr/share/vulkan/icd.d/lvp_icd.json`. Without `VK_DRIVER_FILES` the tests
use the machine's own GPU. lavapipe lacks fragment shader barycentrics and 64-bit LDS and image
atomics, so the Vulkan harness modes of `shader_recompiler_compute_tests` fail on it with "no Vulkan
graphics+compute device with fragment barycentrics and 64-bit LDS/image atomics". Run the `gpu`
label on a real GPU to test them (see the [manual checklist](#manual-checklist)).

### UI tests and the headless display

Each UI test runs inside `tests/ui/with-display.sh`, which starts a private session and runs the
test in it:

```bash
bash tests/ui/with-display.sh wayland -- <command> [<argument>...]
bash tests/ui/with-display.sh x11 -- <command> [<argument>...]
```

The session has a private `dbus-daemon` that activates nothing on the host, a fake desktop portal
when a test asks for one, and either `weston --backend=headless` or `Xvfb` at 1280×800, with its own
temporary home, runtime, configuration, and data directories, the C.UTF-8 locale, and GTK's test
accessibility backend. The script returns the command's exit status, and 77 only when `dbus-daemon`,
weston, or Xvfb is missing. A GTK, libadwaita, or launcher critical or warning message during a UI
test fails it and prints the message; `ui_harness_selftest` checks that.

### Reproducing a property test failure

RapidCheck prints `Using configuration: seed=<N>` before each property and the shrunk
counterexample when the property fails. Rerun the failing test with that seed to get the same input
again:

```bash
RC_PARAMS="seed=1234567" ctest --test-dir _Build/linux -R '^launcher_core_scan_tests_p01$' --output-on-failure
```

Every other `RC_PARAMS` setting is kept, and a `max_success` below 100 is raised to 100.
`property_harness_selftest` checks that a seed reproduces a failure.

## Coverage

The coverage build instruments every first-party target with Clang source-based coverage. The
`coverage` target runs the whole test suite without filters, merges the profiles, and writes the
report, also when tests fail; it then exits non-zero:

```bash
cmake -S . -B _Build/coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKYTY_ENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build _Build/coverage --target all kyty_tests
cmake --build _Build/coverage --target coverage
```

The report is `_Build/coverage/coverage/html/index.html`. The directory `_Build/coverage/coverage/`
holds:

- `html/index.html`: line, function, and branch coverage per directory under `src` and per file,
  with links to the annotated sources;
- `summary.json`: the same numbers for the whole scope, every directory, every file, and the
  coverage groups;
- `coverage.lcov`, `export.json`, and `ctest.xml` (the JUnit result of that test run).

The scope is the Git-tracked files under `src`, without generated and third-party files. A file that
no test runs counts with 0 covered lines. Set `KYTY_COVERAGE_THREADS` (for example to `1`) to lower
the memory use of the report steps.

To evaluate the report against the CI rules on your machine:

```bash
python3 tools/coverage/check.py --summary _Build/coverage/coverage/summary.json \
  --lcov _Build/coverage/coverage/coverage.lcov --groups tools/coverage/groups.json \
  --source-dir . --baseline .github/coverage-baseline.txt
```

### Thresholds

CI fails the coverage check when any of these is not met:

| Rule | Value |
|---|---|
| Launcher core (`src/launcher/core`), line coverage | at least 90 % |
| Launcher core (`src/launcher/core`), function coverage | at least 95 % |
| Updater (`src/updater`), line coverage | at least 90 % |
| GTK launcher code (`src/launcher/gtk`), line coverage | at least 60 % |
| `src/common`, `src/kernel`, `src/libs`, `src/loader`, `src/graphics/guest_gpu`, `src/graphics/host_gpu`, `src/graphics/shader`, `src/graphics/presentation`, `src/launcher`, `src/updater`, line coverage each | above 0 % |
| Overall line coverage of the scope, rounded to two decimals | at most 0.50 percentage points below the baseline |
| Pull requests: lines added or changed under `src` that count toward line coverage, compared with the merge base | at least 80 % covered |

`tools/coverage/groups.json` defines the three groups and their thresholds. Emulator code that needs
commercial games or a real GPU has only the above-0 % and baseline rules.

### Baseline

`.github/coverage-baseline.txt` holds one line: the overall line coverage of the scope with two
decimals, from 0.00 to 100.00 (for example `43.50`).

1. The first baseline comes from the first coverage-enabled CI run on `dev`. Until the file exists,
   the coverage check fails on purpose and writes the measured value to the run summary. The
   maintainer commits that value as the file's content.
2. Every later run fails when its value is more than 0.50 points below the baseline, and writes both
   values to the run summary.
3. When a push to `main` measures a value above the baseline, the run summary shows it as the next
   baseline. The maintainer raises the baseline by committing that value through a pull request to
   `dev`.

A missing file, or a file with anything other than one such value, fails the check.

## CI

`.github/workflows/build.yml` runs on pushes and pull requests to `main` and `dev` and on manual
dispatch. Its jobs:

- **Ubuntu 24.04**: Release build, the full test suite, the release archive, and its license check;
- **Fedora** (the newest generally available release): first without GTK and with
  `KYTY_BUILD_LAUNCHER=OFF`, then with everything, each followed by the full test suite;
- **Coverage**: the coverage build on Ubuntu 24.04, the report as an artifact, and the coverage
  check;
- **Flatpak**: the bundle, `flatpak-builder-lint`, the desktop entry and metainfo validation, the
  license check, and a smoke test of the installed bundle;
- **Publish the release**: only for pushes to `main` of `MrG00gle/KytyPS5-GTK`, and only when every
  other job succeeded.

Every test job runs the whole suite with no name, label, or exclusion filter, on lavapipe for the
GPU tests and with headless display servers for the UI tests. `tools/checks/junit_check.py` then
fails the job for every failed, skipped, or not-run test, so a GPU test or UI test that skips counts
as a failure. `tools/checks/check_regression.py` checks that every test in
`tools/checks/main-passing-tests.txt`, the tests that passed on the original `main`, still passes.

The repository owner approved two exceptions, each for the listed tests only:

- **Expected GPU failures** (`tools/checks/ci-expected-failures.txt`). The Vulkan harness modes of
  `shader_recompiler_compute_tests` need features that lavapipe lacks. CI still runs them. A listed
  test is accepted only when it fails with exactly the listed text. It fails CI when it fails for
  another reason, is skipped, or passes (then remove it from the list). These tests run on a real
  GPU in the manual checklist before each merge.
- **Timing reruns** (`tools/checks/ci-timing-retries.txt`). The two search timing tests
  (`launcher_ui_library_search_timing_wayland` and `_x11`) have little headroom on shared CI runners.
  When the only failures of the full run are listed tests, the `timing-rerun` step runs exactly
  those again, up to 3 times. The job passes only when every other test passed in the full run and
  each listed test passed in the rerun. Every miss is printed with its median in the job summary.

Adding a test to either list needs the owner's approval.

## Manual checklist

Run these checks before asking for the approval to merge `dev` into `main`, and record the results
in the pull request:

- [ ] The Flatpak on a Wayland session and on an X11-only session: `flatpak run
      io.github.MrG00gle.KytyPS5_GTK` shows the main window within 10 seconds, and the launcher and
      the emulator windows use Wayland, or X11 when there is no Wayland.
- [ ] Hardware GPU selection inside the Flatpak: on a host whose GPU and driver support Vulkan 1.3,
      with the matching Flatpak graphics driver extension installed, the emulator log names the
      hardware GPU as the selected device, not a software driver such as llvmpipe. Check this on a
      discrete and on an integrated GPU if you can, so that the device type ranking (discrete before
      integrated before virtual before CPU) is confirmed on real hardware, and check that `--gpu
      <index>` selects the given device.
- [ ] The `gpu` label on a real GPU, without `VK_DRIVER_FILES`:
      `ctest --test-dir _Build/linux -L gpu --output-on-failure` passes completely, including the
      tests of `tools/checks/ci-expected-failures.txt`.
- [ ] Gamepad hotplug over USB and Bluetooth: the running game sees a connected or disconnected
      gamepad within 2 seconds.
- [ ] DualSense haptics and speaker audio over USB and over Bluetooth.
- [ ] Game audio on the default output device, and the microphone input from the selected device or
      the default one.
- [ ] Portal folder access: a game folder chosen in the Flatpak's folder chooser is still readable
      after the launcher restarts and after a reboot.
- [ ] A real game runs from the Flatpak (`flatpak run --command=kyty_emulator
      io.github.MrG00gle.KytyPS5_GTK --game <path>` and from the launcher).
- [ ] After the merge into `main`: the metainfo screenshot URL
      (`https://raw.githubusercontent.com/MrG00gle/KytyPS5-GTK/main/docs/screenshots/kyty-launcher-main.png`)
      returns the image, so the strict lint of the `main` run passes.
