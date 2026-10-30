#include "launcher/core/emulatorArguments.h"

#include "launcher/core/cheats.h"
#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/text.h"

#include <cstdint>
#include <utility>

namespace kyty::core {

namespace {

struct ScreenSize {
	int32_t width  = 1280;
	int32_t height = 720;
};

ScreenSize SizeOf(ScreenResolution resolution) {
	switch (resolution) {
		case ScreenResolution::R1280X720: return {1280, 720};
		case ScreenResolution::R1920X1080: return {1920, 1080};
		case ScreenResolution::R2560X1440: return {2560, 1440};
		case ScreenResolution::R3840X2160: return {3840, 2160};
	}
	return {};
}

// The text form of a field (the Q_ENUM key name of an enum field), from the field table.
std::string FieldText(const GameConfiguration& configuration, std::string_view name) {
	const auto* field = FindGameConfigurationField(name);
	return field != nullptr ? field->format(field->get(configuration)) : std::string();
}

class Arguments {
public:
	void Value(const char* name, std::string value) {
		m_args.emplace_back(name);
		m_args.push_back(std::move(value));
	}
	void Number(const char* name, int64_t value) { Value(name, std::to_string(value)); }
	void Flag(bool enabled, const char* name) {
		if (enabled) {
			m_args.emplace_back(name);
		}
	}
	std::vector<std::string> Take() { return std::move(m_args); }

private:
	std::vector<std::string> m_args;
};

} // namespace

std::vector<std::string> BuildEmulatorArguments(const GameEntry&              entry,
                                                const EffectiveConfiguration& effective) {
	const auto& config     = effective.configuration;
	const auto& controller = effective.controller;
	const auto  size       = SizeOf(config.screen_resolution);

	Arguments args;
	args.Number("--screen-width", size.width);
	args.Number("--screen-height", size.height);
	args.Value("--user-name", std::string(TrimWhitespace(config.user_name)));
	args.Number("--user-id", config.user_id);
	if (!config.audio_input_device.empty()) {
		args.Value("--mic", config.audio_input_device);
	}
	if (!controller.color.empty()) {
		args.Value("--controller-color", controller.color);
	}
	args.Number("--controller-volume", controller.speaker_volume);
	args.Number("--controller-vibration", controller.vibration_intensity);
	args.Number("--keyboard-player", controller.keyboard_player);
	args.Value("--present-mode", FieldText(config, "present_mode"));
	if (config.gpu_index >= 0) {
		args.Number("--gpu", config.gpu_index);
	}
	args.Flag(config.fullscreen_enabled, "--fullscreen");
	args.Flag(config.hdr_enabled, "--hdr");
	args.Flag(config.hide_cursor_enabled, "--hide-cursor");
	args.Flag(config.readback_linear_images, "--readback-linear-images");
	args.Flag(config.readback_tiled_images, "--readback-tiled-images");
	args.Flag(config.sync_raw_image_buffers, "--sync-raw-image-buffers");
	args.Flag(!config.trophy_enabled, "--no-trophy-notifications");
	args.Number("--trophy-duration", config.trophy_duration_seconds);
	args.Flag(config.perf_overlay_enabled, "--perf-overlay");
	args.Flag(config.skip_notice_screen, "--skip-notice-screen");
	args.Flag(config.network_connected, "--network-connected");
	args.Flag(config.tessellation_enabled, "--tessellation");
	args.Number("--vblank-frequency", config.vblank_frequency);
	args.Number("--console-language", config.console_language);
	args.Flag(config.vulkan_validation_enabled, "--vulkan-validation");
	args.Flag(config.shader_validation_enabled, "--shader-validation");
	args.Value("--shader-optimization-type", FieldText(config, "shader_optimization_type"));
	args.Value("--shader-log-direction", FieldText(config, "shader_log_direction"));
	args.Value("--shader-log-folder", config.shader_log_folder);
	args.Flag(config.command_buffer_dump_enabled, "--command-buffer-dump");
	args.Value("--command-buffer-dump-folder", config.command_buffer_dump_folder);
	args.Value("--printf-direction", FieldText(config, "printf_direction"));
	args.Value("--printf-output-file", config.printf_output_file);
	args.Flag(config.profiler_enabled, "--profile");
	args.Flag(config.amd_cpu_enabled, "--amd-cpu");
	for (const auto& mapping: config.host_input_mapping) {
		if (IsValidMappingEntry(mapping)) {
			args.Value("--keymap", mapping);
		}
	}
	args.Flag(config.renderdoc_enabled, "--rd");

	const bool whole_entry =
	    entry.kind == GameEntry::Kind::Archive || config.elf.empty() || config.elf == "eboot.bin";
	args.Value("--game", whole_entry ? entry.path.string() : (entry.path / config.elf).string());
	if (!effective.addons_dir.empty()) {
		args.Value("--addcont-dir", effective.addons_dir);
	}
	if (effective.cheat_plan) {
		args.Value("--game-patch", effective.cheat_plan->string());
	}
	return args.Take();
}

std::optional<std::filesystem::path> FindCheatPlan(const Environment&           env,
                                                   const std::filesystem::path& data_dir,
                                                   std::string_view             title_id) {
	if (!IsCheatTitleId(title_id)) {
		return std::nullopt;
	}
	auto path = CheatFilePath(data_dir, title_id);
	if (!env.FileExists(path)) {
		return std::nullopt;
	}
	return path;
}

} // namespace kyty::core
