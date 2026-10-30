#pragma once

// The Emulator_Arguments for a run (task 15.1; design.md, "Game_Configuration fields"; Requirements
// 3.4, 10.1-10.3). Ported from the Qt launcher's CreateEmulatorArgs (src/launcher/src/
// mainDialog.cpp), which also gives the option order.

#include "launcher/core/environment.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/settingsStore.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

// The arguments after the Emulator's program name, one vector element per argv element (nothing
// is quoted: SpawnEmulator passes them without a shell). The design's LaunchContext (the add-ons
// directory and the cheat plan) is part of EffectiveConfiguration. Per the design's mapping table:
// - --screen-width and --screen-height from screen_resolution; --user-name with TrimWhitespace
//   applied; --user-id;
// - --mic only for a non-empty audio_input_device; --controller-color only for a non-empty color;
//   --controller-volume, --controller-vibration, --keyboard-player always;
// - --gpu only for a gpu_index of 0 or more (-1 is automatic);
// - each boolean field as its flag when true, except trophy_enabled, which gives
//   --no-trophy-notifications when false;
// - the enum fields as their key names, and every number in decimal;
// - one --keymap per entry of the effective input mapping that is IsValidMappingEntry, in order,
//   as stored; an invalid entry, which a Qt-written store may hold, is never passed (the
//   settings editor lists them with FindInvalidMappingEntries);
// - --game <entry.path> for an archive and for the elf "eboot.bin" (or an empty elf, as Qt);
//   otherwise --game <entry.path>/<elf>;
// - --addcont-dir for a non-empty addons_dir, and --game-patch for a cheat_plan;
// - never --redzone (Requirement 3.4).
std::vector<std::string> BuildEmulatorArguments(const GameEntry&              entry,
                                                const EffectiveConfiguration& effective);

// The cheat plan for ResolveEffectiveConfiguration (settingsStore.h): CheatFilePath(data_dir,
// title_id) (cheats.h) when IsCheatTitleId(title_id) and env.FileExists says that the file
// exists; nothing otherwise (Qt's Cheats::PlanPath plus QFileInfo::exists).
std::optional<std::filesystem::path> FindCheatPlan(const Environment&           env,
                                                   const std::filesystem::path& data_dir,
                                                   std::string_view             title_id);

} // namespace kyty::core
