// Feature: linux-gtk-modernization, Property 39: Emulator Data_Directory resolution
//
// Validates Requirement 12.2. Each case draws the Flatpak marker (present or absent),
// XDG_DATA_HOME and HOME (each unset, empty, an absolute path, or a relative path, spelled with
// ".", "..", repeated and trailing separators), an absolute original working directory, and for
// each of --game (also as an archive member path), --game-patch, --addcont-dir,
// --printf-output-file, --shader-log-folder, and --command-buffer-dump-folder an empty, relative,
// or absolute value; inside Flatpak some cases inject a failure of the directory creation or of
// chdir. Emulator::ApplyDataDirectory with recording callbacks must match a reference model:
// - outside Flatpak: nothing is created or entered, and every path is unchanged;
// - inside: the Data_Directory is $XDG_DATA_HOME/Kyty, equal to the launcher's
//   (kyty::core::ResolvePaths on a FakeEnvironment with the same variables), and it is created and
//   then entered; a relative game, patch, or add-on path becomes <cwd>/<value> and a relative log,
//   shader, or buffer path <Data_Directory>/<value>; absolute and empty paths are unchanged; an
//   archive game stays an archive member path;
// - a failure returns "cannot create|enter the data directory <path>: <strerror>" and leaves every
//   path unchanged.

#include "common/archive.h"
#include "emulatorOptions.h"
#include "launcher/core/dataPaths.h"
#include "support/fakeEnvironment.h"
#include "support/property.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
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

std::optional<std::string> AsOptional(const Variable& variable) {
	return variable.kind == Kind::Unset ? std::nullopt : std::optional<std::string>(variable.value);
}

// An empty, relative, or absolute option value.
std::string DrawPath() {
	switch (Choose(0, 3)) {
		case 0: return {};
		case 1: return RelativePath();
		default: return AbsolutePath();
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

// The model of a rewritten path: unchanged when empty or absolute, else base/value.
std::string Against(const std::string& value, const std::string& base) {
	if (value.empty() || value.front() == '/') {
		return value;
	}
	return (base == "/" ? std::string() : base) + "/" + value;
}

struct Paths {
	std::string game;
	std::string patch;
	std::string addcont;
	std::string printf_file;
	std::string shader_folder;
	std::string buffer_folder;
};

Paths PathsOf(const Emulator::RunOptions& options) {
	return {options.app0_dir.native(),
	        options.game_patch.native(),
	        options.addcont_dir.native(),
	        options.config.printf_output_file.native(),
	        options.config.shader_log_folder.native(),
	        options.config.command_buffer_dump_folder.native()};
}

// Byte for byte: the paths were not touched.
bool SameText(const Paths& a, const Paths& b) {
	return a.game == b.game && a.patch == b.patch && a.addcont == b.addcont &&
	       a.printf_file == b.printf_file && a.shader_folder == b.shader_folder &&
	       a.buffer_folder == b.buffer_folder;
}

// Element by element (std::filesystem::path equality, as P38 compares): a path that starts with
// "//" keeps it, in kyty::core::ResolvePaths as in the Emulator, and Linux resolves it as "/".
bool SamePaths(const Paths& a, const Paths& b) {
	const auto same = [](const std::string& x, const std::string& y) {
		return fs::path(x) == fs::path(y);
	};
	return same(a.game, b.game) && same(a.patch, b.patch) && same(a.addcont, b.addcont) &&
	       same(a.printf_file, b.printf_file) && same(a.shader_folder, b.shader_folder) &&
	       same(a.buffer_folder, b.buffer_folder);
}

} // namespace

KYTY_PROPERTY(39, "Emulator Data_Directory resolution") {
	return rc::check(name, []() {
		const bool     flatpak   = OneIn(2);
		const Variable data_home = DrawVariable();
		const Variable home      = DrawVariable();
		const auto     cwd       = AbsolutePath();
		const bool     archive   = OneIn(4);
		Paths drawn {DrawPath(), DrawPath(), DrawPath(), DrawPath(), DrawPath(), DrawPath()};
		if (archive) {
			// "<dir>/Title.zar!", the app0 root of an archive game.
			const std::string dir = OneIn(2) ? RelativePath() : AbsolutePath();
			drawn.game =
			    Common::MakeArchivePath(dir + (dir.ends_with('/') ? "" : "/") + "Title.zar")
			        .native();
		}
		// 0: no failure, 1: creating the directory fails, 2: entering it fails.
		const int failure     = flatpak && OneIn(3) ? Choose(1, 3) : 0;
		const int kErrors[]   = {EACCES, ENOENT, EROFS, ENOSPC, ENOTDIR};
		const int error_value = kErrors[Choose(0, 5)];
		RC_LOG() << "flatpak " << flatpak << " failure " << failure << "\ncwd '" << cwd
		         << "'\nXDG_DATA_HOME " << static_cast<int>(data_home.kind) << " '"
		         << data_home.value << "'\nHOME " << static_cast<int>(home.kind) << " '"
		         << home.value << "'\ngame '" << drawn.game << "' patch '" << drawn.patch
		         << "' addcont '" << drawn.addcont << "'\nprintf '" << drawn.printf_file
		         << "' shaders '" << drawn.shader_folder << "' buffers '" << drawn.buffer_folder
		         << "'\n";

		Emulator::RunOptions options;
		options.app0_dir                          = drawn.game;
		options.elf                               = "/app0/eboot.bin";
		options.game_patch                        = drawn.patch;
		options.addcont_dir                       = drawn.addcont;
		options.config.printf_output_file         = drawn.printf_file;
		options.config.shader_log_folder          = drawn.shader_folder;
		options.config.command_buffer_dump_folder = drawn.buffer_folder;

		std::vector<std::string>           calls;
		Emulator::DataDirectoryEnvironment env;
		env.flatpak            = flatpak;
		env.xdg_data_home      = AsOptional(data_home);
		env.home               = AsOptional(home);
		env.cwd                = cwd;
		env.create_directories = [&](const fs::path& dir) {
			calls.push_back("create " + dir.native());
			return failure == 1 ? error_value : 0;
		};
		env.change_directory = [&](const fs::path& dir) {
			calls.push_back("enter " + dir.native());
			return failure == 2 ? error_value : 0;
		};

		// The model.
		std::string home_dir = "/";
		if (home.kind == Kind::Absolute) {
			home_dir = home.value;
		} else if (home.kind == Kind::Relative) {
			home_dir = cwd + "/" + home.value;
		}
		const std::string data_base =
		    data_home.kind == Kind::Absolute ? data_home.value : home_dir + "/.local/share";
		const std::string data_dir = Normalize(data_base + "/Kyty");
		const std::string base     = Normalize(cwd);

		const auto error = Emulator::ApplyDataDirectory(options, env);
		RC_ASSERT(options.elf == fs::path("/app0/eboot.bin"));
		if (!flatpak) {
			RC_ASSERT(error.empty());
			RC_ASSERT(calls.empty());
			RC_ASSERT(SameText(PathsOf(options), drawn));
			RC_TAG("outside Flatpak");
			return;
		}

		const auto dir = Emulator::FlatpakDataDirectory(env);
		RC_ASSERT(dir == fs::path(data_dir));
		RC_ASSERT(dir.is_absolute() && dir.filename() == "Kyty");
		// The launcher puts the same Data_Directory there.
		KytyTests::FakeEnvironment launcher_env;
		if (data_home.kind != Kind::Unset) {
			launcher_env.Set("XDG_DATA_HOME", data_home.value);
		}
		if (home.kind != Kind::Unset) {
			launcher_env.Set("HOME", home.value);
		}
		launcher_env.AddExistingPath("/.flatpak-info");
		const auto launcher = kyty::core::ResolvePaths(launcher_env, cwd, "/emulator");
		RC_ASSERT(launcher.flatpak);
		RC_ASSERT(launcher.data_dir.native() == dir.native());

		if (failure != 0) {
			const std::string action = failure == 1 ? "create" : "enter";
			RC_ASSERT(error == "cannot " + action + " the data directory " + dir.native() + ": " +
			                       std::strerror(error_value));
			RC_ASSERT(calls.size() == static_cast<size_t>(failure));
			RC_ASSERT(SameText(PathsOf(options), drawn));
			RC_TAG(failure == 1 ? "Flatpak, create fails" : "Flatpak, chdir fails");
			return;
		}

		RC_ASSERT(error.empty());
		RC_ASSERT(calls ==
		          (std::vector<std::string> {"create " + dir.native(), "enter " + dir.native()}));
		const Paths expected {Against(drawn.game, base),
		                      Against(drawn.patch, base),
		                      Against(drawn.addcont, base),
		                      Against(drawn.printf_file, data_dir),
		                      Against(drawn.shader_folder, data_dir),
		                      Against(drawn.buffer_folder, data_dir)};
		RC_ASSERT(SamePaths(PathsOf(options), expected));
		for (const auto& path: {options.app0_dir, options.game_patch, options.addcont_dir,
		                        options.config.printf_output_file, options.config.shader_log_folder,
		                        options.config.command_buffer_dump_folder}) {
			RC_ASSERT(path.empty() || path.is_absolute());
		}
		RC_ASSERT(Common::IsArchivePath(options.app0_dir) == archive);
		if (archive) {
			RC_ASSERT(Common::GetArchiveHostPath(options.app0_dir) ==
			          fs::path(Against(drawn.game.substr(0, drawn.game.size() - 1), base)));
		}

		RC_TAG("Flatpak");
		RC_CLASSIFY(data_home.kind != Kind::Absolute, "XDG_DATA_HOME ignored");
		RC_CLASSIFY(archive, "archive game");
	});
}
