// Feature: linux-gtk-modernization, Property 7: Search semantics
//
// Validates Requirements 7.13 and 7.14. Game lists of 0-24 entries and search texts from the
// libraryGenerators.h alphabet (letters whose case folding is not one-to-one, non-ASCII case pairs,
// White_Space code points, an ill-formed byte), often a case-varied piece of a title or title ID
// with whitespace around it. FilterIndices and MatchesSearch must give exactly the entries whose
// title name or title ID contains the trimmed search text by a unit-by-unit reference match, the
// full list for an empty or whitespace-only text, the same result for T and TrimWhitespace(T), and
// for T followed by any characters x a subset of the result for T.

#include "launcher/core/gameLibrary.h"
#include "launcher/core/text.h"
#include "support/libraryGenerators.h"
#include "support/property.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace {

namespace LibraryGen = KytyTests::LibraryGen;

std::vector<size_t> ReferenceFilter(const std::vector<LibraryGen::SearchEntry>& entries,
                                    const LibraryGen::Text&                     query) {
	const auto          needle = LibraryGen::ReferenceTrim(query.units);
	std::vector<size_t> indices;
	for (size_t i = 0; i < entries.size(); i++) {
		if (LibraryGen::ReferenceContains(entries[i].title.units, needle) ||
		    LibraryGen::ReferenceContains(entries[i].title_id.units, needle)) {
			indices.push_back(i);
		}
	}
	return indices;
}

} // namespace

KYTY_PROPERTY(7, "Search semantics") {
	return rc::check(name, []() {
		const auto                         models = LibraryGen::GenerateSearchEntries();
		std::vector<kyty::core::GameEntry> entries;
		for (size_t i = 0; i < models.size(); i++) {
			kyty::core::GameEntry entry;
			entry.path       = "/games/" + std::to_string(i);
			entry.title_name = models[i].title.bytes;
			entry.title_id   = models[i].title_id.bytes;
			entries.push_back(std::move(entry));
		}
		const auto query = LibraryGen::GenerateQuery(models);
		RC_LOG() << "query '" << query.bytes << "', " << entries.size() << " entries\n";
		for (const auto& entry: entries) {
			RC_LOG() << "  '" << entry.title_name << "' '" << entry.title_id << "'\n";
		}

		const auto result = kyty::core::FilterIndices(entries, query.bytes);
		RC_LOG() << "result size " << result.size() << "\n";

		// A subset of the list, in list order.
		for (size_t i = 0; i < result.size(); i++) {
			RC_ASSERT(result[i] < entries.size());
			if (i > 0) {
				// Not "i == 0 || ...": RC_ASSERT's expression capture would evaluate both sides.
				RC_ASSERT(result[i - 1] < result[i]);
			}
		}
		// Exactly the entries whose title name or title ID contains the trimmed text.
		RC_ASSERT(result == ReferenceFilter(models, query));
		for (size_t i = 0; i < entries.size(); i++) {
			const bool listed = std::binary_search(result.begin(), result.end(), i);
			RC_ASSERT(kyty::core::MatchesSearch(entries[i], query.bytes) == listed);
		}
		// An empty or whitespace-only text gives the full list.
		const bool blank = LibraryGen::ReferenceTrim(query.units).empty();
		RC_ASSERT(kyty::core::TrimWhitespace(query.bytes).empty() == blank);
		if (blank) {
			RC_ASSERT(result.size() == entries.size());
		}
		// T and TrimWhitespace(T) give the same result.
		const std::string trimmed(kyty::core::TrimWhitespace(query.bytes));
		RC_ASSERT(kyty::core::FilterIndices(entries, trimmed) == result);
		// T + x gives a subset of T's result.
		const auto suffix   = LibraryGen::GenerateText(1, 4);
		const auto extended = kyty::core::FilterIndices(entries, query.bytes + suffix.bytes);
		RC_LOG() << "suffix '" << suffix.bytes << "', extended result size " << extended.size()
		         << "\n";
		RC_ASSERT(std::includes(result.begin(), result.end(), extended.begin(), extended.end()));
		auto extended_query = query;
		for (const size_t unit: suffix.units) {
			extended_query.Append(unit);
		}
		RC_ASSERT(extended == ReferenceFilter(models, extended_query));

		RC_TAG(blank                             ? "blank query"
		       : result.empty()                  ? "no match"
		       : result.size() == entries.size() ? "all match"
		                                         : "some match");
	});
}
