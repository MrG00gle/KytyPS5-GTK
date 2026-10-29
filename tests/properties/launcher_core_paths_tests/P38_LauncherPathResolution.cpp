// Feature: linux-gtk-modernization, Property 38: Launcher path resolution
//
// Validates Requirements 12.1, 12.3, 12.6, 12.7. Each case draws the Flatpak marker (present or
// absent), XDG_CONFIG_HOME, XDG_DATA_HOME, and HOME (each unset, empty, an absolute path, or a
// relative path, spelled with ".", "..", repeated and trailing separators), a Kyty.ini in the
// launcher working directory (present or absent, plus decoy Kyty.ini files next to the emulator and
// in HOME), an absolute launcher working directory, and an absolute or relative emulator
// directory. ResolvePaths on a FakeEnvironment must match a reference model written with its own
// path normalizer:
// - inside Flatpak: $XDG_CONFIG_HOME/Kyty/Kyty.ini (the working-directory file ignored) and
//   $XDG_DATA_HOME/Kyty;
// - outside: the working-directory Kyty.ini when it exists, else $XDG_CONFIG_HOME/Kyty/Kyty.ini;
//   the Data_Directory is the emulator directory;
// - an unset, empty, or relative XDG value falls back to $HOME/.config or $HOME/.local/share (as
//   Qt does), an unset or empty HOME is "/", and a relative HOME resolves against the working
//   directory;
// - every launcher data file (compatibility cache, cheat files, trophy data, save data) resolves
//   directly below the Data_Directory, and the spawn working directory equals it;
// - every path is absolute and lexically normal, without a trailing separator.

#include "launcher/core/dataPaths.h"
#include "support/fakeEnvironment.h"
#include "support/property.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// A uniform choice from [min, max).
int Choose(int min, int max) {
	return *rc::gen::resize(100, rc::gen::inRange(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

std::string Components(int min, int max) {
	static const std::vector<std::string> kPool = {"a",   "b c", "Kyty", ".",     "..", "\xC3\xA9",
	                                               "x.y", "-",   "data", "$HOME", "~",  ".config"};
	std::string                           text;
	const int                             count = Choose(min, max + 1);
	for (int i = 0; i < count; i++) {
		if (i > 0) {
			text += OneIn(5) ? "//" : "/";
		}
		text += kPool[static_cast<size_t>(Choose(0, static_cast<int>(kPool.size())))];
	}
	return text;
}

std::string AbsolutePath() {
	std::string text = "/" + Components(0, 4);
	if (OneIn(4)) {
		text += "/";
	}
	return text;
}

std::string RelativePath() {
	std::string text = Components(1, 3);
	if (OneIn(4)) {
		text += "/";
	}
	return text;
}

enum class Kind { Unset, Empty, Absolute, Relative };

struct Variable {
	Kind        kind = Kind::Unset;
	std::string value;
};

Variable DrawVariable() {
	Variable variable;
	variable.kind = static_cast<Kind>(Choose(0, 4));
	if (variable.kind == Kind::Absolute) {
		variable.value = AbsolutePath();
	} else if (variable.kind == Kind::Relative) {
		variable.value = RelativePath();
	}
	return variable;
}

void Apply(KytyTests::FakeEnvironment& env, const char* name, const Variable& variable) {
	if (variable.kind != Kind::Unset) {
		env.Set(name, variable.value);
	}
}

// The reference normalizer: an absolute path with "", ".", and ".." components resolved.
std::string Normalize(const std::string& absolute) {
	std::vector<std::string> parts;
	size_t                   begin = 0;
	while (begin <= absolute.size()) {
		size_t end = absolute.find('/', begin);
		if (end == std::string::npos) {
			end = absolute.size();
		}
		const std::string part = absolute.substr(begin, end - begin);
		if (part == "..") {
			if (!parts.empty()) {
				parts.pop_back();
			}
		} else if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		begin = end + 1;
	}
	std::string text;
	for (const auto& part: parts) {
		text += "/" + part;
	}
	return text.empty() ? "/" : text;
}

bool IsCleanAbsolute(const fs::path& path) {
	return path.is_absolute() && path == path.lexically_normal() &&
	       (path == "/" || path.has_filename());
}

} // namespace

KYTY_PROPERTY(38, "Launcher path resolution") {
	return rc::check(name, []() {
		const bool     flatpak           = OneIn(2);
		const bool     portable          = OneIn(2);
		const Variable config_home       = DrawVariable();
		const Variable data_home         = DrawVariable();
		const Variable home              = DrawVariable();
		const auto     cwd               = AbsolutePath();
		const bool     relative_emulator = OneIn(3);
		const auto     emulator_dir      = relative_emulator ? RelativePath() : AbsolutePath();

		KytyTests::FakeEnvironment env;
		Apply(env, "XDG_CONFIG_HOME", config_home);
		Apply(env, "XDG_DATA_HOME", data_home);
		Apply(env, "HOME", home);
		if (flatpak) {
			env.AddExistingPath("/.flatpak-info");
		}
		if (portable) {
			env.AddExistingPath(cwd + "/Kyty.ini");
		}
		// Decoys, unless they are the working-directory file itself.
		const auto emulator_absolute = relative_emulator ? cwd + "/" + emulator_dir : emulator_dir;
		if (OneIn(2) && Normalize(emulator_absolute) != Normalize(cwd)) {
			env.AddExistingPath(emulator_absolute + "/Kyty.ini");
		}
		if (OneIn(2) && home.kind == Kind::Absolute && Normalize(home.value) != Normalize(cwd)) {
			env.AddExistingPath(home.value + "/Kyty.ini");
		}
		RC_LOG() << "flatpak " << flatpak << " portable " << portable << "\ncwd '" << cwd
		         << "'\nemulator '" << emulator_dir << "'\nXDG_CONFIG_HOME "
		         << static_cast<int>(config_home.kind) << " '" << config_home.value
		         << "'\nXDG_DATA_HOME " << static_cast<int>(data_home.kind) << " '"
		         << data_home.value << "'\nHOME " << static_cast<int>(home.kind) << " '"
		         << home.value << "'\n";

		// The model.
		std::string home_dir = "/";
		if (home.kind == Kind::Absolute) {
			home_dir = home.value;
		} else if (home.kind == Kind::Relative) {
			home_dir = cwd + "/" + home.value;
		}
		const std::string config_dir =
		    config_home.kind == Kind::Absolute ? config_home.value : home_dir + "/.config";
		const std::string data_base =
		    data_home.kind == Kind::Absolute ? data_home.value : home_dir + "/.local/share";
		std::string settings;
		std::string data_dir;
		if (flatpak) {
			settings = Normalize(config_dir + "/Kyty/Kyty.ini");
			data_dir = Normalize(data_base + "/Kyty");
		} else {
			settings =
			    portable ? Normalize(cwd + "/Kyty.ini") : Normalize(config_dir + "/Kyty/Kyty.ini");
			data_dir = Normalize(emulator_absolute);
		}

		const auto paths = kyty::core::ResolvePaths(env, cwd, emulator_dir);
		RC_ASSERT(paths.flatpak == flatpak);
		RC_ASSERT(paths.settings_file == fs::path(settings));
		RC_ASSERT(paths.data_dir == fs::path(data_dir));
		RC_ASSERT(IsCleanAbsolute(paths.settings_file));
		RC_ASSERT(IsCleanAbsolute(paths.data_dir));
		RC_ASSERT(paths.settings_file.filename() == "Kyty.ini");
		RC_ASSERT(kyty::core::SettingsDirectory(paths) == paths.settings_file.parent_path());
		RC_ASSERT(kyty::core::SpawnWorkingDirectory(paths) == paths.data_dir);
		for (const auto& file: {kyty::core::CompatibilityCachePath(paths.data_dir),
		                        kyty::core::CheatDirectory(paths.data_dir),
		                        kyty::core::TrophyDataDirectory(paths.data_dir),
		                        kyty::core::SaveDataDirectory(paths.data_dir)}) {
			RC_ASSERT(file.parent_path() == paths.data_dir);
			RC_ASSERT(IsCleanAbsolute(file));
		}

		RC_TAG(flatpak ? "flatpak" : portable ? "host, portable" : "host, user config");
		RC_CLASSIFY(config_home.kind == Kind::Relative || data_home.kind == Kind::Relative,
		            "a relative XDG value ignored");
		RC_CLASSIFY(home.kind == Kind::Unset || home.kind == Kind::Empty, "HOME is /");
	});
}
