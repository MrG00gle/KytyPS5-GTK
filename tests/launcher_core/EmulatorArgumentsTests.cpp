// Example tests of the Emulator_Arguments (src/launcher/core/emulatorArguments.h, task 15.1): every
// row of the design's mapping table, both --game forms, the omitted options, the input mapping
// filter, per-game and global mappings, the cheat plan, and the absence of --redzone and
// --shader-optimization-type. With --check-emulator <kyty_emulator> it instead runs the real
// emulator on the arguments (CTest emulator_accepts_launcher_arguments).

#include "launcher/core/configurationFields.h"
#include "launcher/core/emulatorArguments.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/settingsStore.h"
#include "support/fakeEnvironment.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
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
	                       "--flush-draws",
	                       "0",
	                       "--trophy-duration",
	                       "6",
	                       "--shader-cache",
	                       "--vblank-frequency",
	                       "60",
	                       "--console-language",
	                       "1",
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
	c.screen_resolution           = kyty::core::ScreenResolution {3840, 2160};
	c.user_name                   = " Player One ";
	c.user_id                     = kyty::core::kMaxUserId;
	c.audio_input_device          = "USB Mic";
	c.present_mode                = kyty::core::PresentMode::Immediate;
	c.gpu_index                   = 2;
	c.fullscreen_enabled          = true;
	c.hdr_enabled                 = true;
	c.hide_cursor_enabled         = true;
	c.stretch_enabled             = true;
	c.readback_linear_images      = true;
	c.readback_tiled_images       = true;
	c.readback_storage_images     = true;
	c.sync_raw_image_buffers      = true;
	c.flush_draws                 = 2147483647;
	c.tessellation_enabled        = true;
	c.occlusion_queries_enabled   = true;
	c.relaxed_shader_float        = true;
	c.native_rt                   = true;
	c.shader_cache_enabled        = true;
	c.trophy_enabled              = false;
	c.trophy_duration_seconds     = 10;
	c.perf_overlay_enabled        = true;
	c.skip_notice_screen          = true;
	c.network_connected           = true;
	c.psn_emulation_enabled       = true;
	c.vblank_frequency            = 360;
	c.console_language            = 29;
	c.vulkan_validation_enabled   = true;
	c.shader_validation_enabled   = true;
	c.shader_log_direction        = kyty::core::LogDirection::File;
	c.shader_log_folder           = "logs/shaders";
	c.command_buffer_dump_enabled = true;
	c.command_buffer_dump_folder  = "dumps";
	c.printf_direction            = kyty::core::LogDirection::Console;
	c.printf_output_file          = "out.txt";
	c.profiler_enabled            = true;
	c.renderdoc_enabled           = true;
	c.pro_enabled                 = true;
	c.pssr_upscaler               = kyty::core::PssrUpscaler::DLSS;
	c.dlss_folder                 = "/models/dlss";
	c.dlss_preset                 = kyty::core::DlssPreset::K;
	c.fsr_folder                  = "/ffx";
	c.amd_cpu_enabled             = true;
	c.guest_tsc_enabled           = true;
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
	                                 "--stretch",
	                                 "--readback-linear-images",
	                                 "--readback-tiled-images",
	                                 "--readback-storage-images",
	                                 "--sync-raw-image-buffers",
	                                 "--flush-draws",
	                                 "2147483647",
	                                 "--no-trophy-notifications",
	                                 "--trophy-duration",
	                                 "10",
	                                 "--perf-overlay",
	                                 "--skip-notice-screen",
	                                 "--network-connected",
	                                 "--emulate-psn",
	                                 "--tessellation",
	                                 "--occlusion-queries",
	                                 "--relaxed-shader-float",
	                                 "--native-rt",
	                                 "--shader-cache",
	                                 "--vblank-frequency",
	                                 "360",
	                                 "--console-language",
	                                 "29",
	                                 "--vulkan-validation",
	                                 "--shader-validation",
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
	                                 "--trinity",
	                                 "--pssr-upscaler",
	                                 "DLSS",
	                                 "--dlss-folder",
	                                 "/models/dlss",
	                                 "--dlss-preset",
	                                 "K",
	                                 "--amd-cpu",
	                                 "--guest-tsc",
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
	    {{1280, 720}, {"1280", "720"}},   {{1920, 1080}, {"1920", "1080"}},
	    {{2560, 1440}, {"2560", "1440"}}, {{3840, 2160}, {"3840", "2160"}},
	    {{2560, 1080}, {"2560", "1080"}}, {{3440, 1440}, {"3440", "1440"}},
	    {{3840, 1600}, {"3840", "1600"}}, {{5120, 2160}, {"5120", "2160"}},
	    {{320, 320}, {"320", "320"}},     {{16384, 16384}, {"16384", "16384"}},
	    {{1366, 768}, {"1366", "768"}}};
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
	    {&GameConfiguration::stretch_enabled, "--stretch"},
	    {&GameConfiguration::readback_linear_images, "--readback-linear-images"},
	    {&GameConfiguration::readback_tiled_images, "--readback-tiled-images"},
	    {&GameConfiguration::readback_storage_images, "--readback-storage-images"},
	    {&GameConfiguration::sync_raw_image_buffers, "--sync-raw-image-buffers"},
	    {&GameConfiguration::tessellation_enabled, "--tessellation"},
	    {&GameConfiguration::occlusion_queries_enabled, "--occlusion-queries"},
	    {&GameConfiguration::relaxed_shader_float, "--relaxed-shader-float"},
	    {&GameConfiguration::native_rt, "--native-rt"},
	    {&GameConfiguration::shader_cache_enabled, "--shader-cache"},
	    {&GameConfiguration::perf_overlay_enabled, "--perf-overlay"},
	    {&GameConfiguration::skip_notice_screen, "--skip-notice-screen"},
	    {&GameConfiguration::network_connected, "--network-connected"},
	    {&GameConfiguration::psn_emulation_enabled, "--emulate-psn"},
	    {&GameConfiguration::vulkan_validation_enabled, "--vulkan-validation"},
	    {&GameConfiguration::shader_validation_enabled, "--shader-validation"},
	    {&GameConfiguration::command_buffer_dump_enabled, "--command-buffer-dump"},
	    {&GameConfiguration::profiler_enabled, "--profile"},
	    {&GameConfiguration::renderdoc_enabled, "--rd"},
	    {&GameConfiguration::amd_cpu_enabled, "--amd-cpu"},
	    {&GameConfiguration::guest_tsc_enabled, "--guest-tsc"}};
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
	// --flush-draws is always passed; 0 is off.
	for (const int32_t draws: {0, 1, 500, 2147483647}) {
		c.flush_draws = draws;
		CheckValue(Build(c), "--flush-draws", std::to_string(draws));
	}
}

// PS5 Pro mode: --trinity, and only with it the upscaler and the upscaler's folder (and preset).
void TestProMode() {
	using kyty::core::DlssPreset;
	using kyty::core::PssrUpscaler;
	GameConfiguration c;
	c.dlss_folder = "/models/dlss";
	c.fsr_folder  = "/ffx";
	c.dlss_preset = DlssPreset::A;
	for (const auto upscaler: {PssrUpscaler::Linear, PssrUpscaler::DLSS, PssrUpscaler::FSR}) {
		c.pro_enabled   = false;
		c.pssr_upscaler = upscaler;
		const auto off  = Build(c);
		for (const char* name:
		     {"--trinity", "--pssr-upscaler", "--dlss-folder", "--dlss-preset", "--fsr-folder"}) {
			CheckAbsent(off, name);
		}
	}
	c.pro_enabled   = true;
	c.pssr_upscaler = PssrUpscaler::Linear;
	auto args       = Build(c);
	Check(Count(args, "--trinity") == 1, "--trinity with Pro mode: " + Show(args));
	CheckValue(args, "--pssr-upscaler", "Linear");
	CheckAbsent(args, "--dlss-folder");
	CheckAbsent(args, "--dlss-preset");
	CheckAbsent(args, "--fsr-folder");

	c.pssr_upscaler = PssrUpscaler::DLSS;
	args            = Build(c);
	CheckValue(args, "--pssr-upscaler", "DLSS");
	CheckValue(args, "--dlss-folder", "/models/dlss");
	CheckValue(args, "--dlss-preset", "A");
	CheckAbsent(args, "--fsr-folder");
	const auto trinity = std::find(args.begin(), args.end(), "--trinity");
	Check(args.end() - trinity >= 7 &&
	          Args(trinity, trinity + 7) == Args {"--trinity", "--pssr-upscaler", "DLSS",
	                                              "--dlss-folder", "/models/dlss", "--dlss-preset",
	                                              "A"},
	      "the upscaler options follow --trinity: " + Show(args));
	for (const auto& [preset, letter]: std::vector<std::pair<DlssPreset, std::string>> {
	         {DlssPreset::B, "B"}, {DlssPreset::K, "K"}, {DlssPreset::O, "O"}}) {
		c.dlss_preset = preset;
		CheckValue(Build(c), "--dlss-preset", letter);
	}
	c.dlss_preset = DlssPreset::Default;
	CheckAbsent(Build(c), "--dlss-preset");
	// An empty folder is passed as it is: the emulator then does not use DLSS.
	c.dlss_folder = "";
	CheckValue(Build(c), "--dlss-folder", "");

	// FSR, read from an upstream Kyty.ini: passed as stored (it has no effect on Linux).
	c.pssr_upscaler = PssrUpscaler::FSR;
	args            = Build(c);
	CheckValue(args, "--pssr-upscaler", "FSR");
	CheckValue(args, "--fsr-folder", "/ffx");
	CheckAbsent(args, "--dlss-folder");
	CheckAbsent(args, "--dlss-preset");
}

void TestEnums() {
	using kyty::core::LogDirection;
	using kyty::core::PresentMode;
	GameConfiguration c;
	for (const auto& [mode, text]:
	     std::vector<std::pair<PresentMode, std::string>> {{PresentMode::Fifo, "Fifo"},
	                                                       {PresentMode::Mailbox, "Mailbox"},
	                                                       {PresentMode::Immediate, "Immediate"}}) {
		c.present_mode = mode;
		CheckValue(Build(c), "--present-mode", text);
	}
	// The emulator drops --shader-optimization-type, so the launcher never passes it.
	CheckAbsent(Build(c), "--shader-optimization-type");
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

// ---- CTest emulator_accepts_launcher_arguments (--check-emulator <kyty_emulator>)
//
// The shipped kyty_emulator accepts the arguments that BuildEmulatorArguments passes. P21
// (tests/properties/emulator_launch_tests) checks them against the in-process parser; this mode
// runs the real binary, with --help first, so that it parses every option and value and exits 0
// without starting a game. A removed or renamed emulator option makes it exit 1 instead.

struct ProcessResult {
	int         exit_code = -1; // -1 unless the process exited normally.
	std::string output;         // stdout and stderr
};

// Runs program with args, stdin from /dev/null, and captures stdout and stderr together.
ProcessResult RunProcess(const std::string& program, const Args& args) {
	Args storage {program};
	storage.insert(storage.end(), args.begin(), args.end());
	std::vector<char*> argv;
	for (auto& arg: storage) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);

	int pipe_fds[2] = {-1, -1};
	Check(pipe2(pipe_fds, O_CLOEXEC) == 0, "create the output pipe");
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
	pid_t     pid   = 0;
	const int error = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipe_fds[1]);
	Check(error == 0, "spawn " + program + ": " + std::strerror(error));

	ProcessResult result;
	char          buffer[4096];
	for (;;) {
		const auto count = read(pipe_fds[0], buffer, sizeof(buffer));
		if (count > 0) {
			result.output.append(buffer, static_cast<std::size_t>(count));
		} else if (count < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	close(pipe_fds[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		Check(errno == EINTR, "wait for " + program);
	}
	if (WIFEXITED(status)) {
		result.exit_code = WEXITSTATUS(status);
	}
	return result;
}

void CheckEmulatorAccepts(const std::string& emulator, const EffectiveConfiguration& effective,
                          const GameEntry& entry, const std::string& what) {
	const auto built = Build(effective, entry);
	Args       args  = {"--help"};
	args.insert(args.end(), built.begin(), built.end());
	const auto result     = RunProcess(emulator, args);
	const auto first_line = result.output.substr(0, result.output.find('\n'));
	Check(result.exit_code == 0 && result.output.find("unknown option") == std::string::npos,
	      what + ": kyty_emulator accepts " + Show(built) + " (exit " +
	          std::to_string(result.exit_code) + ", first line '" + first_line + "')");
}

void CheckEmulator(const std::string& emulator) {
	using kyty::core::FieldKind;
	using kyty::core::FieldValue;
	using kyty::core::GameConfigurationFields;

	// --game and --game-patch must name existing files.
	const KytyTests::TempDir dir("kyty_emulator_arguments");
	const auto               game = dir.MakeDirectories("game");
	dir.WriteFile("game/eboot.bin", "");
	dir.WriteFile("game/bin/game.elf", "");
	const auto addons = dir.MakeDirectories("addons");
	const auto plan   = dir.WriteFile("_Patches/PPSA01234.json", "{}\n");
	const auto entry  = DirectoryEntry(game);

	// (a) The defaults.
	CheckEmulatorAccepts(emulator, EffectiveConfiguration {}, entry, "the defaults");

	// (b) Every boolean field off and then on (trophy_enabled passes its option when off), with
	// every optional value and the largest values; then each value of each enum field.
	EffectiveConfiguration every;
	auto&                  c  = every.configuration;
	c.user_name               = " Player One ";
	c.user_id                 = kyty::core::kMaxUserId;
	c.audio_input_device      = "Test Microphone";
	c.gpu_index               = 0;
	c.trophy_duration_seconds = 10;
	c.vblank_frequency        = 360;
	c.console_language        = kyty::core::kMaxConsoleLanguage;
	c.flush_draws             = 2147483647;
	c.dlss_folder             = "models";
	c.fsr_folder              = "ffx";
	c.host_input_mapping      = {"Cross=J", "Pad:Cross=South"};
	every.controller          = {"#0070D1", 100, 100, kyty::core::kMaxKeyboardPlayer};
	for (const bool on: {false, true}) {
		for (const auto& field: GameConfigurationFields()) {
			if (field.kind == FieldKind::Bool) {
				Check(field.set(c, FieldValue(on)), "set " + std::string(field.name));
			}
		}
		CheckEmulatorAccepts(emulator, every, entry,
		                     on ? "every boolean field on" : "every boolean field off");
	}
	for (const auto& field: GameConfigurationFields()) {
		if (field.kind != FieldKind::Enum) {
			continue;
		}
		for (std::size_t i = 0; i < field.enum_keys.size(); i++) {
			auto value = every;
			// Pro mode is on (every boolean field is); the preset is passed only with DLSS.
			if (field.name == "dlss_preset") {
				value.configuration.pssr_upscaler = kyty::core::PssrUpscaler::DLSS;
			}
			Check(field.set(value.configuration, FieldValue(static_cast<int64_t>(i))),
			      "set " + std::string(field.name));
			CheckEmulatorAccepts(emulator, value, entry,
			                     std::string(field.name) + " " + std::string(field.enum_keys[i]));
		}
	}
	// Custom resolutions: the smallest, the largest, and an ultrawide preset.
	for (const auto& resolution:
	     {kyty::core::ScreenResolution {320, 320}, kyty::core::ScreenResolution {16384, 16384},
	      kyty::core::ScreenResolution {3440, 1440}}) {
		auto sized                            = every;
		sized.configuration.screen_resolution = resolution;
		CheckEmulatorAccepts(emulator, sized, entry,
		                     "the resolution " + kyty::core::FormatScreenResolution(resolution));
	}
	// The other --game form: an executable inside the game directory.
	auto elf              = every;
	elf.configuration.elf = "bin/game.elf";
	CheckEmulatorAccepts(emulator, elf, entry, "an executable other than eboot.bin");

	// (c) An add-ons folder and a cheat plan.
	auto launch       = every;
	launch.addons_dir = addons.string();
	launch.cheat_plan = plan;
	CheckEmulatorAccepts(emulator, launch, entry, "an add-ons folder and a cheat plan");
}

} // namespace

int main(int argc, char* argv[]) {
	KytyTests::CheckNoDisplay("EmulatorArgumentsTests");
	if (argc == 3 && std::string_view(argv[1]) == "--check-emulator") {
		CheckEmulator(argv[2]);
		std::printf("EmulatorArgumentsTests: kyty_emulator accepts the launcher's arguments\n");
		return 0;
	}
	if (argc != 1) {
		std::fprintf(stderr, "usage: %s [--check-emulator <kyty_emulator>]\n", argv[0]);
		return 2;
	}
	TestDefaults();
	TestEveryOptionInOrder();
	TestScreenResolution();
	TestUser();
	TestOmittedValues();
	TestController();
	TestFlags();
	TestNumbers();
	TestProMode();
	TestEnums();
	TestPaths();
	TestGamePath();
	TestInputMapping();
	TestPerGameAndGlobalMapping();
	TestCheatPlan();
	std::printf("EmulatorArgumentsTests: all tests passed\n");
	return 0;
}
