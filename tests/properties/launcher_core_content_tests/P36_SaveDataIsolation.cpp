// Feature: linux-gtk-modernization, Property 36: Save data removal is isolated
//
// Validates Requirement 11.13. Each case builds a real Data_Directory with a _SaveData tree of 2-5
// title IDs (plus look-alike names: the lower-case spelling, a longer ID, a file next to the
// directory), random files, nested and empty directories, and symbolic links from the removed
// title into other titles (to files and to directories) and to nowhere, and a _Trophies directory.
// It removes the save data of one title, spelled in any letter case with whitespace around it,
// through KytyTests::ContentGen::FailingRemover with 0-3 injected undeletable paths (files, links,
// or directories) or, in about one case in ten, for a title that has no save data. RemoveSaveData
// must call the remover only for paths inside _SaveData/<TITLE_ID>, each once and every directory
// after its contents; it must report exactly the injected paths and the directories above them
// (which are then not empty), leave exactly those paths, and leave every other path below the
// Data_Directory byte-identical (link targets included). A title ID that is not one path component
// (in about one case in ten) is refused, and nothing is removed.

#include "launcher/core/saveData.h"
#include "support/contentGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace ContentGen = KytyTests::ContentGen;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

const std::vector<std::string>& Titles() {
	static const std::vector<std::string> titles = {"PPSA01234", "PPSA05678", "CUSA00001",
	                                                "PPSA00010", "PPSA99999"};
	return titles;
}

std::string Spelling(const std::string& title) {
	static const std::vector<std::string> kSpaces = {"", " ", "\t", "\xC2\xA0", "\xE3\x80\x80"};
	std::string                           text    = title;
	for (auto& c: text) {
		if (c >= 'A' && c <= 'Z' && OneIn(2)) {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return kSpaces[Choose<std::size_t>(0, kSpaces.size())] + text +
	       kSpaces[Choose<std::size_t>(0, kSpaces.size())];
}

// A random subtree below directory (relative to data_dir); every created path is added to paths.
void Subtree(const KytyTests::TempDir& temp, const fs::path& directory, int depth,
             std::vector<fs::path>& paths) {
	static const std::vector<std::string> kNames = {
	    "a", "b", "sce_sys", "savedata0", "data.bin", "\xC3\xA9t\xC3\xA9", "x y", ".hidden"};
	std::set<std::string> used;
	const int             count = Choose(0, 5);
	for (int i = 0; i < count; i++) {
		const auto& name = kNames[Choose<std::size_t>(0, kNames.size())];
		if (!used.insert(name).second) {
			continue;
		}
		const auto path = directory / name;
		if (depth < 3 && OneIn(3)) {
			temp.MakeDirectories(fs::path("data") / path);
			paths.push_back(path);
			Subtree(temp, path, depth + 1, paths);
		} else {
			temp.WriteFile(fs::path("data") / path, std::string(Choose<std::size_t>(0, 64), 'k'));
			paths.push_back(path);
		}
	}
}

bool IsAtOrBelow(const fs::path& path, const fs::path& directory) {
	const auto relative = path.lexically_relative(directory);
	return !relative.empty() && *relative.begin() != "..";
}

} // namespace

KYTY_PROPERTY(36, "Save data removal is isolated") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p36");
		const auto               data = temp.Path() / "data";

		// The titles with save data; the removed title is one of them or, rarely, another one.
		std::vector<std::string> present;
		for (const auto& title: Titles()) {
			if (present.size() < 2 || OneIn(2)) {
				present.push_back(title);
			}
		}
		const bool  absent = OneIn(10);
		const auto& removed =
		    absent ? std::string("PPSA77777") : present[Choose<std::size_t>(0, present.size())];
		std::map<std::string, std::vector<fs::path>> paths; // relative to data
		for (const auto& title: present) {
			const fs::path directory = fs::path("_SaveData") / title;
			temp.MakeDirectories(fs::path("data") / directory);
			Subtree(temp, directory, 0, paths[title]);
		}
		temp.WriteFile("data/_SaveData/ppsa01234/lower.bin", "lower-case sibling");
		temp.WriteFile("data/_SaveData/" + removed + "0/x", "longer title");
		temp.WriteFile("data/_SaveData/" + removed + ".bak", "next to it");
		temp.WriteFile("data/_Trophies/" + removed + "/trophies_1000_0.json", "{}");

		// Links from the removed title to other titles and to nowhere.
		const fs::path title_dir = fs::path("_SaveData") / removed;
		if (!absent) {
			int links = Choose(0, 4);
			for (int i = 0; i < links; i++) {
				const auto& other = present[Choose<std::size_t>(0, present.size())];
				fs::path    target =
				    OneIn(4) ? fs::path("/nonexistent/kyty") : data / "_SaveData" / other;
				if (!paths[other].empty() && OneIn(2)) {
					target = data / paths[other][Choose<std::size_t>(0, paths[other].size())];
				}
				const auto link = title_dir / ("link" + std::to_string(i));
				temp.MakeSymlink(target, fs::path("data") / link);
				paths[removed].push_back(link);
			}
		}

		const bool                                 unsafe  = OneIn(10);
		static const std::vector<std::string_view> kUnsafe = {
		    "", "  ", ".", "..", " .. ", "../_Trophies", "PPSA01234/a"};
		const std::string title_argument =
		    unsafe ? std::string(kUnsafe[Choose<std::size_t>(0, kUnsafe.size())])
		           : Spelling(removed);

		// Injected failures below the removed title.
		ContentGen::FailingRemover remover;
		std::set<fs::path>         injected;
		const auto&                candidates = paths[removed];
		const int                  failures   = candidates.empty() ? 0 : Choose(0, 4);
		for (int i = 0; i < failures; i++) {
			const auto path = data / candidates[Choose<std::size_t>(0, candidates.size())];
			injected.insert(path);
			remover.FailOn(path, OneIn(2) ? std::errc::permission_denied
			                              : std::errc::device_or_resource_busy);
		}
		RC_LOG() << "remove '" << title_argument << "' of " << present.size() << " titles, "
		         << candidates.size() << " paths, " << injected.size() << " undeletable\n";

		const auto before  = ContentGen::SnapshotTree(data);
		const auto removal = kyty::core::RemoveSaveData(data, title_argument, remover);
		if (unsafe) {
			RC_ASSERT(!removal.has_value());
			RC_ASSERT(remover.Calls().empty());
			RC_ASSERT(ContentGen::SnapshotTree(data) == before);
			RC_TAG("unsafe title ID refused");
			return;
		}
		RC_ASSERT(removal.has_value());
		const auto directory = data / title_dir;
		RC_ASSERT(removal->directory == directory);

		// The remover saw only paths of the title directory, each once, contents first.
		const auto& calls = remover.Calls();
		for (std::size_t i = 0; i < calls.size(); i++) {
			RC_ASSERT(IsAtOrBelow(calls[i], directory));
			for (std::size_t j = i + 1; j < calls.size(); j++) {
				RC_ASSERT(calls[i] != calls[j]);
				RC_ASSERT(!IsAtOrBelow(calls[j], calls[i]));
			}
		}

		// Expected failures: the injected paths and every directory above them up to the title.
		std::set<fs::path> expected = injected;
		for (const auto& path: injected) {
			for (auto parent = path.parent_path(); IsAtOrBelow(parent, directory);
			     parent      = parent.parent_path()) {
				expected.insert(parent);
			}
		}
		std::set<fs::path> reported;
		for (const auto& failure: removal->failures) {
			RC_ASSERT(reported.insert(failure.path).second);
			if (injected.contains(failure.path)) {
				RC_ASSERT(failure.error == std::errc::permission_denied ||
				          failure.error == std::errc::device_or_resource_busy);
			} else {
				RC_ASSERT(failure.error == std::errc::directory_not_empty);
			}
		}
		RC_ASSERT(reported == expected);

		// Exactly the reported paths remain of the title; everything else is unchanged.
		std::map<std::string, std::string> expected_tree;
		for (const auto& [path, kind]: before) {
			const auto full = data / path;
			if (!IsAtOrBelow(full, directory) || expected.contains(full)) {
				expected_tree[path] = kind;
			}
		}
		auto after = ContentGen::SnapshotTree(data);
		if (absent) {
			RC_ASSERT(calls.empty());
		}
		RC_ASSERT(after == expected_tree);
		RC_TAG(absent ? "no save data" : injected.empty() ? "all removed" : "undeletable paths");
	});
}
