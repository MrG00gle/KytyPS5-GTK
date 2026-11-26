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

The file is an INI file in UTF-8, in the format the upstream launcher writes, so its files keep
working. The launcher writes it atomically (a temporary file, then a rename) and keeps keys and
groups it does not know. When a file exists but cannot be loaded, the launcher asks before its next
save replaces it. A value that cannot be read, or that breaks the rules below, falls back to its
default when the file is loaded.

## Global configuration fields

These fields are stored in the `[GlobalConfiguration]` group. Every field of the first table can
also be set per game, in that game's entry of the `[GameConfigurations]` array. Booleans are `true`
or `false`, integers are decimal, and enumerated values are stored by name.

| Field | Default | Accepted values or range | Emulator option |
|---|---|---|---|
| `screen_resolution` | `1280x720` | `<width>x<height>`, each side 320–16384 | `--screen-width` and `--screen-height` with that size |
| `user_name` | `Kyty` | 1–16 UTF-8 bytes after leading and trailing whitespace is removed | `--user-name` |
| `user_id` | `1000` | 0–2,147,482,879, except 254 and 255 | `--user-id` |
| `audio_input_device` | empty (no audio input) | any device name | `--mic`, left out when empty |
| `present_mode` | `Mailbox` | `Fifo`, `Mailbox`, `Immediate` | `--present-mode` |
| `gpu_index` | `-1` (automatic) | -1, or a device index from 0 | `--gpu`, left out when -1 |
| `fullscreen_enabled` | `false` | `true`, `false` | `--fullscreen` |
| `hdr_enabled` | `false` | `true`, `false` | `--hdr` |
| `hide_cursor_enabled` | `false` | `true`, `false` | `--hide-cursor` |
| `stretch_enabled` | `false` | `true`, `false` | `--stretch` |
| `readback_linear_images` | `false` | `true`, `false` | `--readback-linear-images` |
| `readback_tiled_images` | `false` | `true`, `false` | `--readback-tiled-images` |
| `readback_storage_images` | `false` | `true`, `false` | `--readback-storage-images` |
| `sync_raw_image_buffers` | `false` | `true`, `false` | `--sync-raw-image-buffers` |
| `flush_draws` | `0` (off) | 0–2,147,483,647 | `--flush-draws` |
| `tessellation_enabled` | `false` | `true`, `false` | `--tessellation` |
| `occlusion_queries_enabled` | `false` | `true`, `false` | `--occlusion-queries` |
| `relaxed_shader_float` | `false` | `true`, `false` | `--relaxed-shader-float` |
| `native_rt` | `false` | `true`, `false` | `--native-rt` |
| `shader_cache_enabled` | `true` | `true`, `false` | `--shader-cache` |
| `trophy_enabled` | `true` | `true`, `false` | `--no-trophy-notifications` when `false` |
| `trophy_duration_seconds` | `6` | 5–10 | `--trophy-duration` |
| `perf_overlay_enabled` | `false` | `true`, `false` | `--perf-overlay` |
| `skip_notice_screen` | `false` | `true`, `false` | `--skip-notice-screen` |
| `network_connected` | `false` | `true`, `false` | `--network-connected` |
| `psn_emulation_enabled` | `false` | `true`, `false` | `--emulate-psn` |
| `vblank_frequency` | `60` | 30–360 | `--vblank-frequency` |
| `console_language` | `1` (English, United States) | 0–29 | `--console-language` |
| `vulkan_validation_enabled` | `false` | `true`, `false` | `--vulkan-validation` |
| `shader_validation_enabled` | `false` | `true`, `false` | `--shader-validation` |
| `shader_log_direction` | `Silent` | `Silent`, `Console`, `File` | `--shader-log-direction` |
| `shader_log_folder` | `_Shaders` | a path; not empty while `shader_log_direction` is `File` | `--shader-log-folder` |
| `command_buffer_dump_enabled` | `false` | `true`, `false` | `--command-buffer-dump` |
| `command_buffer_dump_folder` | `_Buffers` | a path; not empty while `command_buffer_dump_enabled` is `true` | `--command-buffer-dump-folder` |
| `printf_direction` | `Silent` | `Silent`, `Console`, `File` | `--printf-direction` |
| `printf_output_file` | `_kyty.txt` | a path; not empty while `printf_direction` is `File` | `--printf-output-file` |
| `profiler_enabled` | `false` | `true`, `false` | `--profile` |
| `renderdoc_enabled` | `false` | `true`, `false` | `--rd` |
| `pro_enabled` | `false` | `true`, `false` | `--trinity` |
| `pssr_upscaler` | `Linear` | `Linear`, `DLSS`, `FSR` | `--pssr-upscaler`, only with `pro_enabled` |
| `dlss_folder` | empty | a folder | `--dlss-folder`, only with `pro_enabled` and `DLSS` |
| `dlss_preset` | `Default` (the model's own) | `Default`, or a letter from `A` to `O` | `--dlss-preset` with the letter, only with `pro_enabled` and `DLSS`, left out for `Default` |
| `fsr_folder` | empty | a folder | `--fsr-folder`, only with `pro_enabled` and `FSR` |
| `amd_cpu_enabled` | `false` | `true`, `false` | `--amd-cpu` |
| `guest_tsc_enabled` | `false` | `true`, `false` | `--guest-tsc` |
| `host_input_mapping` | empty (the built-in mapping) | a list of `Control=Input` entries, both sides non-empty | one `--keymap` per entry |

`screen_resolution` is written as the upstream launcher writes it, for example `1920x1080`. It also
reads the forms that older files contain: the old values `R1280X720`, `R1920X1080`, `R2560X1440`,
and `R3840X2160`, an upper-case `X`, spaces around the numbers, and the integer index (`0`–`3`) of
the four old values. **Global settings** offers the presets 1280 × 720, 1920 × 1080, 2560 × 1440,
3840 × 2160, and the ultrawide 2560 × 1080, 3440 × 1440, 3840 × 1600, and 5120 × 2160, or any width
and height. Relative paths in `shader_log_folder`, `command_buffer_dump_folder`, and
`printf_output_file` are relative to the data directory. **Input mapping** in the main menu edits the
global `host_input_mapping`; the game menu's **Edit game input mapping…** edits a game's own list.

Older files can contain `shader_optimization_type`. The launcher no longer uses or passes it, and
keeps an existing value in `Kyty.ini` unchanged.

Files from older versions do not have the newer fields `stretch_enabled`,
`readback_storage_images`, `flush_draws`, `occlusion_queries_enabled`,
`relaxed_shader_float`, `native_rt`, `shader_cache_enabled`, `psn_emulation_enabled`,
`pro_enabled`, `pssr_upscaler`, `dlss_folder`, `dlss_preset`, `fsr_folder`, and
`guest_tsc_enabled`. Those fields get their defaults. `shader_cache_enabled` is on by default, as in
the upstream launcher, and the emulator keeps the caches in `_PipelineCache` in the
[data directory](architecture.md#data-directory).

**Global settings** offers `Linear` and `DLSS` as the PSSR upscaler. `FSR` and `fsr_folder` are
read and kept so that the upstream launcher's files load, but FSR has no effect on Linux: the
settings window shows `FSR` as `Linear` and does not show `fsr_folder`. DLSS, its model folder, and
`dlss_preset` are not tested by this project's CI. With DLSS selected, the settings window needs a
model folder; with no folder, the emulator does not use DLSS. `dlss_preset` is this launcher's own
field: the upstream launcher does not have it.

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

A game's own add-ons, which **Additional content…** in the game menu adds and removes, are stored
under the game's title ID in the `[AdditionalContent]` group, as the upstream launcher stores them:
`<TITLE_ID>` is a list of entries, each `<label>` for a license only or `<label>=<folder>` for an
add-on with its data in that folder (`<label>` is the add-on's 16-character entitlement label).
The launcher passes each entry as `--addcont <entry>`, after `--addcont-dir`. The emulator does not
start while an entry names a folder that does not exist; the add-ons window shows such an entry as
"Data folder not found".

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
field or an invalid value is rejected, and nothing changes. The one exception is
`shader_optimization_type`, which files exported by older versions contain: the import ignores it.
