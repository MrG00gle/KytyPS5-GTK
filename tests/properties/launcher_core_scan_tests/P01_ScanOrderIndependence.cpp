// Feature: linux-gtk-modernization, Property 1: Scan result is independent of enumeration order
//
// Validates Requirement 7.5. Each case generates a tree model (scanGenerators.h) and scans its
// folders twice on InMemoryScanFileSystem, with listings in two different orders (two seeds; seed
// 0 is name order). The entries (path, kind, metadata, legacy path) and the warnings must be equal.

#include "launcher/core/cancelToken.h"
#include "launcher/core/gameScanner.h"
#include "support/fakeScanFileSystem.h"
#include "support/property.h"
#include "support/scanGenerators.h"

#include <cstdint>

namespace {

namespace ScanGen = KytyTests::ScanGen;

kyty::core::ScanResult ScanWithOrder(const ScanGen::TreeModel& model, uint64_t seed) {
	KytyTests::InMemoryScanFileSystem files;
	KytyTests::InMemoryMetadataReader metadata;
	ScanGen::BuildFileSystem(model, files, metadata);
	files.SetListingOrder(seed);
	const kyty::core::CancelToken cancel;
	return kyty::core::ScanGameFolders(model.folders, files, metadata, cancel);
}

} // namespace

KYTY_PROPERTY(1, "Scan result is independent of enumeration order") {
	return rc::check(name, []() {
		const auto model = ScanGen::GenerateTreeModel();
		RC_LOG() << ScanGen::Describe(model);
		const auto first  = *rc::gen::arbitrary<uint64_t>();
		const auto second = *rc::gen::suchThat(rc::gen::arbitrary<uint64_t>(),
		                                       [first](uint64_t seed) { return seed != first; });

		const auto a = ScanWithOrder(model, first);
		const auto b = ScanWithOrder(model, second);
		RC_ASSERT(!a.cancelled);
		RC_ASSERT(a.entries == b.entries);
		RC_ASSERT(a.warnings == b.warnings);
		RC_TAG(a.entries.empty()      ? "no entries"
		       : a.entries.size() < 4 ? "1-3 entries"
		                              : "4+ entries");
		RC_TAG(a.warnings.empty() ? "no warnings" : "warnings");
	});
}
