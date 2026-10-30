// Feature: linux-gtk-modernization, Property 32: Remote cheat import
//
// Validates Requirement 11.5. Each case draws a PPSA title ID, spelled in any letter case with
// whitespace around it, a game version, and an executable name, and a remote cheat document that
// ValidateRemoteImport accepts for them: "id" in any letter case, 1-5 mods with 1-3 valid memory
// writes each (offsets with or without 0x, byte strings in either case, "absolute": false and
// "expected" equal to "off" now and then), "enabled" absent, true, or false, and unknown members at
// the top level and in mods. The real Data_Directory holds other titles' cheat files and other
// data, and the game's own cheat file is absent, valid, or invalid. ImportRemoteCheats must refuse
// to replace an existing file without the bytes LoadLocalCheats read (NeedsConfirmation, nothing
// changed), then write <data>/_Patches/<UPPERCASE TITLE_ID>.json, whose cheats are the remote
// ones, all disabled, with every other member preserved (the document equals the remote one with
// "enabled": false in each mod), and leave every other path byte-identical.

#include "launcher/core/cheats.h"
#include "support/contentGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;
namespace fs         = std::filesystem;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& Pick(const std::vector<T>& values) {
	return values[Choose<std::size_t>(0, values.size())];
}

std::string Hex(std::size_t digits) {
	static constexpr std::string_view kHex = "0123456789abcdefABCDEF";
	std::string                       text;
	for (std::size_t i = 0; i < digits; i++) {
		text += kHex[Choose<std::size_t>(0, kHex.size())];
	}
	return text;
}

std::string RandomCase(std::string text) {
	for (auto& c: text) {
		if (c >= 'a' && c <= 'z' && OneIn(2)) {
			c = static_cast<char>(c - 'a' + 'A');
		} else if (c >= 'A' && c <= 'Z' && OneIn(2)) {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return text;
}

std::string MemoryWrite() {
	const auto                 bytes = 2 * Choose<std::size_t>(1, 5);
	const auto                 off   = Hex(bytes);
	ContentGen::JsonObjectSpec write = {
	    {"offset", ContentGen::JsonString(Pick(std::vector<std::string> {"", "0x", "0X"}) +
	                                      Hex(Choose<std::size_t>(1, 17)))},
	    {"on", ContentGen::JsonString(Hex(bytes))},
	    {"off", ContentGen::JsonString(off)}};
	if (OneIn(4)) {
		write.push_back({"absolute", "false"});
	}
	if (OneIn(4)) {
		write.push_back({"expected", ContentGen::JsonString(RandomCase(off))});
	}
	return ContentGen::JsonObjectText(write);
}

std::string UnknownValue() {
	return Pick(std::vector<std::string> {"null", "7", "\"text\"", "[1,{\"a\":true}]",
	                                      "{\"nested\":{\"k\":\"\xC3\xA9\"}}", "-2.5"});
}

} // namespace

KYTY_PROPERTY(32, "Remote cheat import") {
	return rc::check(name, []() {
		std::string title = "PPSA";
		for (int i = 0; i < 5; i++) {
			title += static_cast<char>('0' + Choose(0, 10));
		}
		static const std::vector<std::string> kSpaces = {"", " ", "\t", "\xC2\xA0", "\xE3\x80\x80"};
		const auto spelling = Pick(kSpaces) + RandomCase(title) + Pick(kSpaces);
		const auto version =
		    std::to_string(Choose(1, 100)) + "." + std::to_string(Choose(100, 1000));
		const std::string process = Pick(std::vector<std::string> {"eboot.bin", "game.elf"});

		ContentGen::CheatDocumentSpec spec;
		spec.before = {{"id", ContentGen::JsonString(RandomCase(title))}};
		if (OneIn(2)) {
			spec.before.push_back({"name", ContentGen::JsonString("Game \xE2\x9C\x93")});
		}
		spec.before.push_back({"version", ContentGen::JsonString(version)});
		spec.before.push_back({"process", ContentGen::JsonString(RandomCase(process))});
		if (OneIn(2)) {
			spec.before.push_back({"x-before", UnknownValue()});
		}
		const int mods = Choose(1, 6);
		for (int i = 0; i < mods; i++) {
			ContentGen::JsonObjectSpec mod = {
			    {"name", ContentGen::JsonString("Cheat " + std::to_string(i))}};
			const auto enabled = Choose(0, 3);
			if (enabled != 0) {
				mod.push_back({"enabled", enabled == 1 ? "true" : "false"});
			}
			std::vector<std::string> writes;
			const int                count = Choose(1, 4);
			for (int w = 0; w < count; w++) {
				writes.push_back(MemoryWrite());
			}
			mod.push_back({"memory", ContentGen::JsonArrayText(writes)});
			if (OneIn(3)) {
				mod.push_back({"note", UnknownValue()});
			}
			spec.mods.push_back(std::move(mod));
		}
		if (OneIn(2)) {
			spec.after = {{"credits", "[\"Tester\",3]"}, {"x-after", UnknownValue()}};
		}
		const auto remote_text = ContentGen::CheatDocumentText(spec);
		RC_LOG() << spelling << " " << version << " " << process << ": " << remote_text << "\n";
		const auto remote = kyty::core::ParseCheatDocument(remote_text, "remote.json");
		RC_ASSERT(remote.has_value());
		RC_ASSERT(!kyty::core::ValidateRemoteImport(*remote, spelling, version, process));

		// The Data_Directory: other titles' files, other data, and maybe the game's own file.
		const KytyTests::TempDir temp("kyty_p32");
		const auto               data = temp.Path() / "data";
		temp.WriteFile("data/_Patches/PPSA00000.json", "{\"mods\":[]}");
		temp.WriteFile("data/_Patches/readme.txt", "other");
		temp.WriteFile("data/_SaveData/" + title + "/save.bin", "save");
		const auto target = data / "_Patches" / (title + ".json");
		const int  local  = Choose(0, 3); // 0: no file, 1: a valid file, 2: an invalid file
		if (local == 1) {
			temp.WriteFile("data/_Patches/" + title + ".json",
			               R"({"mods":[{"name":"old","enabled":true}]})");
		} else if (local == 2) {
			temp.WriteFile("data/_Patches/" + title + ".json", "{ not json");
		}
		const auto loaded = kyty::core::LoadLocalCheats(data, spelling);
		RC_ASSERT(loaded.path == target && loaded.readable);
		const auto before = ContentGen::SnapshotTree(data);

		if (local != 0) {
			const auto refused =
			    kyty::core::ImportRemoteCheats(data, spelling, *remote, std::nullopt);
			RC_ASSERT(!refused.has_value());
			RC_ASSERT(refused.error().kind == kyty::core::CheatSaveErrorKind::NeedsConfirmation);
			RC_ASSERT(ContentGen::SnapshotTree(data) == before);
		}
		std::optional<std::string_view> expected_bytes;
		if (loaded.bytes) {
			expected_bytes = *loaded.bytes;
		}
		const auto imported =
		    kyty::core::ImportRemoteCheats(data, spelling, *remote, expected_bytes);
		RC_ASSERT(imported.has_value());
		RC_ASSERT(*imported == target);

		// The written file: the remote cheats, all disabled, every other member preserved.
		const auto written = kyty::core::LoadLocalCheats(data, title);
		RC_ASSERT(written.document.has_value());
		RC_ASSERT(written.document->cheats.size() == remote->cheats.size());
		for (std::size_t i = 0; i < remote->cheats.size(); i++) {
			RC_ASSERT(!written.document->cheats[i].enabled);
			RC_ASSERT(written.document->cheats[i].name == remote->cheats[i].name);
			RC_ASSERT(written.document->cheats[i].memory == remote->cheats[i].memory);
		}
		const auto expected = kyty::core::ParseCheatDocument(
		    ContentGen::CheatDocumentText(ContentGen::AllDisabledSpec(spec)), "expected.json");
		RC_ASSERT(expected.has_value());
		RC_ASSERT(written.document->json == expected->json);

		// Nothing else changed.
		auto after = ContentGen::SnapshotTree(data);
		auto rest  = before;
		after.erase("_Patches/" + title + ".json");
		rest.erase("_Patches/" + title + ".json");
		RC_ASSERT(after == rest);
		RC_TAG(local == 0   ? "no local file"
		       : local == 1 ? "replaced a valid file"
		                    : "replaced an invalid file");
	});
}
