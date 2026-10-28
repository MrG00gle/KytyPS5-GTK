// Feature: linux-gtk-modernization, Property 2: Scanning an unchanged tree is idempotent
//
// Validates Requirement 7.6. Each case generates a tree model and folder list (scanGenerators.h),
// scans it twice on the same InMemoryScanFileSystem and metadata reader, and once more on a fresh
// file system built from the same model; all three results must be equal, entries and warnings.

#include "launcher/core/cancelToken.h"
#include "launcher/core/gameScanner.h"
#include "support/fakeScanFileSystem.h"
#include "support/property.h"
#include "support/scanGenerators.h"

#include <cstdint>

namespace {

namespace ScanGen = KytyTests::ScanGen;

} // namespace

KYTY_PROPERTY(2, "Scanning an unchanged tree is idempotent") {
	return rc::check(name, []() {
		const auto model = ScanGen::GenerateTreeModel();
		RC_LOG() << ScanGen::Describe(model);
		const auto seed = *rc::gen::arbitrary<uint64_t>();

		KytyTests::InMemoryScanFileSystem files;
		KytyTests::InMemoryMetadataReader metadata;
		ScanGen::BuildFileSystem(model, files, metadata);
		files.SetListingOrder(seed);
		const kyty::core::CancelToken cancel;
		const auto first  = kyty::core::ScanGameFolders(model.folders, files, metadata, cancel);
		const auto second = kyty::core::ScanGameFolders(model.folders, files, metadata, cancel);
		RC_ASSERT(!first.cancelled);
		RC_ASSERT(first.entries == second.entries);
		RC_ASSERT(first.warnings == second.warnings);

		KytyTests::InMemoryScanFileSystem fresh_files;
		KytyTests::InMemoryMetadataReader fresh_metadata;
		ScanGen::BuildFileSystem(model, fresh_files, fresh_metadata);
		fresh_files.SetListingOrder(seed);
		const auto third =
		    kyty::core::ScanGameFolders(model.folders, fresh_files, fresh_metadata, cancel);
		RC_ASSERT(third == first);
		RC_TAG(first.entries.empty() ? "no entries" : "entries");
	});
}
