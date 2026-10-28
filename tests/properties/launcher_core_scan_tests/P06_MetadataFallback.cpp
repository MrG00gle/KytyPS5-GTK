// Feature: linux-gtk-modernization, Property 6: Metadata fallback for unusable param.json
//
// Validates Requirement 7.9. Each case draws an entry name (scanGenerators.h EntryName), a kind,
// and an unusable param.json: larger than 1 MiB (a valid object padded with whitespace), invalid
// JSON (truncated, trailing garbage, single quotes, unquoted keys, invalid UTF-8, empty, random
// text), or valid JSON whose root is not an object. ParseParamJson must give the directory name or
// the .zar file name without its extension as title, empty title ID, game version, and system
// software version, and metadata_available == false. About one case in four also writes the
// file for real (or leaves it missing) in a directory or a .zar packed with Common::PackArchive and
// reads it through MakeHostMetadataReader, which must give the same fallback.

#include "common/archive.h"
#include "launcher/core/gameMetadata.h"
#include "support/property.h"
#include "support/scanGenerators.h"
#include "support/tempDir.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

namespace fs      = std::filesystem;
namespace ScanGen = KytyTests::ScanGen;

using kyty::core::GameEntry;
using kyty::core::GameMetadata;

void AssertFallback(const GameMetadata& metadata, const std::string& title) {
	RC_ASSERT(metadata.title_name == title);
	RC_ASSERT(metadata.title_id.empty());
	RC_ASSERT(metadata.game_version.empty());
	RC_ASSERT(metadata.firmware_version.empty());
	RC_ASSERT(!metadata.metadata_available);
}

} // namespace

KYTY_PROPERTY(6, "Metadata fallback for unusable param.json") {
	return rc::check(name, []() {
		const auto name_text = ScanGen::EntryName();
		const bool archive   = ScanGen::OneIn(2);
		const auto kind      = archive ? GameEntry::Kind::Archive : GameEntry::Kind::Directory;
		const auto file_name = archive ? name_text + ".zar" : name_text;
		const auto content   = ScanGen::UnusableParamJson();
		RC_LOG() << "entry '" << file_name << "', " << content.category << ", "
		         << content.bytes.size() << " bytes\n";

		const auto fallback = kyty::core::FallbackTitle(fs::path("/games") / file_name, kind);
		RC_ASSERT(fallback == name_text);
		AssertFallback(kyty::core::ParseParamJson(content.bytes, fallback), name_text);
		RC_TAG(content.category);

		if (ScanGen::OneIn(4)) {
			const KytyTests::TempDir temp("kyty_p06");
			const bool               missing = ScanGen::OneIn(4);
			const auto               source  = temp.MakeDirectories("source");
			temp.WriteFile("source/eboot.bin", "ELF");
			if (!missing) {
				temp.WriteFile("source/sce_sys/param.json", content.bytes);
			}
			fs::path entry = temp.Path() / "games" / file_name;
			if (archive) {
				temp.MakeDirectories("games");
				RC_ASSERT(
				    Common::PackArchive(source, entry, [](uint64_t, uint64_t) { return true; }));
			} else {
				std::error_code ec;
				fs::create_directories(entry.parent_path(), ec);
				fs::rename(source, entry, ec);
				RC_ASSERT(!ec);
			}
			const auto reader = kyty::core::MakeHostMetadataReader();
			AssertFallback(reader->Read(entry, kind), name_text);
			RC_TAG(missing   ? "real file: missing"
			       : archive ? "real file: archive"
			                 : "real file: directory");
		}
	});
}
