// Feature: linux-gtk-modernization, Property 9: Compatibility lookup
//
// Validates Requirement 8.5. Databases of 0-20 entries from compatibilityGenerators.h (keys with
// random case and White_Space, every Qt status alias, unrecognized and non-string statuses,
// members that are not objects, blank keys) are parsed; for every entry, Lookup with the title ID
// in random case and with whitespace around it must return the entry's status and comment, and
// Lookup must give Unknown for an empty or whitespace-only ID, for IDs without an entry (an
// entry's ID with one character more, one less, or one changed, and fresh IDs), and for an entry
// whose status is not recognized.

#include "launcher/core/compatibility.h"
#include "launcher/core/gameSettingsFile.h"
#include "support/compatibilityGenerators.h"
#include "support/property.h"

#include <set>
#include <string>
#include <vector>

namespace {

namespace CompatGen = KytyTests::CompatGen;

using kyty::core::CompatibilityEntry;
using kyty::core::CompatibilityStatus;

} // namespace

KYTY_PROPERTY(9, "Compatibility lookup") {
	return rc::check(name, []() {
		const auto model = CompatGen::GenerateDatabase();
		const auto json  = CompatGen::ToJson(model);
		RC_LOG() << json << "\n";
		const auto parsed = kyty::core::ParseCompatibility(json);
		RC_ASSERT(parsed.has_value());
		const auto& database = *parsed;
		RC_ASSERT(database.entries.size() == model.entries.size());

		std::set<std::string> ids;
		bool                  unknown_status = false;
		for (const auto& entry: model.entries) {
			ids.insert(entry.id);
			unknown_status =
			    unknown_status || entry.expected.status == CompatibilityStatus::Unknown;
			// The entry's key in the database is the trimmed, upper-cased key.
			RC_ASSERT(kyty::core::NormalizeTitleId(entry.key) == entry.id);
			RC_ASSERT(database.entries.count(entry.id) == 1);
			for (int i = 0; i < 3; i++) {
				const auto query = CompatGen::VaryTitleId(entry.id);
				RC_LOG() << "lookup '" << query << "'\n";
				RC_ASSERT(kyty::core::Lookup(database, query) == entry.expected);
			}
		}

		// Empty and whitespace-only IDs.
		for (int i = 0; i < 3; i++) {
			RC_ASSERT(kyty::core::Lookup(database, CompatGen::GenerateWhitespace()) ==
			          CompatibilityEntry {});
		}
		// IDs without an entry: near misses of existing IDs and fresh IDs.
		std::vector<std::string> misses;
		for (const auto& entry: model.entries) {
			misses.push_back(entry.id + "0");
			misses.push_back(entry.id.substr(0, entry.id.size() - 1));
			auto changed = entry.id;
			changed[0]   = changed[0] == 'Z' ? 'Y' : 'Z';
			misses.push_back(changed);
		}
		for (int i = 0; i < 5; i++) {
			misses.push_back(CompatGen::GenerateTitleId());
		}
		for (const auto& miss: misses) {
			const auto query  = CompatGen::VaryTitleId(miss);
			const auto result = kyty::core::Lookup(database, query);
			if (ids.count(miss) == 0) {
				RC_ASSERT(result == CompatibilityEntry {});
				RC_ASSERT(result.status == CompatibilityStatus::Unknown);
			} else {
				RC_ASSERT(result == model.ExpectedEntry(miss));
			}
		}

		RC_TAG(model.entries.empty() ? "empty database"
		       : unknown_status      ? "with unrecognized statuses"
		                             : "only recognized statuses");
	});
}
