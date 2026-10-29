#include "launcher/core/settingsStore.h"

#include "launcher/core/configurationFields.h"

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

constexpr std::string_view kLauncher           = "Launcher/";
constexpr std::string_view kMainDialog         = "MainDialog/";
constexpr std::string_view kLauncherWindow     = "LauncherWindow/";
constexpr std::string_view kGlobal             = "GlobalConfiguration/";
constexpr std::string_view kGameConfigurations = "GameConfigurations";
constexpr std::string_view kElementPrefix      = "GameConfigurations/";

std::string Key(std::string_view prefix, std::string_view name) {
	std::string key(prefix);
	key += name;
	return key;
}

// "GameConfigurations/<index + 1>/" for the 0-based index.
std::string ElementPrefix(size_t index) {
	return Key(kElementPrefix, std::to_string(index + 1)) + "/";
}

// A QSettings array index: a decimal number without leading zeros, from 1.
bool IsIndexSegment(std::string_view segment) {
	return !segment.empty() && segment.front() >= '1' && segment.front() <= '9' &&
	       std::all_of(segment.begin(), segment.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The element keys besides the fields.
constexpr std::string_view kElementKeys[] = {"name", "basedir", "game_path", "custom_settings"};

// True for a key below an element that the schema owns: one of kElementKeys or a field, or a key
// below one of them.
bool IsElementSchemaKey(std::string_view sub_key) {
	const auto first = sub_key.substr(0, sub_key.find('/'));
	return std::find(std::begin(kElementKeys), std::end(kElementKeys), first) !=
	           std::end(kElementKeys) ||
	       FindGameConfigurationField(first) != nullptr;
}

// A key that IniFile::Set stores as it is (see IniFile: no empty, leading, trailing, or double
// '/').
bool IsNormalKey(std::string_view key) {
	return !key.empty() && key.front() != '/' && key.back() != '/' &&
	       key.find("//") == std::string_view::npos;
}

// Removes every key that LoadSettings reads or that SerializeSettings writes, with the keys below
// them, and every GameConfigurations element (also those above size); other keys stay.
void RemoveSchemaKeys(IniFile& file) {
	for (const auto name: {"game_dirs", "game_dir", "addons_dir", "grid_view", "grid_icon_width"}) {
		file.Remove(Key(kLauncher, name));
	}
	for (const auto name: {"check_updates_on_startup", "theme", "geometry"}) {
		file.Remove(Key(kMainDialog, name));
	}
	for (const auto name: {"width", "height", "maximized"}) {
		file.Remove(Key(kLauncherWindow, name));
	}
	for (const auto& field: GameConfigurationFields()) {
		if (field.name != kInputMappingCustomField) {
			file.Remove(Key(kGlobal, field.name));
		}
	}
	for (const auto& field: GlobalOnlyFields()) {
		file.Remove(Key(kGlobal, field.name));
	}
	file.Remove(Key(kElementPrefix, "size"));
	std::vector<std::string> elements;
	for (const auto& [key, value]: file.Entries()) {
		if (!key.starts_with(kElementPrefix)) {
			continue;
		}
		const auto rest  = std::string_view(key).substr(kElementPrefix.size());
		const auto slash = rest.find('/');
		if (slash != std::string_view::npos && IsIndexSegment(rest.substr(0, slash))) {
			elements.push_back(Key(kElementPrefix, rest.substr(0, slash)));
		}
	}
	for (const auto& element: elements) {
		file.Remove(element);
	}
}

// Reads typed values from a parsed Kyty.ini and records the keys that get their default.
class Loader {
public:
	Loader(const IniFile& file, std::vector<std::string>& defaulted)
	    : m_file(file), m_defaulted(defaulted) {}

	void Report(std::string key) { m_defaulted.push_back(std::move(key)); }

	[[nodiscard]] bool Has(const std::string& key) const { return m_file.Find(key) != nullptr; }

	// The string at key; nothing when the key is missing (reported unless optional) or its value is
	// not a string (a list or an opaque value; reported).
	std::optional<std::string> Text(const std::string& key, bool optional) {
		if (!Has(key)) {
			if (!optional) {
				Report(key);
			}
			return std::nullopt;
		}
		auto text = m_file.GetString(key);
		if (!text) {
			Report(key);
		}
		return text;
	}

	void Bool(const std::string& key, bool optional, bool& target) {
		if (const auto text = Text(key, optional)) {
			if (const auto value = ParseBoolText(*text)) {
				target = *value;
			} else {
				Report(key);
			}
		}
	}

	template <typename Valid>
	void Int(const std::string& key, bool optional, int32_t& target, Valid valid) {
		if (const auto text = Text(key, optional)) {
			const auto value = ParseIntText(*text);
			if (value && *value >= std::numeric_limits<int32_t>::min() &&
			    *value <= std::numeric_limits<int32_t>::max() && valid(*value)) {
				target = static_cast<int32_t>(*value);
			} else {
				Report(key);
			}
		}
	}

	template <typename Target>
	void Field(const BasicFieldDescriptor<Target>& field, const std::string& key, bool optional,
	           Target& target) {
		if (!Has(key)) {
			if (!optional) {
				Report(key);
			}
			return;
		}
		std::optional<FieldValue> value;
		if (field.kind == FieldKind::StringList) {
			if (auto list = m_file.GetStringList(key)) {
				value = FieldValue(std::move(*list));
			}
		} else if (const auto text = m_file.GetString(key)) {
			value = field.parse(*text);
		}
		if (!value || field.check(*value) || !field.set(target, *value)) {
			Report(key);
		}
	}

	// Every field under prefix; per_game adds input_mapping_custom (optional). Then each output
	// path that is empty while its output is on gets its default.
	void Configuration(std::string_view prefix, bool per_game, GameConfiguration& configuration) {
		for (const auto& field: GameConfigurationFields()) {
			const bool mapping_custom = field.name == kInputMappingCustomField;
			if (mapping_custom && !per_game) {
				continue;
			}
			Field(field, Key(prefix, field.name), mapping_custom, configuration);
		}
		const GameConfiguration defaults;
		for (const auto& field: GameConfigurationFields()) {
			if (IsMissingOutputPath(field.name, configuration)) {
				field.set(configuration, field.get(defaults));
				Report(Key(prefix, field.name));
			}
		}
	}

private:
	const IniFile&            m_file;
	std::vector<std::string>& m_defaulted;
};

void LoadLauncher(const IniFile& file, Loader& loader, LauncherSettings& settings) {
	const auto game_dirs = Key(kLauncher, "game_dirs");
	const auto game_dir  = Key(kLauncher, "game_dir");
	// A store from a launcher before game_dirs: the keys that came later are optional.
	const bool legacy = !loader.Has(game_dirs) && loader.Has(game_dir);
	if (loader.Has(game_dirs)) {
		if (auto folders = file.GetStringList(game_dirs)) {
			settings.game_folders = std::move(*folders);
			// The Qt launcher read game_dir when game_dirs gave no folder.
			if (settings.game_folders.empty() && loader.Has(game_dir)) {
				settings.game_folders =
				    file.GetStringList(game_dir).value_or(settings.game_folders);
			}
		} else {
			loader.Report(game_dirs);
		}
	} else if (legacy) {
		if (auto folders = file.GetStringList(game_dir)) {
			settings.game_folders = std::move(*folders);
		} else {
			loader.Report(game_dir);
		}
	} else {
		loader.Report(game_dirs);
	}
	if (auto addons = loader.Text(Key(kLauncher, "addons_dir"), legacy)) {
		settings.addons_dir = std::move(*addons);
	}
	loader.Bool(Key(kLauncher, "grid_view"), legacy, settings.grid_view);
	loader.Int(Key(kLauncher, "grid_icon_width"), legacy, settings.grid_icon_width,
	           IsValidGridIconWidth);
}

void LoadMainDialog(const IniFile& file, Loader& loader, LauncherSettings& settings) {
	loader.Bool(Key(kMainDialog, "check_updates_on_startup"), false,
	            settings.check_updates_on_startup);
	settings.theme    = file.GetRaw(Key(kMainDialog, "theme"));
	settings.geometry = file.GetRaw(Key(kMainDialog, "geometry"));
}

void LoadLauncherWindow(Loader& loader, LauncherSettings& settings) {
	const auto positive = [](int64_t value) { return value >= 1; };
	loader.Int(Key(kLauncherWindow, "width"), true, settings.window.width, positive);
	loader.Int(Key(kLauncherWindow, "height"), true, settings.window.height, positive);
	loader.Bool(Key(kLauncherWindow, "maximized"), true, settings.window.maximized);
}

void LoadGlobal(Loader& loader, LauncherSettings& settings) {
	loader.Configuration(kGlobal, false, settings.global);
	for (const auto& field: GlobalOnlyFields()) {
		loader.Field(field, Key(kGlobal, field.name), false, settings.controller);
	}
}

void LoadGames(const IniFile& file, Loader& loader, LauncherSettings& settings) {
	int64_t size = 0;
	if (const auto text = loader.Text(Key(kElementPrefix, "size"), false)) {
		const auto value = ParseIntText(*text);
		if (value && *value >= 0) {
			size = *value;
		} else {
			loader.Report(Key(kElementPrefix, "size"));
		}
	}

	// The keys of each element from 1 to size that has any, by the part after "N/".
	std::map<int64_t, std::vector<std::pair<std::string, const IniValue*>>> elements;
	for (const auto& [key, value]: file.Entries()) {
		if (!key.starts_with(kElementPrefix)) {
			continue;
		}
		const auto rest  = std::string_view(key).substr(kElementPrefix.size());
		const auto slash = rest.find('/');
		if (slash == std::string_view::npos || !IsIndexSegment(rest.substr(0, slash))) {
			continue;
		}
		int64_t    index  = 0;
		const auto digits = rest.substr(0, slash);
		const auto [end, err] =
		    std::from_chars(digits.data(), digits.data() + digits.size(), index);
		if (err != std::errc {} || index > size) {
			continue;
		}
		elements[index].emplace_back(std::string(rest.substr(slash + 1)), &value);
	}

	for (const auto& [index, keys]: elements) {
		const auto prefix    = Key(kElementPrefix, std::to_string(index)) + "/";
		const auto path_key  = prefix + "game_path";
		const auto game_path = loader.Text(path_key, false);
		if (!game_path || game_path->empty()) {
			if (game_path) {
				loader.Report(path_key);
			}
			continue;
		}
		GameSettingsEntry entry;
		entry.game_path = *game_path;
		if (auto name = loader.Text(prefix + "name", false)) {
			entry.name = std::move(*name);
		}
		if (auto basedir = loader.Text(prefix + "basedir", false)) {
			entry.basedir = std::move(*basedir);
		}
		// Stored false still means custom (Qt's ReadSettings sets custom_settings = true).
		if (const auto custom = loader.Text(prefix + "custom_settings", false)) {
			if (!ParseBoolText(*custom)) {
				loader.Report(prefix + "custom_settings");
			}
		}
		loader.Configuration(prefix, true, entry.configuration);
		for (const auto& [sub_key, value]: keys) {
			if (!IsElementSchemaKey(sub_key) && IsNormalKey(sub_key)) {
				entry.unknown.emplace(sub_key, *value);
			}
		}
		std::erase_if(settings.games, [&entry](const GameSettingsEntry& other) {
			return other.game_path == entry.game_path;
		});
		settings.games.push_back(std::move(entry));
	}
}

template <typename Target>
void WriteField(IniFile& out, const BasicFieldDescriptor<Target>& field, const std::string& key,
                const Target& target) {
	const auto value = field.get(target);
	if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
		out.SetStringList(key, *list);
	} else {
		out.SetString(key, field.format(value));
	}
}

void WriteConfiguration(IniFile& out, std::string_view prefix, bool per_game,
                        const GameConfiguration& configuration) {
	for (const auto& field: GameConfigurationFields()) {
		if (per_game || field.name != kInputMappingCustomField) {
			WriteField(out, field, Key(prefix, field.name), configuration);
		}
	}
}

bool IsRelativeGamePath(std::string_view game_path) {
	return !game_path.empty() && game_path.front() != '/';
}

bool MatchesAbsolute(const GameSettingsEntry& settings, const GameEntry& entry) {
	return settings.game_path == entry.path.string();
}

bool MatchesLegacy(const GameSettingsEntry& settings, const GameEntry& entry) {
	return IsRelativeGamePath(settings.game_path) && !entry.legacy_relative_path.empty() &&
	       settings.game_path == entry.legacy_relative_path;
}

bool Matches(const GameSettingsEntry& settings, const GameEntry& entry) {
	return MatchesAbsolute(settings, entry) || MatchesLegacy(settings, entry);
}

} // namespace

bool IsValidGridIconWidth(int64_t width) {
	return width >= kMinGridIconWidth && width <= kMaxGridIconWidth &&
	       width % kGridIconWidthStep == 0;
}

LoadReport ParseSettings(std::string_view bytes) {
	const IniFile file = IniFile::Parse(bytes);
	LoadReport    report;
	Loader        loader(file, report.defaulted_keys);
	auto&         settings = report.settings;
	LoadLauncher(file, loader, settings);
	LoadMainDialog(file, loader, settings);
	LoadLauncherWindow(loader, settings);
	LoadGlobal(loader, settings);
	LoadGames(file, loader, settings);
	settings.unknown = file;
	RemoveSchemaKeys(settings.unknown);
	return report;
}

std::string SerializeSettings(const LauncherSettings& settings) {
	IniFile out = settings.unknown;
	RemoveSchemaKeys(out);

	out.SetStringList(Key(kLauncher, "game_dirs"), settings.game_folders);
	out.SetString(Key(kLauncher, "addons_dir"), settings.addons_dir);
	out.SetString(Key(kLauncher, "grid_view"), FormatBoolText(settings.grid_view));
	out.SetString(Key(kLauncher, "grid_icon_width"), std::to_string(settings.grid_icon_width));

	out.SetString(Key(kMainDialog, "check_updates_on_startup"),
	              FormatBoolText(settings.check_updates_on_startup));
	if (settings.theme) {
		out.SetRaw(Key(kMainDialog, "theme"), *settings.theme);
	}
	if (settings.geometry) {
		out.SetRaw(Key(kMainDialog, "geometry"), *settings.geometry);
	}

	out.SetString(Key(kLauncherWindow, "width"), std::to_string(settings.window.width));
	out.SetString(Key(kLauncherWindow, "height"), std::to_string(settings.window.height));
	out.SetString(Key(kLauncherWindow, "maximized"), FormatBoolText(settings.window.maximized));

	WriteConfiguration(out, kGlobal, false, settings.global);
	for (const auto& field: GlobalOnlyFields()) {
		WriteField(out, field, Key(kGlobal, field.name), settings.controller);
	}

	for (size_t i = 0; i < settings.games.size(); i++) {
		const auto& entry  = settings.games[i];
		const auto  prefix = ElementPrefix(i);
		for (const auto& [sub_key, value]: entry.unknown) {
			if (!IsElementSchemaKey(sub_key)) {
				out.Set(prefix + sub_key, value);
			}
		}
		out.SetString(prefix + "name", entry.name);
		out.SetString(prefix + "basedir", entry.basedir);
		out.SetString(prefix + "game_path", entry.game_path);
		out.SetString(prefix + "custom_settings", FormatBoolText(true));
		WriteConfiguration(out, prefix, true, entry.configuration);
	}
	out.SetArraySize(kGameConfigurations, static_cast<int>(settings.games.size()));
	return out.Serialize();
}

Expected<LoadReport, IoError> LoadSettings(const std::filesystem::path& ini) {
	auto bytes = ReadFileLimited(ini, kMaxSettingsFileBytes);
	if (!bytes) {
		return MakeUnexpected(std::move(bytes).error());
	}
	return ParseSettings(*bytes);
}

Expected<void, IoError> SaveSettings(const std::filesystem::path& ini,
                                     const LauncherSettings&      settings) {
	return WriteFileAtomically(ini, SerializeSettings(settings));
}

const GameSettingsEntry* FindGameSettings(const LauncherSettings& settings,
                                          const GameEntry&        entry) {
	for (const auto& game: settings.games) {
		if (MatchesAbsolute(game, entry)) {
			return &game;
		}
	}
	for (const auto& game: settings.games) {
		if (MatchesLegacy(game, entry)) {
			return &game;
		}
	}
	return nullptr;
}

bool HasCustomSettings(const LauncherSettings& settings, const GameEntry& entry) {
	return FindGameSettings(settings, entry) != nullptr;
}

GameConfiguration EnableCustomSettings(const LauncherSettings& settings, const GameEntry& entry) {
	if (const auto* custom = FindGameSettings(settings, entry)) {
		return custom->configuration;
	}
	auto configuration                 = settings.global;
	configuration.input_mapping_custom = false;
	return configuration;
}

Expected<void, ValidationError> SaveCustomSettings(LauncherSettings&        settings,
                                                   const GameEntry&         entry,
                                                   const GameConfiguration& configuration) {
	if (auto error = Validate(configuration)) {
		return MakeUnexpected(std::move(*error));
	}
	const auto* found    = FindGameSettings(settings, entry);
	const auto  position = found != nullptr ? static_cast<size_t>(found - settings.games.data())
	                                        : settings.games.size();

	GameSettingsEntry updated;
	updated.name          = entry.title_name;
	updated.basedir       = entry.path.string();
	updated.game_path     = entry.path.string();
	updated.configuration = configuration;
	if (found != nullptr) {
		updated.unknown = found->unknown;
	}

	std::vector<GameSettingsEntry> games;
	bool                           inserted = false;
	for (size_t i = 0; i < settings.games.size(); i++) {
		if (i == position) {
			games.push_back(updated);
			inserted = true;
		} else if (!Matches(settings.games[i], entry)) {
			games.push_back(std::move(settings.games[i]));
		}
	}
	if (!inserted) {
		games.push_back(std::move(updated));
	}
	settings.games = std::move(games);
	return {};
}

bool ClearCustomSettings(LauncherSettings& settings, const GameEntry& entry) {
	return std::erase_if(settings.games, [&entry](const GameSettingsEntry& game) {
		       return Matches(game, entry);
	       }) > 0;
}

bool MigrateLegacyGamePaths(LauncherSettings& settings, std::span<const GameEntry> entries) {
	bool changed = false;
	for (auto& game: settings.games) {
		if (!IsRelativeGamePath(game.game_path)) {
			continue;
		}
		for (const auto& entry: entries) {
			if (entry.legacy_relative_path.empty() ||
			    entry.legacy_relative_path != game.game_path) {
				continue;
			}
			const auto absolute = entry.path.string();
			const bool taken    = std::any_of(settings.games.begin(), settings.games.end(),
			                                  [&absolute](const GameSettingsEntry& other) {
				                               return other.game_path == absolute;
			                                  });
			if (!taken) {
				game.game_path = absolute;
				changed        = true;
			}
			break;
		}
	}
	return changed;
}

EffectiveConfiguration
ResolveEffectiveConfiguration(const LauncherSettings& settings, const GameEntry& entry,
                              std::optional<std::filesystem::path> cheat_plan) {
	EffectiveConfiguration result;
	const auto*            custom = FindGameSettings(settings, entry);
	result.configuration          = custom != nullptr ? custom->configuration : settings.global;
	if (custom == nullptr || !custom->configuration.input_mapping_custom) {
		result.configuration.host_input_mapping = settings.global.host_input_mapping;
	}
	result.controller = settings.controller;
	result.addons_dir = settings.addons_dir;
	result.cheat_plan = std::move(cheat_plan);
	return result;
}

} // namespace kyty::core
