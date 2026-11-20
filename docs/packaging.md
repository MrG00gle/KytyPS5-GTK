# Packaging

KytyPS5-GTK is packaged as a Flatpak with the App_ID `io.github.MrG00gle.KytyPS5_GTK`. The manifest
is `packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml`. The desktop entry, the AppStream
metainfo, and the icons are in `packaging/linux/`.

The Flatpak contains the launcher `kyty_launcher` (the application command), the emulator
`kyty_emulator` with its assets, the desktop entry, the metainfo, the icons, and the license files of
the project and of every component under `/app/share/licenses/io.github.MrG00gle.KytyPS5_GTK`. It
does not contain `kyty_updater`.

## Install and run

Each release on the [releases page](https://github.com/MrG00gle/KytyPS5-GTK/releases) has a
`KytyPS5-GTK-<date>-<commit>-x86_64.flatpak` bundle. CI also attaches a bundle to every workflow run
as the `flatpak-bundle` artifact; outside official releases, those bundles describe themselves as
development snapshots.

```bash
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./KytyPS5-GTK-<date>-<commit>-x86_64.flatpak
flatpak run io.github.MrG00gle.KytyPS5_GTK
```

Flatpak installs the GNOME 49 runtime and the graphics driver extension from Flathub when they are
missing. To run the emulator directly inside the sandbox:

```bash
flatpak run --command=kyty_emulator io.github.MrG00gle.KytyPS5_GTK --help
flatpak run --command=kyty_emulator io.github.MrG00gle.KytyPS5_GTK --game /games/ExampleGame
```

## Build from the manifest

The build needs `flatpak-builder` and, from Flathub, the GNOME 49 SDK and runtime and the LLVM 20
SDK extension:

```bash
flatpak install --user flathub org.gnome.Platform//49 org.gnome.Sdk//49 \
  org.freedesktop.Sdk.Extension.llvm20//25.08
```

The manifest builds the checkout it sits in (a `dir` source), so initialize the submodules first
(`git submodule update --init --recursive`). Every other source is pinned: git sources by commit and
the FFmpeg archive by SHA-256. The build itself runs without network access.

Run `flatpak-builder` from a directory outside the checkout. Its state directory (`.flatpak-builder/`
in the working directory by default) and the build directory would otherwise land in the checkout,
and the manifest's `dir` source would copy them into the next build:

```bash
mkdir -p ~/.cache/kyty-flatpak && cd ~/.cache/kyty-flatpak
flatpak-builder --force-clean --state-dir=state build \
  <checkout>/packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
```

To install the result into your user installation and run it:

```bash
cd ~/.cache/kyty-flatpak
flatpak-builder --user --install --force-clean --state-dir=state build \
  <checkout>/packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
flatpak run io.github.MrG00gle.KytyPS5_GTK
```

To make a single-file bundle instead, as CI does:

```bash
cd ~/.cache/kyty-flatpak
flatpak-builder --force-clean --state-dir=state --repo=repo build \
  <checkout>/packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
flatpak build-bundle repo kytyps5-gtk.flatpak io.github.MrG00gle.KytyPS5_GTK \
  --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./kytyps5-gtk.flatpak
```

`flatpak-builder --show-manifest <manifest>` prints the parsed manifest. It has no `--state-dir`
option and always writes `.flatpak-builder/` into its working directory, so run it from
`~/.cache/kyty-flatpak` too, never from the checkout.

The CTests `flatpak_manifest_structure`, `flatpak_pins_match_registry`, and
`flatpak_metainfo_content` check the manifest, its pins against `cmake/KytyDependencies.cmake`, and
the metainfo.

## Sandbox permissions

The manifest grants exactly these `finish-args`:

| Permission | Justification |
|---|---|
| `--share=ipc` | The X11 fallback needs the shared IPC namespace for X11 shared memory (MIT-SHM). |
| `--socket=wayland` | The launcher and the emulator show their windows as Wayland clients on Wayland sessions. |
| `--socket=fallback-x11` | On sessions without Wayland, the windows use X11. Flatpak grants the X11 socket only when no Wayland socket is available. |
| `--socket=pulseaudio` | Game audio output, microphone capture, and the DualSense speaker over USB, which is a USB audio device, all go through the host sound server. |
| `--device=all` | The emulator needs the GPU device nodes for Vulkan (`/dev/dri`, and the NVIDIA nodes), `/dev/input` for gamepads and their hotplug, and `/dev/hidraw*` for the DualSense haptics and lightbar over Bluetooth. No narrower device permission includes `/dev/hidraw*`. |
| `--share=network` | The launcher downloads the compatibility list and the cheat catalog, and games use the network when **Enable network connection** is on. |

The manifest grants no `--filesystem` permission, no `--talk-name`, and no access to the
`org.freedesktop.Flatpak` D-Bus service. Folder access goes through the desktop portals: the folder
chooser (FileChooser portal) gives the launcher and the emulator lasting access to each chosen game
folder through the Document portal, which keeps it across restarts and reboots. **Open game folder**
uses the OpenURI portal.

## Access to more folders

For a folder that is not chosen through the folder chooser, for example one that you pass to
`flatpak run --command=kyty_emulator … --game`, grant access with an override:

```bash
flatpak override --user --filesystem=/mnt/games io.github.MrG00gle.KytyPS5_GTK
```

Append `:ro` (`--filesystem=/mnt/games:ro`) for read-only access; compressing a game to a ZArchive
then fails, because the archive is written next to the game. `flatpak override --user --show
io.github.MrG00gle.KytyPS5_GTK` lists the overrides, and `flatpak override --user --reset
io.github.MrG00gle.KytyPS5_GTK` removes them.

## Data locations

Inside the sandbox, the settings file is `~/.var/app/io.github.MrG00gle.KytyPS5_GTK/config/Kyty/Kyty.ini`
and the data directory is `~/.var/app/io.github.MrG00gle.KytyPS5_GTK/data/Kyty`, with the save data,
trophies, cheats, logs, shader logs, dumps, the pipeline cache, and the RenderDoc captures. A
`Kyty.ini` in the working directory is ignored. The [configuration reference](configuration.md) and
the [architecture guide](architecture.md#data-directory) have the details.

## Updates

The Flatpak receives its updates through Flatpak, not through the launcher's Updater: it has no
`kyty_updater`, and the launcher shows no update controls and sends no update requests. Apply the
updates of the runtime and the graphics driver extensions with:

```bash
flatpak update
```

A release bundle carries no update source of its own, so a new KytyPS5-GTK release arrives as a new
bundle. Install it over the old one; the settings and data in
`~/.var/app/io.github.MrG00gle.KytyPS5_GTK` stay:

```bash
flatpak install --user --reinstall ./KytyPS5-GTK-<date>-<commit>-x86_64.flatpak
```

## Validation and lint exceptions

The CI `flatpak` job builds the bundle with flatpak-builder, then runs `flatpak-builder-lint` in
manifest mode on the manifest and in repository mode on the exported repository, `desktop-file-validate`
on the installed desktop entry, `appstreamcli validate --no-net` on the installed metainfo, and
`tools/checks/check_licenses.py` on the installed license files. It then installs the bundle and runs
`kyty_emulator --help` in it. The lint commands, as CI runs them:

```bash
flatpak-builder-lint --exceptions --user-exceptions packaging/flatpak/lint-exceptions.json \
  manifest packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
flatpak-builder-lint --exceptions --user-exceptions packaging/flatpak/lint-exceptions.json \
  repo <repository directory>
```

Locally, `flatpak-builder-lint` is part of the `org.flatpak.Builder` Flatpak on Flathub
(`flatpak run --command=flatpak-builder-lint org.flatpak.Builder …`).

`packaging/flatpak/lint-exceptions.json` lists the lint errors that the project accepts, each with
its justification:

| Lint error | Mode | Justification |
|---|---|---|
| `appid-url-not-reachable` | manifest and repository | The linter derives the URL `https://github.com/mrg00gle/kytyps5_gtk` from the App_ID and expects it to resolve. The repository's name contains a hyphen (`KytyPS5-GTK`), which an App_ID cannot contain, so that URL can never match. The metainfo's homepage is the real repository URL, `https://github.com/MrG00gle/KytyPS5-GTK`. |
| `appstream-screenshots-not-mirrored-in-ostree` | repository | The metainfo screenshot is served from this repository (`https://raw.githubusercontent.com/MrG00gle/KytyPS5-GTK/main/docs/screenshots/kyty-launcher-main.png`). The check requires a `screenshots/x86_64` ref in the exported repository, which only exists when flatpak-builder mirrors the screenshots into it (`--mirror-screenshots-url`, which Flathub's build service passes). A single-file bundle built here has no such mirror. |
| `appstream-external-screenshot-url` | repository | The check requires a screenshot URL on Flathub's media mirror (`https://dl.flathub.org/media`) in the exported repository's AppStream catalogue, and only Flathub's build infrastructure puts screenshots there when it publishes an app. This project does not publish on Flathub: its releases ship the `.flatpak` bundle and the Linux archive on GitHub Releases, so the screenshot stays at its URL in this repository on `raw.githubusercontent.com`. |

On every ref other than `main`, CI lints with a per-run copy of that file that also excepts
`appstream-missing-screenshots`: the screenshot URL points at `main` and resolves only after `dev`
is merged. Runs on `main` lint with the committed file unchanged, so a missing screenshot fails them.
The `appstream-external-screenshot-url` exception does not cover that case: when the URL does not
serve the image, the build leaves the screenshot out of the catalogue, and the linter reports
`appstream-missing-screenshots` instead.

The committed metainfo has an empty `<releases/>` list, because a commit cannot contain its own
release tag. On official runs (pushes to `main` of `MrG00gle/KytyPS5-GTK`), the `flatpak` job writes
one `<release>` entry per published GitHub release plus the tag that the run publishes, newest first,
into the build's copy of the metainfo, and the release job checks that the bundle has the entry for
its tag. Every other run writes the published releases plus its own tag as a `type="development"`
entry, because the linter rejects an empty release list.

Both lists keep one entry per tag date: the `<date>` in `KytyPS5-GTK-<date>-<commit>`, which is
the UTC date of the commit, not the day the release is published. AppStream orders releases by
version, and two tags of one date differ only in their commit hash, which does not sort by age, so
a second entry would fail `appstreamcli validate` (`releases-not-in-order`). The run's own tag
keeps its entry, so every bundle lists its own version. For every other tag date, the list keeps
the most recently published release. A second release with the same tag date therefore replaces
the first in the version history of later bundles, and the first stays on the releases page with
its own bundle.

## The runtimes in CI

The `flatpak` job runs in the `ghcr.io/flathub-infra/flatpak-github-actions:gnome-49` image, which
has the GNOME 49 runtime and SDK preinstalled in `/var/lib/flatpak`. The flatpak-builder action
passes `--install-deps-from=flathub`, so flatpak-builder updates the SDK and the runtime to the
newest build of their branches and installs the LLVM 20 extension: CI builds against the same
`49` and `25.08` branches as a local build from the manifest.

The image keeps `/var/lib/flatpak` in a read-only image layer. On some runners the container's
overlay file system cannot rename the directories of such a layer, so updating a runtime in place
failed with `Invalid cross-device link`. flatpak-builder 1.4.9 then crashed on that error with a
segmentation fault (exit code 139) instead of reporting it. The job therefore copies
`/var/lib/flatpak` to the runner's own file system (`$RUNNER_TEMP/flatpak-system`, about 4 GB)
before the action, and points `FLATPAK_SYSTEM_DIR` at the copy for the later steps.
`tools/checks/workflow_check.py` checks that the job builds with that action and that this step
comes before it.

Before the action runs, the job lists the image's tools and the version and commit of the
preinstalled GNOME runtime and SDK in its job summary and in one notice on the run page. The
digest of the image itself is in the log of the job's "Initialize containers" step.

## Release notes

Before it publishes a release, the `release` job writes the release's text with
`tools/ci/release_notes.py`. The text comes from one of two places:

- **Notes written by hand.** When the text of
  [`packaging/release-notes.md`](../packaging/release-notes.md) outside its HTML comments changed
  since the previous release (the newest published release before this commit), that text is the
  release's text, as written.
- **Otherwise, the commit subjects** since the previous release, at most the newest 100, grouped
  by the area at the start of each subject: Launcher, Updater, Emulator (with the emulator's
  subsystems, such as `shader`), and Packaging. Changes to the tests, the build, CI, and the
  documentation, and subjects without an area, are folded under "Other changes". A link to the
  full changelog ends the text.

To write notes for the next release, replace everything below the comment in
`packaging/release-notes.md` with the new notes in Markdown, and commit the file to `dev` before
`dev` is merged into `main`. All text outside the comment becomes the release's text, so notes of
an earlier release that stay next to the new ones are published again. The notes are used for that
one release only. At the release after it, the file still has the same text, so that release lists
the commit subjects again, and the file does not need to be cleared after a release. A change to
the comment alone does not count as new notes.

CI never changes the text of a release that is already published. A rerun of the `release` job
leaves such a release as it is, so a text that you edit on its GitHub release page stays. The
**Release notes** button of the launcher's update offer opens that page.

## Re-running a release

To re-run a run that has published its release, re-run only its `release` job:
**Re-run failed jobs**, or **Re-run job** on `release` alone. That job finds the complete release
and succeeds without changes. Never use **Re-run all jobs** on such a run. The rebuilt release
archive and bundle no longer match the digests of the published assets, so the `release` job
fails. On a later day the `flatpak` job fails first, because the tag is already published with an
earlier date. Such a rerun fails without changing or deleting anything.

## Runtime and its support window

The Flatpak targets GNOME 49: the runtime `org.gnome.Platform//49`, the SDK `org.gnome.Sdk//49`, and
the SDK extension `org.freedesktop.Sdk.Extension.llvm20//25.08` for Clang and lld (25.08 is the
freedesktop SDK branch that the GNOME 49 SDK is based on). GNOME runtimes get about a year of
support on Flathub. Move to a newer GNOME runtime when Flathub announces the end of life of GNOME 49
at the latest; `flatpak remote-info flathub org.gnome.Platform//49` then shows it, and
`flatpak-builder-lint` already warns with `runtime-update-available-…` while a newer runtime exists.
No CI check watches the date, so the maintainer has to.

To move to GNOME `<v>`:

1. Check that the runtime and the SDK exist on Flathub:
   `flatpak remote-info flathub org.gnome.Platform//<v>` and
   `flatpak remote-info flathub org.gnome.Sdk//<v>`.
2. Find the freedesktop SDK branch of the new SDK: in
   `flatpak remote-info --show-metadata flathub org.gnome.Sdk//<v>`, the
   `[Extension org.freedesktop.Sdk.Extension]` group has `version = <branch>`. Choose the LLVM
   extension that exists on that branch, for example
   `flatpak remote-info flathub org.freedesktop.Sdk.Extension.llvm20//<branch>`.
3. In the manifest, change `runtime-version` (the `sdk` follows it), the `sdk-extensions` entry, and
   the extension's directories in `build-options` (`append-path` and `prepend-ld-library-path`,
   `/usr/lib/sdk/llvm<N>/…`), and the comment there.
4. Update the constants of `tools/checks/flatpak_checks.py` (`RUNTIME_VERSION`, `LLVM_EXTENSION`) and
   its tests in `tools/checks/tests/test_flatpak_checks.py`, in the same commit.
5. Change the `flatpak` job's container image in `.github/workflows/build.yml` to
   `ghcr.io/flathub-infra/flatpak-github-actions:gnome-<v>` (check that the tag exists), and the same
   image in `tools/checks/workflow_check.py` and `tools/checks/tests/test_workflow_check.py`.
6. Update the GNOME version in this guide and in the README's system requirements.
7. Install the new SDK, runtime, and extension (`flatpak install --user flathub
   org.gnome.Platform//<v> org.gnome.Sdk//<v> org.freedesktop.Sdk.Extension.llvm<N>//<branch>`), then
   rebuild from scratch with `flatpak-builder --force-clean` as above, outside the checkout.
8. Run the lint in manifest and repository mode, `desktop-file-validate`, `appstreamcli validate
   --no-net`, and `check_licenses.py` on the new build, run it with `flatpak run`, and run the CTests
   `flatpak_manifest_structure`, `flatpak_pins_match_registry`, `flatpak_metainfo_content`, and
   `workflow_structure`.
