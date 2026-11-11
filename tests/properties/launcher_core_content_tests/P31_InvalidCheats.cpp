// Feature: linux-gtk-modernization, Property 31: Invalid cheat documents are rejected
//
// Validates Requirement 11.6. Each case draws invalid cheat file bytes of one kind: not JSON
// (arbitrary bytes, a valid document cut short or followed by garbage, ill-formed UTF-8 in a
// string), JSON that is not an object, an object without a "mods" array (missing or another
// type), or a "mods" array with one bad mod among valid ones (not an object; a missing, empty, or
// non-string "name"; an "enabled" that is not true or false). ParseCheatDocument must fail with an
// error that starts with the file's name; as the local cheat file in a real Data_Directory,
// LoadLocalCheats must list zero cheats with that error (naming the path), keep the bytes it read,
// and leave the file byte-identical; as a downloaded remote file (FakeHttpClient), the result must
// have no document, not be importable, and name the remote file.

#include "launcher/core/atomicFile.h"
#include "launcher/core/cheats.h"
#include "launcher/core/remoteCheats.h"
#include "support/contentGenerators.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;

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

std::string ValidMod(int index) {
	ContentGen::JsonObjectSpec mod = {
	    {"name", ContentGen::JsonString("cheat " + std::to_string(index))}};
	if (OneIn(2)) {
		mod.push_back({"enabled", OneIn(2) ? "true" : "false"});
	}
	return ContentGen::JsonObjectText(mod);
}

std::string ValidDocument() {
	std::vector<std::string> mods;
	const int                count = Choose(0, 4);
	for (int i = 0; i < count; i++) {
		mods.push_back(ValidMod(i));
	}
	return R"({"id":"PPSA01234","mods":)" + ContentGen::JsonArrayText(mods) + "}";
}

// A mod that breaks one rule.
std::string BadMod() {
	static const std::vector<std::string> kBad = {"1",
	                                              "\"text\"",
	                                              "null",
	                                              "[]",
	                                              "true", // not an object
	                                              "{}",
	                                              R"({"enabled":true})", // no name
	                                              R"({"name":""})",
	                                              R"({"name":null})",
	                                              R"({"name":3})",
	                                              R"({"name":[]})", // bad name
	                                              R"({"name":{}})",
	                                              R"({"name":false})",
	                                              R"({"name":"x","enabled":"true"})",
	                                              R"({"name":"x","enabled":0})", // bad enabled
	                                              R"({"name":"x","enabled":null})",
	                                              R"({"name":"x","enabled":[]})",
	                                              R"({"name":"x","enabled":{}})"};
	return Pick(kBad);
}

struct Draw {
	std::string bytes;
	std::string kind;
};

Draw InvalidBytes() {
	switch (Choose(0, 6)) {
		case 0: {
			auto bytes = *rc::gen::arbitrary<std::string>();
			bytes += "\x01{"; // never valid JSON
			return {bytes, "arbitrary bytes"};
		}
		case 1: {
			const auto valid = ValidDocument();
			const auto cut   = Choose<std::size_t>(0, valid.size());
			return {OneIn(2)
			            ? valid.substr(0, cut)
			            : valid + Pick(std::vector<std::string> {" x", ",", "}", "{}", "\x01"}),
			        "not JSON"};
		}
		case 2:
			return {R"({"mods":[{"name":")" +
			            std::string(Pick(std::vector<std::string> {"\xFF", "\xC3", "\xED\xA0\x80",
			                                                       "\xF4\x90\x80\x80"})) +
			            R"("}]})",
			        "ill-formed UTF-8"};
		case 3:
			return {Pick(std::vector<std::string> {"[]", "[{\"mods\":[]}]", "\"mods\"", "42",
			                                       "true", "null", "-1.5"}),
			        "not an object"};
		case 4:
			return {Pick(std::vector<std::string> {"{}", R"({"id":"PPSA01234"})", R"({"mods":{}})",
			                                       R"({"mods":null})", R"({"mods":"[]"})",
			                                       R"({"mods":1})", R"({"Mods":[]})"}),
			        "no mods array"};
		default: {
			std::vector<std::string> mods;
			const int                count = Choose(0, 4);
			for (int i = 0; i < count; i++) {
				mods.push_back(ValidMod(i));
			}
			mods.insert(mods.begin() + Choose<std::ptrdiff_t>(0, count + 1), BadMod());
			return {R"({"mods":)" + ContentGen::JsonArrayText(mods) + "}", "bad mod"};
		}
	}
}

} // namespace

KYTY_PROPERTY(31, "Invalid cheat documents are rejected") {
	return rc::check(name, []() {
		const auto draw = InvalidBytes();
		RC_LOG() << draw.kind << ": " << draw.bytes << "\n";

		const auto parsed = kyty::core::ParseCheatDocument(draw.bytes, "PPSA01234.json");
		RC_ASSERT(!parsed.has_value());
		RC_ASSERT(parsed.error().message.starts_with("PPSA01234.json: "));
		// Phase 9 V1: the file is a part of its own, and the message is "<file>: <reason>".
		RC_ASSERT(parsed.error().file == "PPSA01234.json");

		// The local cheat file.
		const KytyTests::TempDir temp("kyty_p31");
		const auto               path  = temp.WriteFile("_Patches/PPSA01234.json", draw.bytes);
		const auto               local = kyty::core::LoadLocalCheats(temp.Path(), "ppsa01234");
		RC_ASSERT(local.path == path);
		RC_ASSERT(!local.document.has_value());
		RC_ASSERT(local.readable && local.bytes == draw.bytes);
		RC_ASSERT(local.message.starts_with(path.string() + ": "));
		RC_ASSERT(local.problem.has_value());
		if (local.problem.has_value()) {
			RC_ASSERT(local.problem->file == path.string() &&
			          local.problem->message == local.message);
		}
		const auto after = kyty::core::ReadFileLimited(path, kyty::core::kMaxCheatFileBytes);
		// RC_ASSERT does not short-circuit &&: the value is checked before it is read.
		RC_ASSERT(after.has_value());
		if (after.has_value()) {
			RC_ASSERT(*after == draw.bytes);
		}

		// A downloaded remote file.
		KytyTests::FakeScheduler                   scheduler;
		KytyTests::FakeHttpClient                  http(scheduler);
		std::vector<kyty::core::RemoteCheatResult> results;
		kyty::core::RemoteCheats                   remote(
		    http, [&](const kyty::core::RemoteCheatResult& result) { results.push_back(result); });
		const kyty::core::RemoteCheatFile file {"PPSA01234_01.000_HenCheats.json", "", "01.000",
		                                        "json"};
		http.AddResponse(kyty::core::RemoteCheatFileUrl(file), ContentGen::ResponseOf(draw.bytes));
		RC_ASSERT(remote.Fetch(file, {"PPSA01234", "01.000", "eboot.bin"}).has_value());
		scheduler.RunUntilIdle();
		RC_ASSERT(results.size() == 1u);
		RC_ASSERT(!results[0].document.has_value() && !results[0].importable);
		RC_ASSERT(results[0].error.starts_with(file.name + ": "));
		RC_ASSERT(results[0].fetch.failure == kyty::core::HttpFailure::None &&
		          results[0].problem.has_value());
		if (results[0].problem.has_value()) {
			RC_ASSERT(results[0].problem->file == file.name &&
			          results[0].problem->message == results[0].error);
		}
		RC_TAG(draw.kind);
	});
}
