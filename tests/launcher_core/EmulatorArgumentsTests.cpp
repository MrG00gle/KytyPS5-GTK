// Example tests of the Emulator_Arguments (src/launcher/core/emulatorArguments.h, task 15.1): every
// row of the design's mapping table, both --game forms, the omitted options, the input mapping
// filter, per-game and global mappings, the cheat plan, and the absence of --redzone.

#include "launcher/core/emulatorArguments.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/settingsStore.h"
#include "support/fakeEnvironment.h"
#include "support/noDisplay.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using kyty::core::BuildEmulatorArguments;
using kyty::core::EffectiveConfiguration;
using kyty::core::GameConfiguration;
using kyty::core::GameEntry;
using Args = std::vector<std::string>;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "EmulatorArgumentsTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Show(const Args& args) {
	std::string text;
	for (const auto& arg: args) {
		text += (text.empty() ? "'" : " '") + arg + "'";
	}
	return text;
}

GameEntry DirectoryEntry(const std::filesystem::path& path = "/games/A") {
	GameEntry entry;
	entry.path     = path;
	entry.kind     = GameEntry::Kind::Directory;
	entry.title_id = "PPSA01234";
	return entry;
}

GameEntry ArchiveEntry() {
	GameEntry entry;
	entry.path = "/games/B.zar";
	entry.kind = GameEntry::Kind::Archive;
	return entry;
}

Args Build(const EffectiveConfiguration& effective, const GameEntry& entry = DirectoryEntry()) {
	auto args = BuildEmulatorArguments(entry, effective);
	Check(std::find(args.begin(), args.end(), "--redzone") == args.end(),
	      "--redzone is never passed: " + Show(args));
	return args;
}

Args Build(const GameConfiguration& configuration, const GameEntry& entry = DirectoryEntry()) {
	EffectiveConfiguration effective;
	effective.configuration = configuration;
	return Build(effective, entry);
}

std::size_t Count(const Args& args, const std::string& name) {
	return static_cast<std::size_t>(std::count(args.begin(), args.end(), name));
}

// The value after the only occurrence of name, or nothing when name is absent.
std::optional<std::string> ValueOf(const Args& args, const std::string& name) {
	Check(Count(args, name) <= 1, name + " is passed at most once: " + Show(args));
	const auto it = std::find(args.begin(), args.end(), name);
	if (it == args.end()) {
		return std::nullopt;
	}
	Check(it + 1 != args.end(), name + " has a value: " + Show(args));
	return *(it + 1);
}

void CheckValue(const Args& args, const std::string& name, const std::string& expected) {
	const auto value = ValueOf(args, name);
	Check(value == expected, name + " " + expected + ": " + Show(args));
}

void CheckAbsent(const Args& args, const std::string& name) {
	Check(Count(args, name) == 0, name + " is omitted: " + Show(args));
}

std::vector<std::string> Keymaps(const Args& args) {
	std::vector<std::string> keymaps;
	for (std::size_t i = 0; i + 1 < args.size(); i++) {
		if (args[i] == "--keymap") {
			keymaps.push_back(args[++i]);
		}
	}
	return keymaps;
}

void TestDefaults() {
	const Args expected = {"--screen-width",
	                       "1280",
	                       "--screen-height",
	                       "720",
	                       "--user-name",
	                       "Kyty",
	                       "--user-id",
	                       "1000",
	                       "--controller-volume",
	                       "50",
	                       "--controller-vibration",
	                       "100",
	                       "--keyboard-player",
	                       "0",
	                       "--present-mode",
	                       "Mailbox",
	                       "--trophy-duration",
	                       "6",
	                       "--vblank-frequency",
	                       "60",
	                       "--console-language",
	                       "1",
	                       "--shader-optimization-type",
	                       "Performance",
	                       "--shader-log-direction",
	                       "Silent",
	                       "--shader-log-folder",
	                       "_Shaders",
	                       "--command-buffer-dump-folder",
	                       "_Buffers",
	                       "--printf-direction",
	                       "Silent",
	                       "--printf-output-file",
	                       "_kyty.txt",
	                       "--game",
	                       "/games/A"};
	const auto args     = Build(EffectiveConfiguration {});
	Check(args == expected, "the default arguments: " + Show(args));
}

// Every option at once, in the Qt launcher's order.
void TestEveryOptionInOrder() {
	EffectiveConfiguration effective;
	auto&                  c      = effective.configuration;
	c.screen_resolution           = kyty::core::ScreenResolution::R3840X2160;
	c.user_name                   = " Player One ";
	c.user_id                     = kyty::core::kMaxUserId;
	c.audio_input_device          = "USB Mic";
	c.present_mode                = kyty::core::PresentMode::Immediate;
	c.gpu_index                   = 2;
	c.fullscreen_enabled          = true;
	c.hdr_enabled                 = true;
	c.hide_cursor_enabled         = true;
	c.readback_linear_images      = true;
	c.readback_tiled_images       = true;
	c.sync_raw_image_buffers      = true;
	c.tessellation_enabled        = true;
	c.trophy_enabled              = false;
	c.trophy_duration_seconds     = 10;
	c.perf_overlay_enabled        = true;
	c.skip_notice_screen          = true;
	c.network_connected           = true;
	c.vblank_frequency            = 360;
	c.console_language            = 29;
	c.vulkan_validation_enabled   = true;
	c.shader_validation_enabled   = true;
	c.shader_optimization_type    = kyty::core::ShaderOptimizationType::None;
	c.shader_log_direction        = kyty::core::LogDirection::File;
	c.shader_log_folder           = "logs/shaders";
	c.command_buffer_dump_enabled = true;
	c.command_buffer_dump_folder  = "dumps";
	c.printf_direction            = kyty::core::LogDirection::Console;
	c.printf_output_file          = "out.txt";
	c.profiler_enabled            = true;
	c.renderdoc_enabled           = true;
	c.amd_cpu_enabled             = true;
	c.elf                         = "bin/game.elf";
	c.host_input_mapping          = {"Cross=J", "Pad:Cross=South"};
	effective.controller          = {"#0070D1", 0, 100, 4};
	effective.addons_dir          = "/addons";
	effective.cheat_plan          = "/data/_Patches/PPSA01234.json";
	const Args expected           = {"--screen-width",
	                                 "3840",
	                                 "--screen-height",
	                                 "2160",
	                                 "--user-name",
	                                 "Player One",
	                                 "--user-id",
	                                 "2147482879",
	                                 "--mic",
	                                 "USB Mic",
	                                 "--controller-color",
	                                 "#0070D1",
	                                 "--controller-volume",
	                                 "0",
	                                 "--controller-vibration",
	                                 "100",
	                                 "--keyboard-player",
	                                 "4",
	                                 "--present-mode",
	                                 "Immediate",
	                                 "--gpu",
	                                 "2",
	                                 "--fullscreen",
	                                 "--hdr",
	                                 "--hide-cursor",
	                                 "--readback-linear-images",
	                                 "--readback-tiled-images",
	                                 "--sync-raw-image-buffers",
	                                 "--no-trophy-notifications",
	                                 "--trophy-duration",
	                                 "10",
	                                 "--perf-overlay",
	                                 "--skip-notice-screen",
	                                 "--network-connected",
	                                 "--tessellation",
	                                 "--vblank-frequency",
	                                 "360",
	                                 "--console-language",
	                                 "29",
	                                 "--vulkan-validation",
	                                 "--shader-validation",
	                                 "--shader-optimization-type",
	                                 "None",
	                                 "--shader-log-direction",
	                                 "File",
	                                 "--shader-log-folder",
	                                 "logs/shaders",
	                                 "--command-buffer-dump",
	                                 "--command-buffer-dump-folder",
	                                 "dumps",
	                                 "--printf-direction",
	                                 "Console",
	                                 "--printf-output-file",
	                                 "out.txt",
	                                 "--profile",
	                                 "--amd-cpu",
	                                 "--keymap",
	                                 "Cross=J",
	                                 "--keymap",
	                                 "Pad:Cross=South",
	                                 "--rd",
	                                 "--game",
	                                 "/games/A/bin/game.elf",
	                                 "--addcont-dir",
	                                 "/addons",
	                                 "--game-patch",
	                                 "/data/_Patches/PPSA01234.json"};
	const auto args               = Build(effective);
	Check(args == expected, "every option in order: " + Show(args));
}

void TestScreenResolution() {
	using kyty::core::ScreenResolution;
	const std::vector<std::pair<ScreenResolution, std::pair<std::string, std::string>>> rows = {
	    {ScreenResolution::R1280X720, {"1280", "720"}},
	    {ScreenResolution::R1920X1080, {"1920", "1080"}},
	    {ScreenResolution::R2560X1440, {"2560", "1440"}},
	    {ScreenResolution::R3840X2160, {"3840", "2160"}}};
	for (const auto& [resolution, size]: rows) {
		GameConfiguration c;
		c.screen_resolution = resolution;
		const auto args     = Build(c);
		CheckValue(args, "--screen-width", size.first);
		CheckValue(args, "--screen-height", size.second);
	}
}

void TestUser() {
	GameConfiguration c;
	for (const auto& [stored, passed]: std::vector<std::pair<std::string, std::string>> {
	         {"Kyty", "Kyty"},
	         {"  Player One\t", "Player One"},
	         {"\xE3\x80\x80\xE5\x90\x8D\xE5\x89\x8D\xC2\xA0", "\xE5\x90\x8D\xE5\x89\x8D"},
	         {"\n a b \r", "a b"}}) {
		c.user_name = stored;
		CheckValue(Build(c), "--user-name", passed);
	}
	for (const int32_t id: {0, 1, 256, kyty::core::kMaxUserId}) {
		c.user_id = id;
		CheckValue(Build(c), "--user-id", std::to_string(id));
	}
}

void TestOmittedValues() {
	GameConfiguration c;
	CheckAbsent(Build(c), "--mic");
	c.audio_input_device = "Built-in Audio Analog Stereo";
	CheckValue(Build(c), "--mic", "Built-in Audio Analog Stereo");

	c.gpu_index = -1;
	CheckAbsent(Build(c), "--gpu");
	for (const int32_t gpu: {0, 1, 7}) {
		c.gpu_index = gpu;
		CheckValue(Build(c), "--gpu", std::to_string(gpu));
	}

	EffectiveConfiguration effective;
	effective.controller.color = "";
	CheckAbsent(Build(effective), "--controller-color");
	effective.controller.color = "#0070d1";
	CheckValue(Build(effective), "--controller-color", "#0070d1");

	effective.addons_dir = "";
	CheckAbsent(Build(effective), "--addcont-dir");
	effective.addons_dir = "/mnt/add ons";
	CheckValue(Build(effective), "--addcont-dir", "/mnt/add ons");

	CheckAbsent(Build(effective), "--game-patch");
}

void TestController() {
	EffectiveConfiguration effective;
	for (const auto& [volume, vibration, player]:
	     std::vector<std::tuple<int32_t, int32_t, int32_t>> {
	         {0, 0, 0}, {50, 100, 1}, {100, 37, 4}}) {
		effective.controller.speaker_volume      = volume;
		effective.controller.vibration_intensity = vibration;
		effective.controller.keyboard_player     = player;
		const auto args                          = Build(effective);
		CheckValue(args, "--controller-volume", std::to_string(volume));
		CheckValue(args, "--controller-vibration", std::to_string(vibration));
		CheckValue(args, "--keyboard-player", std::to_string(player));
	}
}

void TestFlags() {
	struct Row {
		bool GameConfiguration::* field;
		const char*               flag;
	};
	const std::vector<Row> rows = {
	    {&GameConfiguration::fullscreen_enabled, "--fullscreen"},
	    {&GameConfiguration::hdr_enabled, "--hdr"},
	    {&GameConfiguration::hide_cursor_enabled, "--hide-cursor"},
	    {&GameConfiguration::readback_linear_images, "--readback-linear-images"},
	    {&GameConfiguration::readback_tiled_images, "--readback-tiled-images"},
	    {&GameConfiguration::sync_raw_image_buffers, "--sync-raw-image-buffers"},
	    {&GameConfiguration::tessellation_enabled, "--tessellation"},
	    {&GameConfiguration::perf_overlay_enabled, "--perf-overlay"},
	    {&GameConfiguration::skip_notice_screen, "--skip-notice-screen"},
	    {&GameConfiguration::network_connected, "--network-connected"},
	    {&GameConfiguration::vulkan_validation_enabled, "--vulkan-validation"},
	    {&GameConfiguration::shader_validation_enabled, "--shader-validation"},
	    {&GameConfiguration::command_buffer_dump_enabled, "--command-buffer-dump"},
	    {&GameConfiguration::profiler_enabled, "--profile"},
	    {&GameConfiguration::renderdoc_enabled, "--rd"},
	    {&GameConfiguration::amd_cpu_enabled, "--amd-cpu"}};
	for (const auto& row: rows) {
		GameConfiguration off;
		off.*row.field = false;
		CheckAbsent(Build(off), row.flag);
		GameConfiguration on;
		on.*row.field   = true;
		const auto args = Build(on);
		Check(Count(args, row.flag) == 1, std::string(row.flag) + " when on: " + Show(args));
		// Only this flag changes the arguments.
		auto without = args;
		without.erase(std::find(without.begin(), without.end(), row.flag));
		Check(without == Build(off), std::string(row.flag) + " adds only itself");
	}

	GameConfiguration trophies;
	trophies.trophy_enabled = true;
	CheckAbsent(Build(trophies), "--no-trophy-notifications");
	trophies.trophy_enabled = false;
	Check(Count(Build(trophies), "--no-trophy-notifications") == 1,
	      "--no-trophy-notifications when trophies are off");
}

void TestNumbers() {
	GameConfiguration c;
	for (const int32_t seconds: {5, 6, 10}) {
		c.trophy_duration_seconds = seconds;
		CheckValue(Build(c), "--trophy-duration", std::to_string(seconds));
	}
	for (const int32_t frequency: {30, 60, 144, 360}) {
		c.vblank_frequency = frequency;
		CheckValue(Build(c), "--vblank-frequency", std::to_string(frequency));
	}
	for (const int32_t language: {0, 1, 29}) {
		c.console_language = language;
		CheckValue(Build(c), "--console-language", std::to_string(language));
	}
}

void TestEnums() {
	using kyty::core::LogDirection;
	using kyty::core::PresentMode;
	using kyty::core::ShaderOptimizationType;
	GameConfiguration c;
	for (const auto& [mode, text]:
	     std::vector<std::pair<PresentMode, std::string>> {{PresentMode::Fifo, "Fifo"},
	                                                       {PresentMode::Mailbox, "Mailbox"},
	                                                       {PresentMode::Immediate, "Immediate"}}) {
		c.present_mode = mode;
		CheckValue(Build(c), "--present-mode", text);
	}
	for (const auto& [type, text]: std::vector<std::pair<ShaderOptimizationType, std::string>> {
	         {ShaderOptimizationType::None, "None"},
	         {ShaderOptimizationType::Size, "Size"},
	         {ShaderOptimizationType::Performance, "Performance"}}) {
		c.shader_optimization_type = type;
		CheckValue(Build(c), "--shader-optimization-type", text);
	}
	for (const auto& [direction, text]:
	     std::vector<std::pair<LogDirection, std::string>> {{LogDirection::Silent, "Silent"},
	                                                        {LogDirection::Console, "Console"},
	                                                        {LogDirection::File, "File"}}) {
		c.shader_log_direction = direction;
		c.printf_direction     = direction;
		const auto args        = Build(c);
		CheckValue(args, "--shader-log-direction", text);
		CheckValue(args, "--printf-direction", text);
	}
}

void TestPaths() {
	GameConfiguration c;
	c.shader_log_folder          = "my shader logs";
	c.command_buffer_dump_folder = "/tmp/dumps";
	c.printf_output_file         = "printf.txt";
	auto args                    = Build(c);
	CheckValue(args, "--shader-log-folder", "my shader logs");
	CheckValue(args, "--command-buffer-dump-folder", "/tmp/dumps");
	CheckValue(args, "--printf-output-file", "printf.txt");

	// Empty paths are valid while their output is off and are passed as empty values.
	c.shader_log_folder          = "";
	c.command_buffer_dump_folder = "";
	c.printf_output_file         = "";
	args                         = Build(c);
	CheckValue(args, "--shader-log-folder", "");
	CheckValue(args, "--command-buffer-dump-folder", "");
	CheckValue(args, "--printf-output-file", "");
}

void TestGamePath() {
	GameConfiguration c;
	c.elf = "eboot.bin";
	CheckValue(Build(c), "--game", "/games/A");
	c.elf = "";
	CheckValue(Build(c), "--game", "/games/A");
	c.elf = "bin/eboot_alt.bin";
	CheckValue(Build(c), "--game", "/games/A/bin/eboot_alt.bin");
	c.elf = "./eboot.bin";
	CheckValue(Build(c), "--game", "/games/A/./eboot.bin");
	c.elf = "EBOOT.BIN";
	CheckValue(Build(c), "--game", "/games/A/EBOOT.BIN");
	CheckValue(Build(c, DirectoryEntry("/games/trailing/")), "--game", "/games/trailing/EBOOT.BIN");

	// An archive is always passed as it is, whatever the elf.
	for (const char* elf: {"eboot.bin", "bin/x.elf"}) {
		c.elf = elf;
		CheckValue(Build(c, ArchiveEntry()), "--game", "/games/B.zar");
	}

	// The path is one argument with its bytes unchanged (no quoting, no shell).
	const std::string odd =
	    "/games/it's a \"game\" $HOME; `x` | \\ *?[]{}!#~ \xE6\x97\xA5\xE6\x9C\xAC";
	c.elf           = "eboot.bin";
	const auto args = Build(c, DirectoryEntry(odd));
	CheckValue(args, "--game", odd);
	Check(args.back() == odd, "--game is the last pair without add-ons or cheats");
}

void TestInputMapping() {
	GameConfiguration c;
	c.host_input_mapping = {"Cross=J", "NoEquals", "=J", "Cross=", " = ", "", " Pad:Cross = South ",
	                        "a=b=c",   "\t=x"};
	const auto args      = Build(c);
	const auto keymaps   = Keymaps(args);
	Check(keymaps == std::vector<std::string> {"Cross=J", " Pad:Cross = South ", "a=b=c"},
	      "only valid entries become --keymap, in order and as stored: " + Show(args));
	Check(Count(args, "--keymap") == 3, "one --keymap per valid entry");

	// The dropped entries are what FindInvalidMappingEntries reports.
	const auto invalid = kyty::core::FindInvalidMappingEntries(c.host_input_mapping);
	Check(invalid.size() == c.host_input_mapping.size() - keymaps.size(),
	      "every entry is either passed or reported");
	Check(keymaps == kyty::core::DropInvalidMappingEntries(c.host_input_mapping),
	      "the passed entries are DropInvalidMappingEntries()");

	c.host_input_mapping = {"NoEquals"};
	CheckAbsent(Build(c), "--keymap");
	c.host_input_mapping.clear();
	CheckAbsent(Build(c), "--keymap");
}

// The mapping that ResolveEffectiveConfiguration selects reaches the arguments.
void TestPerGameAndGlobalMapping() {
	kyty::core::LauncherSettings settings;
	settings.global.host_input_mapping = {"Cross=G", "global without equals"};
	const auto entry                   = DirectoryEntry();

	auto resolved = kyty::core::ResolveEffectiveConfiguration(settings, entry, std::nullopt);
	Check(Keymaps(Build(resolved, entry)) == std::vector<std::string> {"Cross=G"},
	      "a game without custom settings uses the global mapping");

	kyty::core::GameSettingsEntry custom;
	custom.game_path                          = entry.path.string();
	custom.configuration.user_name            = "Custom";
	custom.configuration.host_input_mapping   = {"Cross=P", "per-game without equals"};
	custom.configuration.input_mapping_custom = false;
	settings.games.push_back(custom);
	resolved  = kyty::core::ResolveEffectiveConfiguration(settings, entry, std::nullopt);
	auto args = Build(resolved, entry);
	CheckValue(args, "--user-name", "Custom");
	Check(Keymaps(args) == std::vector<std::string> {"Cross=G"},
	      "custom settings without input_mapping_custom use the global mapping");

	settings.games.back().configuration.input_mapping_custom = true;
	resolved = kyty::core::ResolveEffectiveConfiguration(settings, entry, std::nullopt);
	Check(Keymaps(Build(resolved, entry)) == std::vector<std::string> {"Cross=P"},
	      "input_mapping_custom uses the game's own mapping");
}

void TestCheatPlan() {
	KytyTests::FakeEnvironment  env;
	const std::filesystem::path data_dir = "/data/kyty";
	const std::filesystem::path plan     = "/data/kyty/_Patches/PPSA01234.json";
	Check(!kyty::core::FindCheatPlan(env, data_dir, "PPSA01234"), "no cheat plan without the file");

	env.AddExistingPath(plan);
	env.AddExistingPath("/data/kyty/_Patches/CUSA01234.json");
	env.AddExistingPath("/data/kyty/_Patches/.json");
	const auto found = kyty::core::FindCheatPlan(env, data_dir, " ppsa01234\t");
	Check(found && found->native() == plan.native(), "the plan of a PPSA title ID");
	Check(!kyty::core::FindCheatPlan(env, data_dir, "CUSA01234"),
	      "no plan for a title ID that is not PPSA, even when the file exists");
	Check(!kyty::core::FindCheatPlan(env, data_dir, ""), "no plan for an empty title ID");
	Check(!kyty::core::FindCheatPlan(env, data_dir, "PPSA54321"), "no plan for another game");

	kyty::core::LauncherSettings settings;
	const auto                   entry = DirectoryEntry();
	auto       resolved = kyty::core::ResolveEffectiveConfiguration(settings, entry, found);
	const auto args     = Build(resolved, entry);
	CheckValue(args, "--game-patch", plan.string());
	Check(args.size() >= 2 && args[args.size() - 2] == "--game-patch",
	      "--game-patch comes last: " + Show(args));

	resolved = kyty::core::ResolveEffectiveConfiguration(
	    settings, entry, kyty::core::FindCheatPlan(env, data_dir, "PPSA54321"));
	CheckAbsent(Build(resolved, entry), "--game-patch");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("EmulatorArgumentsTests");
	TestDefaults();
	TestEveryOptionInOrder();
	TestScreenResolution();
	TestUser();
	TestOmittedValues();
	TestController();
	TestFlags();
	TestNumbers();
	TestEnums();
	TestPaths();
	TestGamePath();
	TestInputMapping();
	TestPerGameAndGlobalMapping();
	TestCheatPlan();
	std::printf("EmulatorArgumentsTests: all tests passed\n");
	return 0;
}
