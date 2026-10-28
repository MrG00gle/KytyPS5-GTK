// clang-format off
// Feature: linux-gtk-modernization, Property 4: Game folder normalization keeps one entry per directory
// clang-format on
//
// Validates Requirement 7.1. Each case creates 1-5 real directories (some nested) under a fresh
// temporary directory, symbolic links to some of them and to their parents (absolute and relative
// targets), and two missing directories. It then builds a list of 1-12 folder strings, each a
// spelling of one of those directories (scanGenerators.h SpellDirectory: trailing separators, "."
// and ".." segments, relative to the working directory, through links) or the empty string, and
// runs NormalizeGameFolders on the host file system. The result must be the absolute form of the
// first spelling of each directory, in first-occurrence order: absolute, at most one per
// directory, none for the empty string.

#include "launcher/core/gameScanner.h"
#include "launcher/core/scanFileSystem.h"
#include "support/property.h"
#include "support/scanGenerators.h"
#include "support/tempDir.h"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs      = std::filesystem;
namespace ScanGen = KytyTests::ScanGen;

} // namespace

KYTY_PROPERTY(4, "Game folder normalization keeps one entry per directory") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p04");
		std::error_code          ec;
		const auto               base = fs::canonical(temp.Path(), ec);
		RC_ASSERT(!ec);

		// Real directories, each below the base or below an earlier one.
		std::vector<fs::path> directories;
		const auto            count = ScanGen::Choose(1, 6);
		for (int i = 0; i < count; i++) {
			const auto parent =
			    i == 0 || ScanGen::OneIn(2) ? base : directories[ScanGen::Choose(0, i)];
			directories.push_back(parent / ("d" + std::to_string(i)));
			fs::create_directories(directories.back(), ec);
			RC_ASSERT(!ec);
		}

		// Links named l<k> in the base: to a directory, or to a directory's parent.
		std::vector<std::vector<fs::path>> aliases(directories.size());
		std::vector<std::vector<fs::path>> parent_aliases(directories.size());
		const auto                         link_count = ScanGen::Choose(0, 4);
		for (int k = 0; k < link_count; k++) {
			const auto index  = ScanGen::Choose<size_t>(0, directories.size());
			const auto link   = base / ("l" + std::to_string(k));
			const bool parent = ScanGen::OneIn(3);
			const auto target = parent ? directories[index].parent_path() : directories[index];
			fs::create_symlink(ScanGen::OneIn(2) ? target : target.lexically_relative(base), link,
			                   ec);
			RC_ASSERT(!ec);
			if (parent) {
				for (size_t j = 0; j < directories.size(); j++) {
					if (directories[j].parent_path() == target) {
						parent_aliases[j].push_back(link);
					}
				}
			} else {
				aliases[index].push_back(link);
			}
		}

		// Identities: the real directories, then two missing ones (spelled without links).
		std::vector<fs::path> identities = directories;
		identities.push_back(base / "missing0");
		identities.push_back(base / "missing0" / "missing1");
		const std::vector<fs::path> no_aliases;

		std::vector<std::string> input;
		std::vector<int>         identity_of;
		const auto               spellings = ScanGen::Choose(1, 13);
		for (int i = 0; i < spellings; i++) {
			if (ScanGen::OneIn(10)) {
				input.emplace_back();
				identity_of.push_back(-1);
				continue;
			}
			const auto identity = ScanGen::Choose<size_t>(0, identities.size());
			const bool real     = identity < directories.size();
			input.push_back(ScanGen::SpellDirectory(identities[identity],
			                                        real ? aliases[identity] : no_aliases,
			                                        real ? parent_aliases[identity] : no_aliases));
			identity_of.push_back(static_cast<int>(identity));
		}
		for (const auto& text: input) {
			RC_LOG() << "'" << text << "'\n";
		}

		std::vector<fs::path> expected;
		std::set<int>         seen;
		for (size_t i = 0; i < input.size(); i++) {
			if (identity_of[i] >= 0 && seen.insert(identity_of[i]).second) {
				expected.push_back(fs::absolute(input[i]));
			}
		}

		const auto host   = kyty::core::MakeHostScanFileSystem();
		const auto result = kyty::core::NormalizeGameFolders(input, *host);
		RC_ASSERT(result == expected);

		std::set<std::string> canonical_directories;
		for (const auto& folder: result) {
			RC_ASSERT(folder.is_absolute());
			const auto canonical = fs::canonical(folder, ec);
			if (!ec) {
				RC_ASSERT(canonical_directories.insert(canonical.string()).second);
			}
		}
		RC_TAG(seen.size() < input.size() ? "with aliases or duplicates" : "all distinct");
	});
}
