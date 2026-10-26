#include "emulatorOptions.h"

#include "common/archive.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <unistd.h>
#include <fmt/format.h>
#include <magic_enum.hpp>

namespace Emulator {

namespace {

using Config::ConfigOptions;

template <typename E>
bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value.c_str());
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

bool ParseUint32(const std::string& value, uint32_t& out) {
	uint32_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

bool ParseControllerColor(const std::string& value, Config::ControllerColor& out) {
	if (value.size() != 7 || value[0] != '#') {
		return false;
	}
	uint32_t rgb = 0;
	auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), rgb, 16);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
	       static_cast<uint8_t>(rgb)};
	return true;
}

bool ParseInt32(const std::string& value, int32_t& out) {
	int32_t number    = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

std::string InvalidValue(std::string_view name, std::string_view value, std::string_view reason) {
	return fmt::format("invalid value for {}: {} ({})", name, value, reason);
}

std::string OnlyOnce(std::string_view name) {
	return fmt::format("{} can only be specified once", name);
}

// "expected A, B, or C" from the enumerator names.
template <typename E>
std::string EnumReason() {
	constexpr auto names = magic_enum::enum_names<E>();
	std::string    text  = "expected ";
	for (size_t index = 0; index < names.size(); index++) {
		if (index > 0) {
			text += (index + 1 == names.size()) ? ", or " : ", ";
		}
		text += names[index];
	}
	return text;
}

template <typename E>
std::string ApplyEnum(E& out, std::string_view name, const std::string& value) {
	if (!ParseEnum(value, out)) {
		return InvalidValue(name, value, EnumReason<E>());
	}
	return {};
}

// A Value or Terminal option applies its value to options. It returns an empty string on
// success, otherwise the error message.
using ValueHandler = std::string (*)(RunOptions& options, std::string_view name,
                                     const std::string& value);

std::string ApplyGame(RunOptions& options, std::string_view name, const std::string& value) {
	if (!options.app0_dir.empty()) {
		return OnlyOnce(name);
	}

	const std::string fixed = Common::FixFilenameSlash(value);
	const auto        path  = Common::PathFromUtf8(fixed);
	// An archive-member path ("game.zar!/eboot.bin") is checked through its host archive.
	const auto host = Common::IsArchivePath(path) ? Common::GetArchiveHostPath(path) : path;

	if (::access(host.c_str(), F_OK) != 0 && (errno == ENOENT || errno == ENOTDIR)) {
		return fmt::format("{} path does not exist: {}", name, fixed);
	}
	// A directory game also needs search permission.
	const int mode = Common::File::IsDirectoryExisting(host) ? (R_OK | X_OK) : R_OK;
	if (::access(host.c_str(), mode) != 0) {
		return fmt::format("{} path is not readable: {}", name, fixed);
	}

	if (Common::File::IsDirectoryExisting(path)) {
		options.app0_dir = path;
		options.elf      = "/app0/eboot.bin";
	} else if (Common::IsSupportedArchive(path) && Common::File::IsFileExisting(path)) {
		const auto root = Common::MakeArchivePath(path);
		if (!Common::File::IsFileExisting(root / "eboot.bin")) {
			return InvalidValue(name, fixed, "archive does not contain eboot.bin");
		}
		options.app0_dir = root;
		options.elf      = "/app0/eboot.bin";
	} else if (Common::File::IsFileExisting(path)) {
		options.app0_dir = path.parent_path();

		if (options.app0_dir.empty()) {
			options.app0_dir = ".";
		}

		options.elf = std::filesystem::path("/app0") / path.filename();
	} else {
		return InvalidValue(name, fixed, "expected a directory, ELF, or archive");
	}
	return {};
}

std::string ApplyGamePatch(RunOptions& options, std::string_view name, const std::string& value) {
	if (!options.game_patch.empty()) {
		return OnlyOnce(name);
	}
	const std::string fixed = Common::FixFilenameSlash(value);
	const auto        path  = Common::PathFromUtf8(fixed);

	if (!Common::File::IsFileExisting(path)) {
		return InvalidValue(name, fixed, "must point to an existing file");
	}
	options.game_patch = path;
	return {};
}

std::string ApplyAddcontDir(RunOptions& options, std::string_view name, const std::string& value) {
	if (!options.addcont_dir.empty()) {
		return OnlyOnce(name);
	}
	options.addcont_dir = Common::PathFromUtf8(Common::FixFilenameSlash(value));
	return {};
}

std::string ApplyScreenWidth(RunOptions& options, std::string_view name, const std::string& value) {
	if (!ParseUint32(value, options.config.screen_width) || options.config.screen_width == 0) {
		return InvalidValue(name, value, "expected a positive integer");
	}
	return {};
}

std::string ApplyScreenHeight(RunOptions& options, std::string_view name,
                              const std::string& value) {
	if (!ParseUint32(value, options.config.screen_height) || options.config.screen_height == 0) {
		return InvalidValue(name, value, "expected a positive integer");
	}
	return {};
}

std::string ApplyUserName(RunOptions& options, std::string_view name, const std::string& value) {
	if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
		return InvalidValue(name, value,
		                    fmt::format("must contain 1-{} bytes", Config::MAX_USER_NAME_LENGTH));
	}
	options.config.user_name = value;
	return {};
}

std::string ApplyUserId(RunOptions& options, std::string_view name, const std::string& value) {
	if (!ParseUserId(value, options.config.user_id)) {
		return InvalidValue(name, value,
		                    fmt::format("expected 0-{}, except 254 and 255", Config::MAX_USER_ID));
	}
	return {};
}

std::string ApplyMic(RunOptions& options, std::string_view /*name*/, const std::string& value) {
	options.config.audio_input_device = value;
	return {};
}

std::string ApplyControllerColor(RunOptions& options, std::string_view name,
                                 const std::string& value) {
	Config::ControllerColor color {};
	if (!ParseControllerColor(value, color)) {
		return InvalidValue(name, value, "expected #RRGGBB");
	}
	options.config.controller_color = color;
	return {};
}

std::string ApplyControllerVolume(RunOptions& options, std::string_view name,
                                  const std::string& value) {
	if (!ParseUint32(value, options.config.controller_speaker_volume) ||
	    options.config.controller_speaker_volume > 100) {
		return InvalidValue(name, value, "expected 0-100");
	}
	return {};
}

std::string ApplyControllerVibration(RunOptions& options, std::string_view name,
                                     const std::string& value) {
	if (!ParseUint32(value, options.config.controller_vibration_intensity) ||
	    options.config.controller_vibration_intensity > 100) {
		return InvalidValue(name, value, "expected 0-100");
	}
	return {};
}

std::string ApplyKeyboardPlayer(RunOptions& options, std::string_view name,
                                const std::string& value) {
	if (!ParseUint32(value, options.config.keyboard_player) ||
	    options.config.keyboard_player > static_cast<uint32_t>(Config::MAX_PLAYERS)) {
		return InvalidValue(name, value, fmt::format("expected 0-{}", Config::MAX_PLAYERS));
	}
	return {};
}

std::string ApplyPresentMode(RunOptions& options, std::string_view name, const std::string& value) {
	return ApplyEnum(options.config.present_mode, name, value);
}

std::string ApplyGpu(RunOptions& options, std::string_view name, const std::string& value) {
	if (!ParseInt32(value, options.config.gpu_index)) {
		return InvalidValue(name, value, "expected an integer");
	}
	return {};
}

std::string ApplyVblankFrequency(RunOptions& options, std::string_view name,
                                 const std::string& value) {
	if (!ParseUint32(value, options.config.vblank_frequency)) {
		return InvalidValue(name, value, "expected a non-negative integer");
	}
	return {};
}

std::string ApplyConsoleLanguage(RunOptions& options, std::string_view name,
                                 const std::string& value) {
	if (!ParseConsoleLanguage(value, options.config.console_language)) {
		return InvalidValue(name, value,
		                    fmt::format("expected 0-{}", Config::MAX_CONSOLE_LANGUAGE));
	}
	return {};
}

std::string ApplyShaderOptimizationType(RunOptions& options, std::string_view name,
                                        const std::string& value) {
	return ApplyEnum(options.config.shader_optimization_type, name, value);
}

std::string ApplyShaderLogDirection(RunOptions& options, std::string_view name,
                                    const std::string& value) {
	return ApplyEnum(options.config.shader_log_direction, name, value);
}

std::string ApplyShaderLogFolder(RunOptions& options, std::string_view /*name*/,
                                 const std::string& value) {
	options.config.shader_log_folder = Common::PathFromUtf8(value);
	return {};
}

std::string ApplyCommandBufferDumpFolder(RunOptions& options, std::string_view /*name*/,
                                         const std::string& value) {
	options.config.command_buffer_dump_folder = Common::PathFromUtf8(value);
	return {};
}

std::string ApplyPrintfDirection(RunOptions& options, std::string_view name,
                                 const std::string& value) {
	return ApplyEnum(options.config.printf_direction, name, value);
}

std::string ApplyPrintfOutputFile(RunOptions& options, std::string_view /*name*/,
                                  const std::string& value) {
	options.config.printf_output_file = Common::PathFromUtf8(value);
	return {};
}

std::string ApplyTrophyDuration(RunOptions& options, std::string_view name,
                                const std::string& value) {
	if (!ParseUint32(value, options.config.trophy_duration_seconds)) {
		return InvalidValue(name, value, "expected a non-negative integer");
	}
	return {};
}

std::string ApplyKeymap(RunOptions& options, std::string_view name, const std::string& value) {
	const auto split = value.find('=');
	if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
		return InvalidValue(name, value, "expected Control=Input");
	}
	options.config.keymap.push_back(value);
	return {};
}

std::string ApplyGuestExec(RunOptions& options, std::string_view /*name*/,
                           const std::string& value) {
	options.elf = Common::PathFromUtf8(Common::FixFilenameSlash(value));
	return {};
}

struct OptionDef {
	OptionSpec            spec;
	bool ConfigOptions::* field = nullptr; // Flag: the member it sets.
	bool                  value = true;    // Flag: the value it stores.
	ValueHandler          apply = nullptr; // Value and Terminal: applies the value.
};

constexpr OptionDef FlagOption(std::string_view name, bool ConfigOptions::* field,
                               std::string_view help, bool value = true) {
	return {{name, OptionKind::Flag, {}, help}, field, value};
}

constexpr OptionDef ValueOption(std::string_view name, std::string_view hint, ValueHandler apply,
                                std::string_view help) {
	return {{name, OptionKind::Value, hint, help}, nullptr, true, apply};
}

constexpr OptionDef TerminalOption(std::string_view name, std::string_view hint,
                                   ValueHandler apply, std::string_view help) {
	return {{name, OptionKind::Terminal, hint, help}, nullptr, true, apply};
}

constexpr OptionDef HelpOption(std::string_view name, std::string_view help) {
	return {{name, OptionKind::Help, {}, help}};
}

static_assert(Config::DEFAULT_USER_ID == 1000, "update the --user-id help text");

// Every option the Emulator accepts, in help order. A '\n' in a help string starts a
// continuation line.
constexpr OptionDef OPTION_DEFS[] = {
    ValueOption("--game", "<dir|elf|zar>", ApplyGame, "Game directory, ELF, or ZArchive to load."),
    ValueOption("--game-patch", "<json>", ApplyGamePatch, "ETAHen cheat file."),
    ValueOption("--addcont-dir", "<dir>", ApplyAddcontDir, "Add-ons (DLC) folder (default: none)."),
    ValueOption("--screen-width", "<num>", ApplyScreenWidth, "Window width. Default: 1280."),
    ValueOption("--screen-height", "<num>", ApplyScreenHeight, "Window height. Default: 720."),
    ValueOption("--user-name", "<name>", ApplyUserName,
                "Local user name (1-16 bytes). Default: Kyty."),
    ValueOption("--user-id", "<num>", ApplyUserId, "Local user ID. Default: 1000."),
    ValueOption("--mic", "<name>", ApplyMic, "Capture from this microphone; omit for silence."),
    ValueOption("--controller-color", "<#RRGGBB>", ApplyControllerColor,
                "Override the controller lightbar color."),
    ValueOption("--controller-volume", "<0-100>", ApplyControllerVolume,
                "DualSense speaker volume. Default: 50."),
    ValueOption("--controller-vibration", "<0-100>", ApplyControllerVibration,
                "DualSense vibration intensity. Default: 100."),
    ValueOption("--keyboard-player", "<0-4>", ApplyKeyboardPlayer,
                "Keyboard player; 0 shares player 1 with the gamepad. Default: 0."),
    ValueOption("--present-mode", "<value>", ApplyPresentMode,
                "Fifo, Mailbox, or Immediate. Default: Mailbox."),
    ValueOption("--gpu", "<index>", ApplyGpu, "Vulkan physical device index. Default: auto."),
    FlagOption("--fullscreen", &ConfigOptions::fullscreen_enabled,
               "Run in borderless desktop fullscreen."),
    FlagOption("--hdr", &ConfigOptions::hdr_enabled, "Enable HDR10 output when supported."),
    FlagOption("--hide-cursor", &ConfigOptions::hide_cursor_enabled,
               "Hide the cursor after 2 s idle. Default: off."),
    FlagOption("--no-splash", &ConfigOptions::splash_enabled, "Disable the startup splash screen.",
               false),
    FlagOption("--vr", &ConfigOptions::vr_enabled, "Enable the virtual VR headset."),
    FlagOption("--amd-cpu", &ConfigOptions::amd_cpu_enabled, "Apply AMD CPU instruction patches."),
    ValueOption("--vblank-frequency", "<num>", ApplyVblankFrequency,
                "Virtual vblank frequency. Default: 60."),
    ValueOption("--console-language", "<0-29>", ApplyConsoleLanguage,
                "Console language. Default: 1 (English US)."),
    FlagOption("--vulkan-validation", &ConfigOptions::vulkan_validation_enabled,
               "Enable Vulkan validation."),
    FlagOption("--network-connected", &ConfigOptions::network_connected,
               "Use the host local network."),
    FlagOption("--gpu-assisted-validation", &ConfigOptions::gpu_assisted_validation_enabled,
               "Bounds-check shader accesses on the GPU.\n"
               "Implies --vulkan-validation; very slow."),
    FlagOption("--shader-validation", &ConfigOptions::shader_validation_enabled,
               "Enable shader validation."),
    FlagOption("--tessellation", &ConfigOptions::tessellation_enabled,
               "Draw tessellation patches; skipped by default."),
    ValueOption("--shader-optimization-type", "<value>", ApplyShaderOptimizationType,
                "None, Size, or Performance."),
    ValueOption("--shader-log-direction", "<value>", ApplyShaderLogDirection,
                "Silent, Console, or File."),
    ValueOption("--shader-log-folder", "<path>", ApplyShaderLogFolder,
                "Shader log output folder."),
    FlagOption("--command-buffer-dump", &ConfigOptions::command_buffer_dump_enabled,
               "Enable command buffer dumps."),
    ValueOption("--command-buffer-dump-folder", "<path>", ApplyCommandBufferDumpFolder,
                "Command buffer dump folder."),
    FlagOption("--graphics-debug-dump", &ConfigOptions::graphics_debug_dump_enabled,
               "Enable graphics debug dumps."),
    ValueOption("--printf-direction", "<value>", ApplyPrintfDirection, "Silent, Console, or File."),
    ValueOption("--printf-output-file", "<path>", ApplyPrintfOutputFile,
                "Guest printf output file."),
    FlagOption("--profile", &ConfigOptions::profiler_enabled, "Enable the Tracy profiler."),
    FlagOption("--spirv-debug-printf", &ConfigOptions::spirv_debug_printf_enabled,
               "Enable SPIR-V debug printf."),
    FlagOption("--readback-linear-images", &ConfigOptions::readback_linear_images,
               "Read back writable linear images on submit."),
    FlagOption("--readback-tiled-images", &ConfigOptions::readback_tiled_images,
               "Read back small writable tiled images on submit."),
    FlagOption("--sync-raw-image-buffers", &ConfigOptions::sync_raw_image_buffers,
               "Synchronize raw reads of GPU images."),
    FlagOption("--no-trophy-notifications", &ConfigOptions::trophy_enabled,
               "Disable trophy unlock toasts and their sound.", false),
    ValueOption("--trophy-duration", "<seconds>", ApplyTrophyDuration,
                "Trophy toast duration, 5-10. Default: 6."),
    FlagOption("--perf-overlay", &ConfigOptions::perf_overlay_enabled,
               "Show the performance panel (F2)."),
    FlagOption("--playgo-hack", &ConfigOptions::playgo_hack_enabled,
               "Use the supplied PlayGo stub fallback."),
    FlagOption("--skip-notice-screen", &ConfigOptions::skip_notice_screen,
               "Skip startup logos and notices in supported games."),
    ValueOption("--keymap", "<Control=Input>", ApplyKeymap, "DualSense mapping; may be repeated."),
    FlagOption("--rd", &ConfigOptions::renderdoc_enabled, "Enable RenderDoc capture."),
    TerminalOption("--guest-exec", "<elf> [args...]", ApplyGuestExec,
                   "Run this guest ELF with the remaining arguments instead of eboot.bin."),
    HelpOption("--help", "Print this help and exit."),
    HelpOption("-h", "Same as --help."),
};

constexpr bool HasUniqueNames() {
	for (size_t i = 0; i < std::size(OPTION_DEFS); i++) {
		for (size_t j = i + 1; j < std::size(OPTION_DEFS); j++) {
			if (OPTION_DEFS[i].spec.name == OPTION_DEFS[j].spec.name) {
				return false;
			}
		}
	}
	return true;
}
static_assert(HasUniqueNames());

constexpr auto OPTION_SPECS = [] {
	std::array<OptionSpec, std::size(OPTION_DEFS)> specs {};
	for (size_t i = 0; i < specs.size(); i++) {
		specs[i] = OPTION_DEFS[i].spec;
	}
	return specs;
}();

const OptionDef* FindOption(std::string_view name) {
	const auto* def =
	    std::ranges::find(OPTION_DEFS, name, [](const OptionDef& d) { return d.spec.name; });
	return def != std::end(OPTION_DEFS) ? def : nullptr;
}

ParseOutcome ErrorOutcome(std::string error) {
	ParseOutcome outcome {};
	outcome.status = ParseOutcome::Error;
	outcome.error  = std::move(error);
	return outcome;
}

std::string OptionLabel(const OptionSpec& option) {
	if (option.value_hint.empty()) {
		return std::string(option.name);
	}
	return fmt::format("{} {}", option.name, option.value_hint);
}

} // namespace

std::span<const OptionSpec> Options() {
	return OPTION_SPECS;
}

ParseOutcome ParseOptions(std::span<const std::string> args) {
	ParseOutcome outcome {};
	auto&        options = outcome.options;
	bool         help    = args.empty();

	for (size_t index = 0; index < args.size(); index++) {
		const std::string& arg = args[index];
		const OptionDef*   def = FindOption(arg);

		// An unknown option is rejected before the argument after it is read as a value.
		if (def == nullptr) {
			if (arg.starts_with('-')) {
				return ErrorOutcome(fmt::format("unknown option: {}", arg));
			}
			return ErrorOutcome(
			    fmt::format("unexpected argument: {} (pass the game with --game)", arg));
		}

		if (def->spec.kind == OptionKind::Help) {
			help = true;
			continue;
		}

		if (def->spec.kind == OptionKind::Flag) {
			options.config.*def->field = def->value;
			continue;
		}

		if (index + 1 >= args.size()) {
			return ErrorOutcome(fmt::format("missing value for {}", arg));
		}

		index++;
		if (auto error = def->apply(options, def->spec.name, args[index]); !error.empty()) {
			return ErrorOutcome(std::move(error));
		}

		if (def->spec.kind == OptionKind::Terminal) {
			const auto rest = args.subspan(index + 1);
			options.config.guest_arguments.assign(rest.begin(), rest.end());
			break;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	if (help) {
		outcome.status = ParseOutcome::Help;
		return outcome;
	}

	if (options.app0_dir.empty() || options.elf.empty()) {
		return ErrorOutcome("missing required option: --game");
	}

	outcome.status = ParseOutcome::Run;
	return outcome;
}

std::string UsageText() {
	size_t width = 0;
	for (const auto& option: Options()) {
		width = std::max(width, OptionLabel(option).size());
	}

	// Two spaces of indent, the padded label, then two spaces before the help column.
	const std::string continuation = "\n" + std::string(width + 4, ' ');

	std::string text = "kyty_emulator --game <dir|elf|zar> [options]\n\nOptions:\n";
	for (const auto& option: Options()) {
		std::string help;
		for (char c: option.help) {
			if (c == '\n') {
				help += continuation;
			} else {
				help += c;
			}
		}
		text += fmt::format("  {:<{}}  {}\n", OptionLabel(option), width, help);
	}
	return text;
}

} // namespace Emulator
