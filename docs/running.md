# Running KytyPS5-GTK

`kyty_launcher` is the GTK launcher: it finds your games, edits their settings, starts the emulator,
and shows the emulator's output. `kyty_emulator` is the emulator itself, which you can also start
from a terminal. Use only legally obtained game files.

Start the launcher from a build or a release archive with `./kyty_launcher`, or from the Flatpak
with `flatpak run io.github.MrG00gle.KytyPS5_GTK` (see the [packaging guide](packaging.md)).

## First launch

The first start shows an empty game list with **Add game folder**. The launcher keeps its settings
in `Kyty.ini`. A `Kyty.ini` in the directory you start the launcher from is used as a portable
settings file; otherwise the launcher uses `~/.config/Kyty/Kyty.ini`. The Flatpak always uses its
own configuration directory. **Global settings** shows the settings file and the data directory in
use. The [configuration reference](configuration.md) lists every setting.

The launcher looks for `kyty_emulator` next to itself, then in its parent directory, so it finds the
emulator both in an installed tree and in a build directory. Outside the Flatpak, the directory of
`kyty_emulator` is also the data directory, where save data, trophies, cheats, and logs go, and the
emulator runs with it as its working directory. The [architecture guide](architecture.md#data-directory)
lists everything in it.

If no Wayland or X11 display can be opened, the launcher prints an error and exits with a non-zero
status within 5 seconds, without touching the settings.

## Adding game folders

Add the folders that hold your games with **Add game folder** in the empty game list, with
**Add game folder…** in the main menu, or with **Add folders…** under **Game folders** in
**Global settings**. The launcher
searches every game folder recursively and lists:

- each directory that contains `eboot.bin`;
- each ZArchive (`.zar`) file whose archive root contains `eboot.bin`.

**Refresh game list** in the main menu scans the folders again. Folders that cannot be read are
listed in a message, and the rest of the list stays usable. In the Flatpak, a folder chosen in the
folder chooser stays readable after the launcher restarts and after a reboot, until you remove it
from the game folders. The add-ons (DLC) folder under **Add-ons** in **Global settings** works the
same way.

Select a game and press **Run**, press Enter, or double-click it. The game menu also opens the
game's folder, its trophies, its additional content, its cheats (for `PPSA` titles), and its own
settings and input mapping, and it can compress the game to a ZArchive or remove its save data.

## ZArchive games

A ZArchive (`.zar`) file is a compressed, read-only copy of a game directory. The emulator mounts it
read-only and streams it, so it does not need to be extracted first. Run it from the game list like
any other game, or pass the archive to `kyty_emulator --game`.

To create one, choose **Compress to ZAR…** in the game menu of a directory game. The launcher writes
`<game directory>.zar` next to the directory and shows the progress, and **Cancel** stops it and
deletes the partial archive. When the archive is complete, the launcher asks whether to delete the
original directory (keeping it is the default) and scans the game folders again, so the new entry
appears. The launcher refuses to overwrite an existing `.zar` file.

## The log view

**Show log** in the main menu opens the log view. The launcher clears it when a game starts and then
shows every line that the emulator writes to standard output or standard error, within a second of
the write and in the order of each stream. Invalid UTF-8 shows as the replacement character (U+FFFD),
and the view keeps at least the latest 100,000 lines. **Save log…** writes the lines to a UTF-8 text
file.

The emulator writes its log only when **Printf output** on the **Debug** page of the settings is
`Console` or `File`. Set it in **Global settings**, or with **Edit game config…** when the game has
its own settings. With the default, `Silent`, the log view shows only a few lines. For a bug report,
set **Printf output** to `Console` before you reproduce the problem, then save the log view with
**Save log…**. With `File`, the emulator writes the log to `_kyty.txt` in the data directory instead
([architecture guide](architecture.md#data-directory)). Attach the log, unedited, to bug reports.
Logging can slow the emulator down, so set **Printf output** back to `Silent` afterwards.

**Stop** asks the emulator to close (SIGTERM). If it is still running 5 seconds later, the launcher
ends it (SIGKILL). When the emulator exits with a non-zero exit code or is ended by a signal that you
did not request, the launcher shows the exit code or the signal name and adds it to the log view.
The log stays until the next game starts.

## Release archive and the Updater

Each release on the [releases page](https://github.com/MrG00gle/KytyPS5-GTK/releases) has a
`KytyPS5-GTK-<date>-<commit>-Linux-x86_64.tar.gz` archive, with a `.sha256` file next to it. Extract
it into a writable folder and run `./kyty_launcher` there.

The release archive is the only build with the Updater. **Check for updates** (and **Check for
updates on startup** in **Global settings**) looks for a newer release of this fork, downloads its
archive, checks its SHA-256 checksum and size, and installs it with `kyty_updater` after the
launcher quits. Close the running game first. Files outside the release package, such as settings,
saves, and patches, stay, and a failed installation restores the previous files. The next start
shows whether the installation succeeded. The installation folder must be writable.

Source builds, development builds, and builds of other repositories hide the update controls and
show a link to the releases page instead. The Flatpak has no Updater at all: it gets its updates
through Flatpak (see the [packaging guide](packaging.md#updates)).

## Running the emulator directly

```bash
./kyty_emulator --game "/games/ExampleGame"
./kyty_emulator --game "/games/ExampleGame.zar"
./kyty_emulator --game "/games/ExampleGame" --screen-width 1920 --screen-height 1080 --fullscreen
```

`--game` is required. It accepts a game directory, an ELF file of a game, or a ZArchive file. Paths
may contain spaces, quotes, and non-ASCII characters, but the emulator reads a backslash in a path
as a `/` separator, so the launcher warns about game paths that contain one. The emulator
prints the error and the usage text and exits with status 1 for an unknown option, a missing or
invalid value, or a `--game` path that does not exist or cannot be read. `--help` (or `-h`) prints
the usage text and exits with status 0.

`--game`, `--game-patch`, and `--addcont-dir` may be given once. For the other options that take a
value, the last one wins, and `--keymap` may be repeated. Relative paths are relative to the
emulator's working directory. Inside the Flatpak, `--game`, `--game-patch`, and `--addcont-dir` are
taken relative to the directory the emulator was started from, and the relative log, shader, and
dump paths are taken relative to the data directory.

To use another Vulkan loader, set `SDL_VULKAN_LIBRARY`:

```bash
SDL_VULKAN_LIBRARY=/path/to/libvulkan.so.1 ./kyty_emulator --game "/games/ExampleGame"
```

## Emulator options

Every option of `kyty_emulator --help`. A flag takes no value; its default is off unless the table
says otherwise. Enumerated values are case-sensitive.

| Option | Value format | Accepted values or range | Default | Effect |
|---|---|---|---|---|
| `--game` | `<dir\|elf\|zar>`: a path | An existing, readable game directory, ELF file, or `.zar` file whose root contains `eboot.bin` | none (required) | The game to load. |
| `--game-patch` | `<json>`: a path | An existing ETAHen cheat file | none | Applies the cheats of this file. |
| `--addcont-dir` | `<dir>`: a path | Any folder | none | The add-ons (DLC) folder. |
| `--screen-width` | `<num>`: an integer | 1 or more | 1280 | Window width. |
| `--screen-height` | `<num>`: an integer | 1 or more | 720 | Window height. |
| `--user-name` | `<name>`: text | 1–16 bytes | `Kyty` | Local user name exposed to games. |
| `--user-id` | `<num>`: an integer | 0–2,147,482,879, except 254 and 255 | 1000 | Local user ID exposed to games. |
| `--mic` | `<name>`: a device name | Any audio input device name | none (silence) | Captures audio from this microphone. |
| `--controller-color` | `<#RRGGBB>` | `#` and 6 hexadecimal digits | none (the game controls the lightbar) | Overrides the controller lightbar color. |
| `--controller-volume` | `<0-100>`: an integer | 0–100 | 50 | DualSense speaker volume. |
| `--controller-vibration` | `<0-100>`: an integer | 0–100 | 100 | DualSense vibration intensity. |
| `--keyboard-player` | `<0-4>`: an integer | 0–4 | 0 | The player that the keyboard controls; 0 shares player 1 with the first gamepad. |
| `--present-mode` | `<value>` | `Fifo`, `Mailbox`, `Immediate` | `Mailbox` | Vulkan presentation mode. |
| `--gpu` | `<index>`: an integer | A Vulkan physical device index; a negative value or an index without a device selects automatically | automatic | The GPU to use. Automatic selection prefers a discrete GPU, then an integrated one. |
| `--fullscreen` | flag | – | off | Runs in borderless desktop fullscreen. |
| `--hdr` | flag | – | off | Enables HDR10 output when supported. |
| `--hide-cursor` | flag | – | off | Hides the cursor after 2 seconds without mouse activity. |
| `--no-splash` | flag | – | splash on | Disables the startup splash screen. |
| `--vr` | flag | – | off | Enables the virtual VR headset. |
| `--amd-cpu` | flag | – | off | Applies AMD CPU instruction patches. |
| `--vblank-frequency` | `<num>`: an integer | 30–360 (other values are clamped to that range) | 60 | Virtual vblank frequency in Hz. |
| `--console-language` | `<0-29>`: an integer | 0–29 | 1 (English, United States) | Console language. |
| `--vulkan-validation` | flag | – | off | Enables the Vulkan validation layers. |
| `--network-connected` | flag | – | off | Lets games use the host's local network. |
| `--gpu-assisted-validation` | flag | – | off | Bounds-checks shader accesses on the GPU. Implies `--vulkan-validation`; very slow. |
| `--shader-validation` | flag | – | off | Enables shader validation. |
| `--tessellation` | flag | – | off | Draws tessellation patches, which are skipped by default. |
| `--shader-optimization-type` | `<value>` | `None`, `Size`, `Performance` | `None` | Shader optimization. The launcher's own default is `Performance`. |
| `--shader-log-direction` | `<value>` | `Silent`, `Console`, `File` | `Silent` | Where shader logs go. |
| `--shader-log-folder` | `<path>` | Any folder | `_Shaders` | Shader log folder for `File`. |
| `--command-buffer-dump` | flag | – | off | Enables command buffer dumps. |
| `--command-buffer-dump-folder` | `<path>` | Any folder | `_Buffers` | Command buffer dump folder. |
| `--graphics-debug-dump` | flag | – | off | Enables graphics debug dumps. |
| `--printf-direction` | `<value>` | `Silent`, `Console`, `File` | `Silent` | Where the emulator's log, including the game's printf output, goes: `Silent` writes none, `Console` writes it to standard output (shown in the launcher's log view), and `File` writes it to the `--printf-output-file` file. |
| `--printf-output-file` | `<path>` | Any file | `_kyty.txt` | The log file for `File`. |
| `--profile` | flag | – | off | Enables the Tracy profiler. |
| `--spirv-debug-printf` | flag | – | off | Enables SPIR-V debug printf. |
| `--readback-linear-images` | flag | – | off | Reads back writable linear images on submit. |
| `--readback-tiled-images` | flag | – | off | Reads back small writable tiled images on submit. |
| `--sync-raw-image-buffers` | flag | – | off | Synchronizes raw reads of GPU images. |
| `--no-trophy-notifications` | flag | – | notifications on | Disables trophy unlock notifications and their sound. |
| `--trophy-duration` | `<seconds>`: an integer | 5–10 (other values are clamped to that range) | 6 | How long a trophy notification stays on screen. |
| `--perf-overlay` | flag | – | off | Shows the performance panel (toggle with F2). |
| `--playgo-hack` | flag | – | off | Uses the supplied PlayGo stub fallback. |
| `--skip-notice-screen` | flag | – | off | Skips startup logos and notices in supported games. |
| `--keymap` | `<Control=Input>` | A DualSense control and a host input, both non-empty | the built-in mapping | Maps one DualSense control; may be repeated. |
| `--rd` | flag | – | off | Enables RenderDoc capture. |
| `--guest-exec` | `<elf> [args...]`: a path and arguments | A guest ELF; every argument after it goes to that ELF | `eboot.bin` of the game | Runs this guest ELF instead of `eboot.bin`. It must be the last option. |
| `--help` | flag | – | – | Prints the usage text and exits. |
| `-h` | flag | – | – | Same as `--help`. |

The launcher builds these options from the game's settings; the
[configuration reference](configuration.md) shows which setting sets which option.
