# Configuration reference

The launcher stores its settings, the global emulator settings, and the settings of each game in one
settings file, `Kyty.ini`. **Global settings** edits the global configuration, which every game uses
unless it has its own settings (**Edit game config…** in the game menu). When the launcher starts a
game, it turns the game's settings into `kyty_emulator` options; the
[running guide](running.md#emulator-options) describes every option.

## Where the settings are stored

| Build | Settings file | Data directory |
|---|---|---|
| Release archive or source build, with a `Kyty.ini` in the directory you start the launcher from (portable) | `./Kyty.ini` | the directory that contains `kyty_emulator` |
| Release archive or source build, otherwise (user) | `$XDG_CONFIG_HOME/Kyty/Kyty.ini`, or `~/.config/Kyty/Kyty.ini` when `XDG_CONFIG_HOME` is unset or empty | the directory that contains `kyty_emulator` |
| Flatpak | `$XDG_CONFIG_HOME/Kyty/Kyty.ini` inside the sandbox, which is `~/.var/app/io.github.MrG00gle.KytyPS5_GTK/config/Kyty/Kyty.ini` | `~/.var/app/io.github.MrG00gle.KytyPS5_GTK/data/Kyty` |

Inside the Flatpak, only the user location applies: a `Kyty.ini` in the working directory is
ignored, and the launcher creates the `Kyty` directory when it is missing. Outside the Flatpak, the
portable file wins when it exists. **Global settings** shows the settings file and the data
directory of the current session as absolute paths. The
[architecture guide](architecture.md#data-directory) lists the contents of the data directory.

The file is an INI file in UTF-8, in the format that the upstream launcher writes, so files written
by the upstream launcher keep working. The launcher writes it atomically (a temporary file, then a rename) and keeps keys and
groups it does not know. When a file exists but cannot be loaded, the launcher asks before its next
save replaces it. A value that cannot be read, or that breaks the rules below, falls back to its
default when the file is loaded.

## Global configuration fields

These fields are stored in the `[GlobalConfiguration]` group. Every field of the first table can
also be set per game, in that game's entry of the `[GameConfigurations]` array. Booleans are `true`
or `false`, integers are decimal, and enumerated values are stored by name.

| Field | Default | Accepted values or range | Emulator option |
|---|---|---|---|
| `screen_resolution` | `R1280X720` | `R1280X720`, `R1920X1080`, `R2560X1440`, `R3840X2160` | `--screen-width` and `--screen-height` with that size |
| `user_name` | `Kyty` | 1–16 UTF-8 bytes after leading and trailing whitespace is removed | `--user-name` |
| `user_id` | `1000` | 0–2,147,482,879, except 254 and 255 | `--user-id` |
| `audio_input_device` | empty (no audio input) | any device name | `--mic`, left out when empty |
| `present_mode` | `Mailbox` | `Fifo`, `Mailbox`, `Immediate` | `--present-mode` |
| `gpu_index` | `-1` (automatic) | -1, or a device index from 0 | `--gpu`, left out when -1 |
| `fullscreen_enabled` | `false` | `true`, `false` | `--fullscreen` |
| `hdr_enabled` | `false` | `true`, `false` | `--hdr` |
| `hide_cursor_enabled` | `false` | `true`, `false` | `--hide-cursor` |
| `readback_linear_images` | `false` | `true`, `false` | `--readback-linear-images` |
| `readback_tiled_images` | `false` | `true`, `false` | `--readback-tiled-images` |
| `sync_raw_image_buffers` | `false` | `true`, `false` | `--sync-raw-image-buffers` |
| `tessellation_enabled` | `false` | `true`, `false` | `--tessellation` |
| `trophy_enabled` | `true` | `true`, `false` | `--no-trophy-notifications` when `false` |
| `trophy_duration_seconds` | `6` | 5–10 | `--trophy-duration` |
| `perf_overlay_enabled` | `false` | `true`, `false` | `--perf-overlay` |
| `skip_notice_screen` | `false` | `true`, `false` | `--skip-notice-screen` |
| `network_connected` | `false` | `true`, `false` | `--network-connected` |
| `vblank_frequency` | `60` | 30–360 | `--vblank-frequency` |
| `console_language` | `1` (English, United States) | 0–29 | `--console-language` |
| `vulkan_validation_enabled` | `false` | `true`, `false` | `--vulkan-validation` |
| `shader_validation_enabled` | `false` | `true`, `false` | `--shader-validation` |
| `shader_optimization_type` | `Performance` | `None`, `Size`, `Performance` | `--shader-optimization-type` |
| `shader_log_direction` | `Silent` | `Silent`, `Console`, `File` | `--shader-log-direction` |
| `shader_log_folder` | `_Shaders` | a path; not empty while `shader_log_direction` is `File` | `--shader-log-folder` |
| `command_buffer_dump_enabled` | `false` | `true`, `false` | `--command-buffer-dump` |
| `command_buffer_dump_folder` | `_Buffers` | a path; not empty while `command_buffer_dump_enabled` is `true` | `--command-buffer-dump-folder` |
| `printf_direction` | `Silent` | `Silent`, `Console`, `File` | `--printf-direction` |
| `printf_output_file` | `_kyty.txt` | a path; not empty while `printf_direction` is `File` | `--printf-output-file` |
| `profiler_enabled` | `false` | `true`, `false` | `--profile` |
| `renderdoc_enabled` | `false` | `true`, `false` | `--rd` |
| `amd_cpu_enabled` | `false` | `true`, `false` | `--amd-cpu` |
| `host_input_mapping` | empty (the built-in mapping) | a list of `Control=Input` entries, both sides non-empty | one `--keymap` per entry |

`screen_resolution` also reads the forms `1280x720` and the integer index of the value, which older
files may contain. Relative paths in `shader_log_folder`, `command_buffer_dump_folder`, and
`printf_output_file` are relative to the data directory. **Input mapping** in the main menu edits the
global `host_input_mapping`; the game menu's **Edit game input mapping…** edits a game's own list.

These fields exist only in the global configuration and apply to every game:

| Field | Group | Default | Accepted values or range | Emulator option |
|---|---|---|---|---|
| `controller_color` | `[GlobalConfiguration]` | empty (the game controls the lightbar) | empty or `#RRGGBB` | `--controller-color`, left out when empty |
| `controller_speaker_volume` | `[GlobalConfiguration]` | `50` | 0–100; other integers are clamped | `--controller-volume` |
| `controller_vibration_intensity` | `[GlobalConfiguration]` | `100` | 0–100; other integers are clamped | `--controller-vibration` |
| `controller_keyboard_player` | `[GlobalConfiguration]` | `0` (shares player 1 with the first gamepad) | 0–4; other integers are clamped | `--keyboard-player` |
| `game_dirs` | `[Launcher]` | empty | a list of folders | no option (the launcher scans these folders) |
| `addons_dir` | `[Launcher]` | empty | a folder | `--addcont-dir`, left out when empty |

These fields exist only in a game's own settings:

| Field | Default | Accepted values or range | Emulator option |
|---|---|---|---|
| `elf` | `eboot.bin` | a non-empty relative path without a `..` component | `--game <game directory>` for `eboot.bin` or a ZArchive game, otherwise `--game <game directory>/<elf>` |
| `input_mapping_custom` | `false` | `true`, `false` | no option; `true` makes the game use its own `host_input_mapping` instead of the global one |

When the launcher starts a game, it also passes `--game-patch <data directory>/_Patches/<TITLE_ID>.json`
when the game has a cheat file there.

## Other launcher settings

| Key | Group | Default | Meaning |
|---|---|---|---|
| `grid_view` | `[Launcher]` | `false` | Shows the game grid instead of the game list. |
| `grid_icon_width` | `[Launcher]` | `224` | Game image size in the grid: 128–320 in steps of 16. |
| `check_updates_on_startup` | `[MainDialog]` | `true` | Looks for a newer release at startup. Used only by release archive builds (see the [running guide](running.md#release-archive-and-the-updater)). |
| `width`, `height`, `maximized` | `[LauncherWindow]` | the window's default size | The main window size and state, restored at the next start. |
| `theme`, `geometry` | `[MainDialog]` | – | Written by the upstream launcher. The GTK launcher keeps them unchanged and follows the desktop's color scheme. |

## Game settings files

**Export settings…** in the game menu writes a game's configuration to a JSON file, and
**Import settings…** reads one back into a game. **Import…** and **Export…** under **Game configs**
in **Global settings** offer the same from the settings window, and the import can update several
games at once. An import changes only the fields that the file contains. A file with an unknown
field or an invalid value is rejected, and nothing changes.
