// Emulator::FindAssetFile (task 25.2, Requirement 14.6): the trophy sound and the touchpad cursors
// are looked up in <exe dir>/assets (the portable layout) and then in
// <exe dir>/../share/kyty/assets (the fhs layout of the Flatpak). Checked with a recording
// predicate and with real files in a temporary tree.

#include "emulatorOptions.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "AssetLookupTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void CheckPath(const fs::path& actual, const std::string& expected, const std::string& what) {
	Check(actual.native() == expected,
	      what + " is '" + expected + "', got '" + actual.native() + "'");
}

// The candidates in order, and the first one that exists in the given set.
void TestOrder() {
	const fs::path    exe_dir  = "/opt/kyty/bin/";
	const fs::path    relative = fs::path("sounds") / "trophy-unlock.wav";
	const std::string portable = "/opt/kyty/bin/assets/sounds/trophy-unlock.wav";
	const std::string fhs      = "/opt/kyty/bin/../share/kyty/assets/sounds/trophy-unlock.wav";
	const std::vector<std::set<std::string>> existing = {{}, {portable}, {fhs}, {portable, fhs}};
	const std::vector<std::string>           expected = {portable, portable, fhs, portable};
	for (size_t i = 0; i < existing.size(); i++) {
		std::vector<std::string> asked;
		const auto found = Emulator::FindAssetFile(exe_dir, relative, [&](const fs::path& path) {
			asked.push_back(path.native());
			return existing[i].count(path.native()) != 0;
		});
		const auto what  = "case " + std::to_string(i);
		CheckPath(found, expected[i], what);
		const std::vector<std::string> expected_asked =
		    existing[i].count(portable) != 0 ? std::vector<std::string> {portable}
		                                     : std::vector<std::string> {portable, fhs};
		Check(asked == expected_asked,
		      what + ": the portable path is asked first, then the fhs one");
	}
	// A base path without a trailing separator gives the same candidates.
	CheckPath(Emulator::FindAssetFile("/opt/kyty/bin", fs::path("cursors") / "hand.png",
	                                  [](const fs::path&) { return false; }),
	          "/opt/kyty/bin/assets/cursors/hand.png", "no trailing separator");
}

class TempDirectory {
public:
	TempDirectory() {
		std::string pattern = (fs::temp_directory_path() / "kyty-asset-lookup-XXXXXX").string();
		Check(::mkdtemp(pattern.data()) != nullptr, "mkdtemp");
		m_path = pattern;
	}
	~TempDirectory() {
		std::error_code ec;
		fs::remove_all(m_path, ec);
	}
	TempDirectory(const TempDirectory&)            = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;

	[[nodiscard]] const fs::path& Path() const { return m_path; }

private:
	fs::path m_path;
};

void WriteFile(const fs::path& path) {
	fs::create_directories(path.parent_path());
	std::ofstream(path) << "asset";
	Check(fs::is_regular_file(path), "created " + path.native());
}

// Real trees: a portable one (<exe dir>/assets) and an fhs one (bin/ and share/kyty/assets).
void TestRealTrees() {
	TempDirectory temporary;
	const auto    root = temporary.Path();

	const auto portable = root / "portable";
	WriteFile(portable / "assets" / "sounds" / "trophy-unlock.wav");
	WriteFile(portable / "assets" / "cursors" / "hand.png");
	CheckPath(Emulator::FindAssetFile(portable, "sounds/trophy-unlock.wav"),
	          (portable / "assets/sounds/trophy-unlock.wav").native(), "portable sound");
	CheckPath(Emulator::FindAssetFile(portable, "cursors/hand.png"),
	          (portable / "assets/cursors/hand.png").native(), "portable cursor");

	const auto prefix = root / "fhs";
	fs::create_directories(prefix / "bin");
	WriteFile(prefix / "share" / "kyty" / "assets" / "sounds" / "trophy-unlock.wav");
	WriteFile(prefix / "share" / "kyty" / "assets" / "cursors" / "drawing.png");
	const auto sound = Emulator::FindAssetFile(prefix / "bin", "sounds/trophy-unlock.wav");
	CheckPath(sound, (prefix / "bin/../share/kyty/assets/sounds/trophy-unlock.wav").native(),
	          "fhs sound");
	Check(fs::is_regular_file(sound), "the fhs sound opens through the returned path");
	CheckPath(Emulator::FindAssetFile(prefix / "bin", "cursors/drawing.png"),
	          (prefix / "bin/../share/kyty/assets/cursors/drawing.png").native(), "fhs cursor");

	// A missing asset gives the portable path, which the "not found" messages name.
	CheckPath(Emulator::FindAssetFile(prefix / "bin", "cursors/hand.png"),
	          (prefix / "bin/assets/cursors/hand.png").native(), "missing cursor");
	// A directory with the asset's name in <exe dir>/assets is not the asset: the fhs file wins.
	fs::create_directories(prefix / "bin" / "assets" / "cursors" / "drawing.png");
	CheckPath(Emulator::FindAssetFile(prefix / "bin", "cursors/drawing.png"),
	          (prefix / "bin/../share/kyty/assets/cursors/drawing.png").native(),
	          "a directory in the portable place");
}

} // namespace

int main() {
	TestOrder();
	TestRealTrees();
	std::printf("AssetLookupTests: all cases passed\n");
	return 0;
}
