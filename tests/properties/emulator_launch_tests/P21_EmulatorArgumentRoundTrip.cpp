// Feature: linux-gtk-modernization, Property 21: Emulator argument round trip
//
// Validates Requirements 3.4 and 10.2. Each case draws a valid configuration
// (configurationGenerators.h), controller settings, an add-ons directory, and a Game_Entry, and
// mixes invalid stored mapping entries (as a Qt-written store holds them) into the mapping. A
// directory entry is created on disk with its elf file, because ParseOptions checks the --game
// path; an archive entry is a real ZArchive that contains eboot.bin. Half of the PPSA title IDs
// get a cheat file, and FindCheatPlan finds it. BuildEmulatorArguments must give arguments whose
// option names are all in Emulator::Options() (so never --redzone), and the Emulator's
// ParseOptions must accept them with status Run and options equal to the fields per the design's
// mapping table. The Emulator reads a '\' in an --addcont-dir, --game, or --game-patch value as
// '/' (Common::FixFilenameSlash in its option parser), so the add-ons directory is compared after
// that replacement, and elf names on disk hold neither '\' nor NUL.

#include "common/archive.h"
#include "common/emulatorConfig.h"
#include "emulatorOptions.h"
#include "launcher/core/cheats.h"
#include "launcher/core/emulatorArguments.h"
#include "launcher/core/environment.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/text.h"
#include "support/configurationGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ConfigGen = KytyTests::ConfigGen;
namespace fs        = std::filesystem;

using ConfigGen::OneIn;
using ConfigGen::PickOf;

// A ZArchive with eboot.bin, made once per process.
const fs::path& ArchiveFixture() {
	static const KytyTests::TempDir root("kyty_p21_archive");
	static const fs::path           archive = [] {
		root.WriteFile("source/eboot.bin", "eboot");
		root.WriteFile("source/sce_sys/param.json", "{}");
		auto path = root.Path() / "game.zar";
		if (!Common::PackArchive(root.Path() / "source", path,
		                         [](uint64_t /*done*/, uint64_t /*total*/) { return true; })) {
			path.clear();
		}
		return path;
	}();
	return archive;
}

// An elf that can exist below the game directory: a valid elf without '\' or NUL in any
// component and without a "." or ".." component.
std::string DiskElf() {
	if (OneIn(2)) {
		return "eboot.bin";
	}
	static const std::vector<std::string> common = {
	    "bin/eboot_alt.bin", "./eboot.bin", "a..b/c",
	    "dir/.../x.elf",     "EBOOT.BIN",   "sub dir/it's \"x\" $y.elf"};
	if (OneIn(2)) {
		return PickOf(common);
	}
	std::vector<char32_t> alphabet;
	for (const char32_t c: ConfigGen::Alphabet()) {
		if (c != U'/' && c != U'\\' && c != 0) {
			alphabet.push_back(c);
		}
	}
	std::string elf;
	const int   segments = ConfigGen::Choose(1, 4);
	for (int i = 0; i < segments; i++) {
		std::string segment;
		const int   length = ConfigGen::Choose(1, 7);
		for (int j = 0; j < length; j++) {
			segment += ConfigGen::Utf8(PickOf(alphabet));
		}
		if (segment == "." || segment == "..") {
			segment = "x";
		}
		elf += (i == 0 ? "" : "/") + segment;
	}
	return elf;
}

// Invalid stored entries of a Qt-written Kyty.ini, which the arguments must leave out.
std::vector<std::string> WithInvalidEntries(std::vector<std::string> mapping) {
	static const std::vector<std::string> invalid = {
	    "Cross", "=J", "Cross=", " = ", "", "\t=x", "x=\xE3\x80\x80", "Pad:Cross"};
	const int count = ConfigGen::Choose(1, 4);
	for (int i = 0; i < count; i++) {
		const auto at = ConfigGen::Choose<size_t>(0, mapping.size() + 1);
		mapping.insert(mapping.begin() + static_cast<std::ptrdiff_t>(at), PickOf(invalid));
	}
	return mapping;
}

Config::PresentMode EmulatorMode(kyty::core::PresentMode mode) {
	switch (mode) {
		case kyty::core::PresentMode::Fifo: return Config::PresentMode::Fifo;
		case kyty::core::PresentMode::Mailbox: return Config::PresentMode::Mailbox;
		case kyty::core::PresentMode::Immediate: return Config::PresentMode::Immediate;
	}
	return Config::PresentMode::Mailbox;
}

Config::ShaderOptimizationType EmulatorType(kyty::core::ShaderOptimizationType type) {
	switch (type) {
		case kyty::core::ShaderOptimizationType::None: return Config::ShaderOptimizationType::None;
		case kyty::core::ShaderOptimizationType::Size: return Config::ShaderOptimizationType::Size;
		case kyty::core::ShaderOptimizationType::Performance:
			return Config::ShaderOptimizationType::Performance;
	}
	return Config::ShaderOptimizationType::None;
}

Config::LogDirection EmulatorDirection(kyty::core::LogDirection direction) {
	switch (direction) {
		case kyty::core::LogDirection::Silent: return Config::LogDirection::Silent;
		case kyty::core::LogDirection::Console: return Config::LogDirection::Console;
		case kyty::core::LogDirection::File: return Config::LogDirection::File;
	}
	return Config::LogDirection::Silent;
}

struct Size {
	uint32_t width  = 0;
	uint32_t height = 0;
};

Size SizeOf(kyty::core::ScreenResolution resolution) {
	switch (resolution) {
		case kyty::core::ScreenResolution::R1280X720: return {1280, 720};
		case kyty::core::ScreenResolution::R1920X1080: return {1920, 1080};
		case kyty::core::ScreenResolution::R2560X1440: return {2560, 1440};
		case kyty::core::ScreenResolution::R3840X2160: return {3840, 2160};
	}
	return {};
}

std::optional<Config::ControllerColor> ColorOf(const std::string& text) {
	if (text.empty()) {
		return std::nullopt;
	}
	const auto digit = [&](size_t i) {
		const char c = text[i];
		return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
	};
	Config::ControllerColor color {};
	for (size_t i = 0; i < 3; i++) {
		color[i] = static_cast<uint8_t>(digit(1 + 2 * i) * 16 + digit(2 + 2 * i));
	}
	return color;
}

std::string Joined(const std::vector<std::string>& args) {
	std::string text;
	for (const auto& arg: args) {
		text += (text.empty() ? "'" : " '") + arg + "'";
	}
	return text;
}

} // namespace

KYTY_PROPERTY(21, "Emulator argument round trip") {
	return rc::check(name, []() {
		const KytyTests::TempDir dir("kyty_p21");

		kyty::core::EffectiveConfiguration effective;
		auto&                              config = effective.configuration;
		config                                    = ConfigGen::ValidConfiguration();
		effective.controller                      = ConfigGen::ValidController();
		effective.addons_dir = OneIn(3) ? std::string() : ConfigGen::AnyText(16);

		kyty::core::GameEntry entry;
		const bool            archive = OneIn(4);
		if (archive) {
			entry.kind = kyty::core::GameEntry::Kind::Archive;
			entry.path = ArchiveFixture();
			RC_ASSERT(!entry.path.empty());
		} else {
			entry.kind = kyty::core::GameEntry::Kind::Directory;
			entry.path = dir.MakeDirectories("game dir");
			config.elf = DiskElf();
			if (config.elf != "eboot.bin") {
				dir.WriteFile(fs::path("game dir") / config.elf, "elf");
			}
		}
		RC_ASSERT(!kyty::core::Validate(config).has_value());

		const auto valid_mapping = config.host_input_mapping;
		const bool with_invalid  = OneIn(2);
		if (with_invalid) {
			config.host_input_mapping = WithInvalidEntries(config.host_input_mapping);
		}

		const auto title = ConfigGen::TitleId();
		if (kyty::core::IsCheatTitleId(title) && OneIn(2)) {
			const auto plan = kyty::core::CheatFilePath(dir.Path(), title);
			dir.WriteFile(plan.lexically_relative(dir.Path()), "{\"mods\":[]}");
		}
		const auto environment = kyty::core::MakePosixEnvironment();
		effective.cheat_plan   = kyty::core::FindCheatPlan(*environment, dir.Path(), title);

		const auto args = kyty::core::BuildEmulatorArguments(entry, effective);
		RC_LOG() << "title '" << title << "', arguments: " << Joined(args) << "\n";

		// Every option name is in the Emulator's table, and --redzone is not (Requirement 3.4).
		const auto options = Emulator::Options();
		for (const auto& option: options) {
			RC_ASSERT(option.name != "--redzone");
		}
		for (size_t i = 0; i < args.size(); i++) {
			const auto it = std::find_if(options.begin(), options.end(), [&](const auto& option) {
				return option.name == args[i];
			});
			RC_ASSERT(it != options.end());
			RC_ASSERT(it->kind == Emulator::OptionKind::Flag ||
			          it->kind == Emulator::OptionKind::Value);
			if (it->kind == Emulator::OptionKind::Value) {
				RC_ASSERT(i + 1 < args.size());
				i++;
			}
		}

		const auto outcome = Emulator::ParseOptions(args);
		RC_LOG() << "outcome " << static_cast<int>(outcome.status) << " '" << outcome.error
		         << "'\n";
		RC_ASSERT(outcome.status == Emulator::ParseOutcome::Run);
		const auto& parsed = outcome.options;
		const auto& c      = parsed.config;

		const auto size = SizeOf(config.screen_resolution);
		RC_ASSERT(c.screen_width == size.width);
		RC_ASSERT(c.screen_height == size.height);
		RC_ASSERT(c.user_name == std::string(kyty::core::TrimWhitespace(config.user_name)));
		RC_ASSERT(c.user_id == config.user_id);
		RC_ASSERT(c.audio_input_device == config.audio_input_device);
		RC_ASSERT(c.controller_color == ColorOf(effective.controller.color));
		RC_ASSERT(c.controller_speaker_volume ==
		          static_cast<uint32_t>(effective.controller.speaker_volume));
		RC_ASSERT(c.controller_vibration_intensity ==
		          static_cast<uint32_t>(effective.controller.vibration_intensity));
		RC_ASSERT(c.keyboard_player == static_cast<uint32_t>(effective.controller.keyboard_player));
		RC_ASSERT(c.present_mode == EmulatorMode(config.present_mode));
		RC_ASSERT(c.gpu_index == config.gpu_index);
		RC_ASSERT(c.fullscreen_enabled == config.fullscreen_enabled);
		RC_ASSERT(c.hdr_enabled == config.hdr_enabled);
		RC_ASSERT(c.hide_cursor_enabled == config.hide_cursor_enabled);
		RC_ASSERT(c.readback_linear_images == config.readback_linear_images);
		RC_ASSERT(c.readback_tiled_images == config.readback_tiled_images);
		RC_ASSERT(c.sync_raw_image_buffers == config.sync_raw_image_buffers);
		RC_ASSERT(c.tessellation_enabled == config.tessellation_enabled);
		RC_ASSERT(c.trophy_enabled == config.trophy_enabled);
		RC_ASSERT(c.trophy_duration_seconds ==
		          static_cast<uint32_t>(config.trophy_duration_seconds));
		RC_ASSERT(c.perf_overlay_enabled == config.perf_overlay_enabled);
		RC_ASSERT(c.skip_notice_screen == config.skip_notice_screen);
		RC_ASSERT(c.network_connected == config.network_connected);
		RC_ASSERT(c.vblank_frequency == static_cast<uint32_t>(config.vblank_frequency));
		RC_ASSERT(c.console_language == static_cast<uint32_t>(config.console_language));
		RC_ASSERT(c.vulkan_validation_enabled == config.vulkan_validation_enabled);
		RC_ASSERT(c.shader_validation_enabled == config.shader_validation_enabled);
		RC_ASSERT(c.shader_optimization_type == EmulatorType(config.shader_optimization_type));
		RC_ASSERT(c.shader_log_direction == EmulatorDirection(config.shader_log_direction));
		RC_ASSERT(c.shader_log_folder.native() == config.shader_log_folder);
		RC_ASSERT(c.command_buffer_dump_enabled == config.command_buffer_dump_enabled);
		RC_ASSERT(c.command_buffer_dump_folder.native() == config.command_buffer_dump_folder);
		RC_ASSERT(c.printf_direction == EmulatorDirection(config.printf_direction));
		RC_ASSERT(c.printf_output_file.native() == config.printf_output_file);
		RC_ASSERT(c.profiler_enabled == config.profiler_enabled);
		RC_ASSERT(c.renderdoc_enabled == config.renderdoc_enabled);
		RC_ASSERT(c.amd_cpu_enabled == config.amd_cpu_enabled);
		// One --keymap per valid entry, in order and as stored.
		RC_ASSERT(c.keymap == valid_mapping);
		RC_ASSERT(c.keymap == kyty::core::DropInvalidMappingEntries(config.host_input_mapping));

		// The options the launcher never passes keep the Emulator's defaults.
		const Config::ConfigOptions defaults;
		RC_ASSERT(c.splash_enabled == defaults.splash_enabled);
		RC_ASSERT(c.vr_enabled == defaults.vr_enabled);
		RC_ASSERT(c.gpu_assisted_validation_enabled == defaults.gpu_assisted_validation_enabled);
		RC_ASSERT(c.graphics_debug_dump_enabled == defaults.graphics_debug_dump_enabled);
		RC_ASSERT(c.spirv_debug_printf_enabled == defaults.spirv_debug_printf_enabled);
		RC_ASSERT(c.playgo_hack_enabled == defaults.playgo_hack_enabled);
		RC_ASSERT(c.guest_arguments == defaults.guest_arguments);

		// --game: the entry's path for an archive and for eboot.bin, else <path>/<elf>.
		if (archive) {
			RC_ASSERT(parsed.app0_dir.native() == Common::MakeArchivePath(entry.path).native());
			RC_ASSERT(parsed.elf.native() == "/app0/eboot.bin");
		} else if (config.elf == "eboot.bin") {
			RC_ASSERT(parsed.app0_dir.native() == entry.path.native());
			RC_ASSERT(parsed.elf.native() == "/app0/eboot.bin");
		} else {
			const auto game = entry.path / config.elf;
			RC_ASSERT(parsed.app0_dir.native() == game.parent_path().native());
			RC_ASSERT(parsed.elf.native() == (fs::path("/app0") / game.filename()).native());
		}

		auto addons = effective.addons_dir;
		std::replace(addons.begin(), addons.end(), '\\', '/');
		RC_ASSERT(parsed.addcont_dir.native() == addons);
		if (effective.cheat_plan) {
			RC_ASSERT(effective.cheat_plan->native() ==
			          kyty::core::CheatFilePath(dir.Path(), title).native());
			RC_ASSERT(parsed.game_patch.native() == effective.cheat_plan->native());
		} else {
			RC_ASSERT(parsed.game_patch.empty());
		}

		RC_TAG(archive                     ? "archive"
		       : config.elf == "eboot.bin" ? "directory, eboot.bin"
		                                   : "directory, other elf");
		RC_CLASSIFY(effective.cheat_plan.has_value(), "cheat plan");
		RC_CLASSIFY(with_invalid, "invalid mapping entries dropped");
	});
}
