// Example tests of param.json parsing and the archive-aware content reads
// (src/launcher/core/gameMetadata.h, gameContent.h; Requirements 7.8 and 7.9). Real temporary
// directories and .zar files packed with Common::PackArchive; no test changes permissions.

#include "common/archive.h"
#include "launcher/core/gameContent.h"
#include "launcher/core/gameMetadata.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <system_error>

namespace {

namespace fs = std::filesystem;

using kyty::core::EntryFileExists;
using kyty::core::FallbackTitle;
using kyty::core::GameEntry;
using kyty::core::GameMetadata;
using kyty::core::kMaxImageBytes;
using kyty::core::kMaxParamJsonBytes;
using kyty::core::MakeHostMetadataReader;
using kyty::core::ParseParamJson;
using kyty::core::ReadEntryFile;
using kyty::core::ReadIcon0;
using kyty::core::ReadParamJson;
using kyty::core::ReadPic0;
using kyty::core::ReadSnd0;
using kyty::core::ResolveEntryFile;
using KytyTests::TempDir;

constexpr auto kDirectory = GameEntry::Kind::Directory;
constexpr auto kArchive   = GameEntry::Kind::Archive;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GameMetadataTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

GameMetadata Fallback(const std::string& title) {
	GameMetadata metadata;
	metadata.title_name = title;
	return metadata;
}

void CheckFallback(const GameMetadata& metadata, const std::string& title,
                   const std::string& what) {
	Check(metadata == Fallback(title), what + ": fallback metadata, title '" + metadata.title_name +
	                                       "', available " +
	                                       std::to_string(metadata.metadata_available));
}

fs::path Pack(const TempDir& dir, const fs::path& source, const std::string& archive_name) {
	const auto archive = dir.Path() / archive_name;
	Check(Common::PackArchive(source, archive, [](uint64_t, uint64_t) { return true; }),
	      "pack " + source.string());
	return archive;
}

// A JSON object padded with spaces to exactly `size` bytes.
std::string PaddedObject(size_t size) {
	std::string text = R"({"titleId":"PPSA00001","appVersion":"01.000"})";
	text.append(size - text.size(), ' ');
	return text;
}

void TestLocalizedTitle() {
	const auto with_default = ParseParamJson(R"({"localizedParameters":{
		"defaultLanguage":"ja-JP",
		"en-US":{"titleName":"English"},
		"ja-JP":{"titleName":"  Japanese\t"}}})",
	                                         "dir");
	Check(with_default.title_name == "Japanese", "the defaultLanguage title, trimmed");
	Check(with_default.metadata_available, "an object root makes the metadata available");

	const auto default_empty = ParseParamJson(R"({"localizedParameters":{
		"defaultLanguage":"ja-JP", "ja-JP":{"titleName":"   "},
		"de-DE":{"titleName":"Deutsch"}, "en-US":{"titleName":"English"}}})",
	                                          "dir");
	Check(default_empty.title_name == "English", "en-US when the default title is blank");

	const auto default_missing = ParseParamJson(R"({"localizedParameters":{
		"defaultLanguage":"fr-FR", "en-US":{"titleName":"English"}}})",
	                                            "dir");
	Check(default_missing.title_name == "English", "en-US when the default language is missing");

	const auto first = ParseParamJson(R"({"localizedParameters":{
		"defaultLanguage":"", "zz-ZZ":{"titleName":"Last"}, "de-DE":{"titleName":"Deutsch"},
		"aa-AA":{"titleName":""}, "en-US":{"titleId":"no title"}}})",
	                                  "dir");
	Check(first.title_name == "Deutsch", "else the first non-empty title in key order");

	const auto wrong_types = ParseParamJson(R"({"localizedParameters":{
		"defaultLanguage":7, "en-US":{"titleName":42}, "de-DE":"Deutsch", "fr-FR":[]}})",
	                                        "dir");
	Check(wrong_types.title_name == "dir" && wrong_types.metadata_available,
	      "non-string titles are ignored: the fallback title, metadata available");

	Check(ParseParamJson(R"({"localizedParameters":[]})", "dir").title_name == "dir",
	      "localizedParameters that is not an object");
	Check(ParseParamJson(R"({"localizedParameters":{}})", "dir").title_name == "dir",
	      "empty localizedParameters");
	const auto none = ParseParamJson("{}", "Folder Name");
	Check(none.title_name == "Folder Name" && none.metadata_available && none.title_id.empty() &&
	          none.game_version.empty() && none.firmware_version.empty(),
	      "an empty object: the fallback title, metadata available, other fields empty");
	Check(ParseParamJson(R"({"localizedParameters":{"en-US":{"titleName":"\u00c9t\u00e9"}}})", "x")
	              .title_name == "\xC3\x89t\xC3\xA9",
	      "escaped non-ASCII titles are UTF-8");
}

void TestTitleId() {
	Check(ParseParamJson(R"({"titleId":" PPSA01234 ","contentId":"UP0000-PPSA99999_00-X"})", "d")
	              .title_id == "PPSA01234",
	      "titleId wins, trimmed");
	const std::string content_id = "UP0000-PPSA05678_00-0123456789ABCDEF";
	Check(content_id.size() == 36, "the sample contentId has 36 bytes");
	Check(ParseParamJson(R"({"contentId":")" + content_id + R"("})", "d").title_id == "PPSA05678",
	      "contentId[7..16) when titleId is missing");
	Check(ParseParamJson(R"({"titleId":"","contentId":")" + content_id + R"("})", "d").title_id ==
	          "PPSA05678",
	      "contentId[7..16) when titleId is empty");
	Check(ParseParamJson(R"({"titleId":5,"contentId":")" + content_id + R"("})", "d").title_id ==
	          "PPSA05678",
	      "contentId[7..16) when titleId is not a string");
	Check(ParseParamJson(R"({"contentId":"  )" + content_id + R"(  "})", "d").title_id ==
	          "PPSA05678",
	      "contentId is trimmed before the length check");
	Check(ParseParamJson(R"({"contentId":"UP0000-PPSA05678_00"})", "d").title_id.empty(),
	      "a short contentId gives no title ID");
	Check(ParseParamJson(R"({"contentId":")" + content_id + R"(X"})", "d").title_id.empty(),
	      "a 37-byte contentId gives no title ID");
	std::string no_dash = content_id;
	no_dash[6]          = '_';
	Check(ParseParamJson(R"({"contentId":")" + no_dash + R"("})", "d").title_id.empty(),
	      "contentId without '-' at index 6");
	std::string no_underscore = content_id;
	no_underscore[16]         = '-';
	Check(ParseParamJson(R"({"contentId":")" + no_underscore + R"("})", "d").title_id.empty(),
	      "contentId without '_' at index 16");
	Check(ParseParamJson(R"({"contentId":12})", "d").title_id.empty(),
	      "a non-string contentId gives no title ID");
}

void TestVersions() {
	Check(ParseParamJson(R"({"appVersion":" 01.050 ","contentVersion":"02.000"})", "d")
	              .game_version == "01.050",
	      "appVersion wins, trimmed");
	Check(ParseParamJson(R"({"contentVersion":"02.000"})", "d").game_version == "02.000",
	      "contentVersion when appVersion is missing");
	Check(ParseParamJson(R"({"appVersion":"","contentVersion":"02.000"})", "d").game_version ==
	          "02.000",
	      "contentVersion when appVersion is empty");
	Check(ParseParamJson(R"({"appVersion":1.5})", "d").game_version.empty(),
	      "a non-string version is ignored");

	Check(ParseParamJson(R"({"requiredSystemSoftwareVersion":"0x0150000000000000"})", "d")
	              .firmware_version == "1.50",
	      "firmware 1.50");
	Check(ParseParamJson(R"({"requiredSystemSoftwareVersion":" 0x1023450000ABCDEF "})", "d")
	              .firmware_version == "10.23.45",
	      "firmware with a patch, trimmed");
	Check(ParseParamJson(R"({"requiredSystemSoftwareVersion":"0x01500000000000"})", "d")
	          .firmware_version.empty(),
	      "a short firmware value is empty");
	Check(ParseParamJson(R"({"requiredSystemSoftwareVersion":"0X0150000000000000"})", "d")
	              .firmware_version == "1.50",
	      "an upper-case 0X prefix is accepted, as in Qt (phase 5 D29)");
	Check(ParseParamJson(R"({"requiredSystemSoftwareVersion":22020096})", "d")
	          .firmware_version.empty(),
	      "a numeric firmware value is empty");
}

void TestUnusable() {
	CheckFallback(ParseParamJson("", "Game"), "Game", "empty bytes");
	CheckFallback(ParseParamJson("{", "Game"), "Game", "invalid JSON");
	CheckFallback(ParseParamJson(R"({"titleId":"PPSA01234",})", "Game"), "Game",
	              "a trailing comma");
	CheckFallback(ParseParamJson(R"({"titleId":"PPSA01234"} x)", "Game"), "Game",
	              "trailing garbage");
	CheckFallback(ParseParamJson("{\"titleId\":\"\xFF\"}", "Game"), "Game", "invalid UTF-8");
	CheckFallback(ParseParamJson(R"([{"titleId":"PPSA01234"}])", "Game"), "Game", "an array root");
	CheckFallback(ParseParamJson(R"("PPSA01234")", "Game"), "Game", "a string root");
	CheckFallback(ParseParamJson("null", "Game"), "Game", "a null root");
	CheckFallback(ParseParamJson("12", "Game"), "Game", "a number root");
	CheckFallback(ParseParamJson("/* c */ {}", "Game"), "Game", "comments are not JSON");

	const auto at_limit = PaddedObject(kMaxParamJsonBytes);
	Check(at_limit.size() == 1024 * 1024, "the padded object is exactly 1 MiB");
	const auto accepted = ParseParamJson(at_limit, "Game");
	Check(accepted.metadata_available && accepted.title_id == "PPSA00001",
	      "exactly 1 MiB is parsed");
	CheckFallback(ParseParamJson(PaddedObject(kMaxParamJsonBytes + 1), "Game"), "Game",
	              "one byte over 1 MiB");

	// Deep nesting is parsed without recursion and is not an object root.
	CheckFallback(ParseParamJson(std::string(100000, '[') + std::string(100000, ']'), "Game"),
	              "Game", "deeply nested arrays");
}

void TestFallbackTitle() {
	Check(FallbackTitle("/games/My Game", kDirectory) == "My Game", "a directory name");
	Check(FallbackTitle("/games/My Game/", kDirectory) == "My Game", "a trailing separator");
	Check(FallbackTitle("/games/My.Game.zar", kDirectory) == "My.Game.zar",
	      "a directory keeps its whole name");
	Check(FallbackTitle("/games/Game.zar", kArchive) == "Game", "an archive without .zar");
	Check(FallbackTitle("/games/a.b.zar", kArchive) == "a.b", "only the last extension goes");
	Check(FallbackTitle("/games/.zar", kArchive) == ".zar", "a file named .zar keeps its name");
	Check(FallbackTitle("/", kDirectory) == "/", "the root directory");
}

void TestHostDirectory() {
	const TempDir dir("kyty_metadata");
	const auto    reader = MakeHostMetadataReader();

	const auto game = dir.MakeDirectories("Some Game");
	dir.WriteFile("Some Game/sce_sys/param.json",
	              R"({"localizedParameters":{"defaultLanguage":"en-US",
	                  "en-US":{"titleName":"Real Title"}},
	                  "titleId":"PPSA01234","appVersion":"01.000",
	                  "requiredSystemSoftwareVersion":"0x0412000000000000"})");
	const auto metadata = reader->Read(game, kDirectory);
	Check(metadata.title_name == "Real Title" && metadata.title_id == "PPSA01234" &&
	          metadata.game_version == "01.000" && metadata.firmware_version == "4.12" &&
	          metadata.metadata_available,
	      "a directory's param.json is read");

	const auto missing = dir.MakeDirectories("No Param");
	CheckFallback(reader->Read(missing, kDirectory), "No Param", "a missing param.json");
	CheckFallback(reader->Read(dir.Path() / "Gone", kDirectory), "Gone", "a missing directory");

	const auto exact = dir.MakeDirectories("Exact");
	dir.WriteFile("Exact/sce_sys/param.json", PaddedObject(kMaxParamJsonBytes));
	Check(reader->Read(exact, kDirectory).metadata_available, "a 1 MiB param.json file is read");
	const auto over = dir.MakeDirectories("Over");
	dir.WriteFile("Over/sce_sys/param.json", PaddedObject(kMaxParamJsonBytes + 1));
	CheckFallback(reader->Read(over, kDirectory), "Over", "a param.json one byte over 1 MiB");

	const auto array = dir.MakeDirectories("Array");
	dir.WriteFile("Array/sce_sys/param.json", "[1, 2]");
	CheckFallback(reader->Read(array, kDirectory), "Array", "an array param.json");

	const auto as_directory = dir.MakeDirectories("AsDirectory/sce_sys/param.json");
	CheckFallback(reader->Read(dir.Path() / "AsDirectory", kDirectory), "AsDirectory",
	              "param.json that is a directory");
	(void)as_directory;

	// A symbolic link to a file is followed, as the Qt launcher's reads did.
	dir.WriteFile("Shared/param.json", R"({"titleId":"PPSA00002"})");
	dir.MakeSymlink("../../Shared/param.json", "Linked/sce_sys/param.json");
	Check(reader->Read(dir.Path() / "Linked", kDirectory).title_id == "PPSA00002",
	      "a param.json symbolic link is followed");

	// A FIFO is never opened (opening it would block).
	const auto fifo_game = dir.MakeDirectories("Fifo/sce_sys");
	Check(::mkfifo((fifo_game / "param.json").c_str(), 0644) == 0, "mkfifo");
	CheckFallback(reader->Read(dir.Path() / "Fifo", kDirectory), "Fifo", "a FIFO param.json");
	Check(!EntryFileExists(dir.Path() / "Fifo", kDirectory, "sce_sys/param.json"),
	      "a FIFO is not a regular file");
}

void TestHostArchive() {
	const TempDir dir("kyty_metadata_zar");
	const auto    reader = MakeHostMetadataReader();

	dir.WriteFile("src/eboot.bin", "ELF");
	dir.WriteFile("src/sce_sys/param.json",
	              R"({"titleId":"PPSA04321","contentVersion":"02.010",
	                  "localizedParameters":{"en-US":{"titleName":"Archived"}}})");
	const auto archive  = Pack(dir, dir.Path() / "src", "Packed Game.zar");
	const auto metadata = reader->Read(archive, kArchive);
	Check(metadata.title_name == "Archived" && metadata.title_id == "PPSA04321" &&
	          metadata.game_version == "02.010" && metadata.firmware_version.empty() &&
	          metadata.metadata_available,
	      "an archive's param.json is read");

	dir.WriteFile("bare/eboot.bin", "ELF");
	const auto bare = Pack(dir, dir.Path() / "bare", "Bare.v2.zar");
	CheckFallback(reader->Read(bare, kArchive), "Bare.v2", "an archive without param.json");

	dir.WriteFile("over/sce_sys/param.json", PaddedObject(kMaxParamJsonBytes + 1));
	const auto over = Pack(dir, dir.Path() / "over", "Over.zar");
	CheckFallback(reader->Read(over, kArchive), "Over", "an archive param.json over 1 MiB");
	dir.WriteFile("exact/sce_sys/param.json", PaddedObject(kMaxParamJsonBytes));
	const auto exact = Pack(dir, dir.Path() / "exact", "Exact.zar");
	Check(reader->Read(exact, kArchive).metadata_available, "an archive param.json of 1 MiB");

	dir.WriteFile("invalid/sce_sys/param.json", "{\"titleId\":");
	const auto invalid = Pack(dir, dir.Path() / "invalid", "Invalid.zar");
	CheckFallback(reader->Read(invalid, kArchive), "Invalid", "invalid JSON in an archive");

	const auto garbage = dir.WriteFile("Garbage.zar", std::string(4096, 'z'));
	CheckFallback(reader->Read(garbage, kArchive), "Garbage", "a file that is not a ZArchive");
	CheckFallback(reader->Read(dir.Path() / "Missing.zar", kArchive), "Missing",
	              "a missing archive");
	const auto not_zar = dir.WriteFile("plain.bin", "x");
	CheckFallback(reader->Read(not_zar, kArchive), "plain", "an archive path without .zar");

	dir.WriteFile("dirparam/sce_sys/param.json/inner.txt", "x");
	const auto dir_param = Pack(dir, dir.Path() / "dirparam", "DirParam.zar");
	CheckFallback(reader->Read(dir_param, kArchive), "DirParam",
	              "param.json that is a directory in the archive");
}

void TestAccessors() {
	const TempDir dir("kyty_content");
	const auto    game = dir.MakeDirectories("game");
	dir.WriteFile("game/sce_sys/icon0.png", "icon bytes");
	dir.WriteFile("game/sce_sys/pic0.png", "picture bytes");
	dir.WriteFile("game/sce_sys/snd0.at9", "sound bytes");
	dir.WriteFile("game/sce_sys/param.json", "{}");
	dir.WriteFile("game/sce_sys/empty.bin", "");

	Check(ReadIcon0(game, kDirectory) == std::optional<std::string>("icon bytes"), "icon0.png");
	Check(ReadPic0(game, kDirectory) == std::optional<std::string>("picture bytes"), "pic0.png");
	Check(ReadSnd0(game, kDirectory) == std::optional<std::string>("sound bytes"), "snd0.at9");
	Check(ReadParamJson(game, kDirectory) == std::optional<std::string>("{}"), "param.json");
	Check(ReadEntryFile(game, kDirectory, "sce_sys/empty.bin", 0) == std::optional<std::string>(""),
	      "an empty file at limit 0");
	Check(!ReadEntryFile(game, kDirectory, "sce_sys/icon0.png", 9).has_value(),
	      "a file over the limit");
	Check(ReadEntryFile(game, kDirectory, "sce_sys/icon0.png", 10).has_value(),
	      "a file at the limit");
	Check(!ReadEntryFile(game, kDirectory, "sce_sys", 100).has_value(), "a directory");
	Check(EntryFileExists(game, kDirectory, "sce_sys/icon0.png"), "icon0.png exists");
	Check(!EntryFileExists(game, kDirectory, "sce_sys"), "a directory is not a file");
	Check(!EntryFileExists(game, kDirectory, "sce_sys/missing.png"), "a missing file");
	Check(ResolveEntryFile(game, kDirectory, "sce_sys/icon0.png") == game / "sce_sys/icon0.png",
	      "a directory entry resolves below it");

	const auto bare = dir.MakeDirectories("bare");
	Check(!ReadIcon0(bare, kDirectory) && !ReadPic0(bare, kDirectory) &&
	          !ReadSnd0(bare, kDirectory) && !ReadParamJson(bare, kDirectory),
	      "missing files are nullopt");

	// The 32 MiB image limit, with sparse files.
	const auto large = dir.MakeDirectories("large/sce_sys");
	dir.WriteFile("large/sce_sys/icon0.png", "");
	dir.WriteFile("large/sce_sys/pic0.png", "");
	std::error_code ec;
	fs::resize_file(large / "icon0.png", kMaxImageBytes, ec);
	Check(!ec, "resize icon0.png");
	fs::resize_file(large / "pic0.png", kMaxImageBytes + 1, ec);
	Check(!ec, "resize pic0.png");
	const auto icon = ReadIcon0(dir.Path() / "large", kDirectory);
	Check(icon.has_value() && icon->size() == kMaxImageBytes, "a 32 MiB icon0.png is read");
	Check(!ReadPic0(dir.Path() / "large", kDirectory).has_value(),
	      "a pic0.png one byte over 32 MiB is not");

	const auto archive = Pack(dir, game, "game.zar");
	Check(ResolveEntryFile(archive, kArchive, "sce_sys/icon0.png") ==
	          Common::MakeArchivePath(archive, "sce_sys/icon0.png"),
	      "an archive entry resolves to an archive path");
	Check(ReadIcon0(archive, kArchive) == std::optional<std::string>("icon bytes"),
	      "icon0.png from an archive");
	Check(ReadPic0(archive, kArchive) == std::optional<std::string>("picture bytes"),
	      "pic0.png from an archive");
	Check(ReadSnd0(archive, kArchive) == std::optional<std::string>("sound bytes"),
	      "snd0.at9 from an archive");
	Check(ReadEntryFile(archive, kArchive, "sce_sys/empty.bin", 0) ==
	          std::optional<std::string>(""),
	      "an empty archive member at limit 0");
	Check(!ReadEntryFile(archive, kArchive, "sce_sys/icon0.png", 9).has_value(),
	      "an archive member over the limit");
	Check(!ReadEntryFile(archive, kArchive, "sce_sys", 100).has_value(),
	      "an archive directory is not a file");
	Check(EntryFileExists(archive, kArchive, "sce_sys/snd0.at9"), "snd0.at9 exists in the archive");
	Check(!EntryFileExists(archive, kArchive, "sce_sys"), "an archive directory");
	Check(!EntryFileExists(archive, kArchive, "sce_sys/missing.png"), "a missing member");
	Check(!ReadIcon0(dir.Path() / "missing.zar", kArchive).has_value(), "a missing archive");
	Check(!EntryFileExists(dir.Path() / "missing.zar", kArchive, "sce_sys/icon0.png"),
	      "nothing exists in a missing archive");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GameMetadataTests");
	TestLocalizedTitle();
	TestTitleId();
	TestVersions();
	TestUnusable();
	TestFallbackTitle();
	TestHostDirectory();
	TestHostArchive();
	TestAccessors();
	std::printf("GameMetadataTests: all tests passed\n");
	return 0;
}
