// Feature: linux-gtk-modernization, Property 30: Cheat document round trip
//
// Validates Requirement 11.7. Each case writes a cheat document as JSON text (member by member,
// through KytyTests::ContentGen::CheatDocumentSpec): 0-6 mods whose members come in any order, a
// non-empty name of ASCII, escapes, control characters, Latin-1, CJK, and four-byte characters,
// "enabled" absent, true, or false, "memory" absent or any JSON value (nested arrays and objects,
// integers up to 64 bits, floats, strings), and unknown members; top-level members before and
// after "mods" (id, name, version, process, author, credits with values of any type, unknown keys).
// ParseCheatDocument must accept it with the written names in order; after the enabled states of
// some cheats are toggled (the Local view's check boxes),
// ParseCheatDocument(SerializeCheatDocument) must give the same cheats (names, enabled states,
// memory) and metadata, the same document when nothing was toggled, and a fixed point when
// serialized again.

#include "launcher/core/cheats.h"
#include "support/contentGenerators.h"
#include "support/property.h"

#include <cstddef>
#include <cstdint>
#include <set>
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

std::string Utf8(uint32_t code_point) {
	std::string text;
	if (code_point < 0x80) {
		text += static_cast<char>(code_point);
	} else if (code_point < 0x800) {
		text += static_cast<char>(0xC0 | (code_point >> 6u));
		text += static_cast<char>(0x80 | (code_point & 0x3Fu));
	} else if (code_point < 0x10000) {
		text += static_cast<char>(0xE0 | (code_point >> 12u));
		text += static_cast<char>(0x80 | ((code_point >> 6u) & 0x3Fu));
		text += static_cast<char>(0x80 | (code_point & 0x3Fu));
	} else {
		text += static_cast<char>(0xF0 | (code_point >> 18u));
		text += static_cast<char>(0x80 | ((code_point >> 12u) & 0x3Fu));
		text += static_cast<char>(0x80 | ((code_point >> 6u) & 0x3Fu));
		text += static_cast<char>(0x80 | (code_point & 0x3Fu));
	}
	return text;
}

// 1-max_length code points, valid UTF-8.
std::string Text(int max_length) {
	std::string text;
	const int   length = Choose(1, max_length + 1);
	for (int i = 0; i < length; i++) {
		switch (Choose(0, 6)) {
			case 0: text += Utf8(Choose<uint32_t>(0x20, 0x7F)); break;
			case 1:
				text += Pick(std::vector<std::string> {"\"", "\\", "\n", "\t", "\x01", "/"});
				break;
			case 2: text += Utf8(Choose<uint32_t>(0xA0, 0x100)); break;
			case 3: text += Utf8(Choose<uint32_t>(0x4E00, 0x4F00)); break;
			case 4: text += Utf8(Choose<uint32_t>(0x1F600, 0x1F650)); break;
			default: text += Utf8(Choose<uint32_t>('a', 'z' + 1)); break;
		}
	}
	return text;
}

std::string Value(int depth);

std::string ObjectValue(int depth) {
	ContentGen::JsonObjectSpec members;
	std::set<std::string>      keys;
	const int                  count = Choose(0, 4);
	for (int i = 0; i < count; i++) {
		auto key = Text(4);
		if (keys.insert(key).second) {
			members.push_back({key, Value(depth - 1)});
		}
	}
	return ContentGen::JsonObjectText(members);
}

std::string Value(int depth) {
	switch (Choose(0, depth > 0 ? 9 : 7)) {
		case 0: return "null";
		case 1: return OneIn(2) ? "true" : "false";
		case 2: return std::to_string(Choose<int64_t>(-1000000000000, 1000000000000));
		case 3:
			return Pick(std::vector<std::string> {"18446744073709551615", "-9223372036854775808",
			                                      "0", "-0", "42"});
		case 4:
			return Pick(std::vector<std::string> {"0.5", "-1.25e-7", "3.141592653589793", "1e300",
			                                      "2.5E+10", "-0.0", "1.0", "123456789.125"});
		case 5:
		case 6: return ContentGen::JsonString(Text(8));
		case 7: {
			std::vector<std::string> values;
			const int                count = Choose(0, 4);
			for (int i = 0; i < count; i++) {
				values.push_back(Value(depth - 1));
			}
			return ContentGen::JsonArrayText(values);
		}
		default: return ObjectValue(depth);
	}
}

template <typename T>
void Shuffle(std::vector<T>& values) {
	for (std::size_t i = values.size(); i > 1; i--) {
		std::swap(values[i - 1], values[Choose<std::size_t>(0, i)]);
	}
}

struct GeneratedMod {
	ContentGen::JsonObjectSpec members;
	std::string                name;
};

GeneratedMod Mod() {
	GeneratedMod mod;
	mod.name = Text(12);
	mod.members.push_back({"name", ContentGen::JsonString(mod.name)});
	if (!OneIn(3)) {
		mod.members.push_back({"enabled", OneIn(2) ? "true" : "false"});
	}
	if (!OneIn(4)) {
		mod.members.push_back({"memory", Value(2)});
	}
	for (const auto* key: {"note", "author", "\xC3\xA9t\xC3\xA9"}) {
		if (OneIn(4)) {
			mod.members.push_back({key, Value(1)});
		}
	}
	Shuffle(mod.members);
	return mod;
}

ContentGen::JsonObjectSpec TopMembers(std::set<std::string>& used) {
	static const std::vector<std::string> kKeys = {"id",     "name",    "version", "process",
	                                               "author", "credits", "x-extra", "\xE2\x9C\x93"};
	ContentGen::JsonObjectSpec            members;
	const int                             count = Choose(0, 5);
	for (int i = 0; i < count; i++) {
		const auto& key = Pick(kKeys);
		if (!used.insert(key).second) {
			continue;
		}
		std::string value = OneIn(4) ? Value(2) : ContentGen::JsonString(Text(10));
		if (key == "credits" && !OneIn(4)) {
			value = ContentGen::JsonArrayText(
			    {ContentGen::JsonString(Text(6)), Value(0), ContentGen::JsonString(Text(6))});
		}
		members.push_back({key, value});
	}
	return members;
}

} // namespace

KYTY_PROPERTY(30, "Cheat document round trip") {
	return rc::check(name, []() {
		ContentGen::CheatDocumentSpec spec;
		std::set<std::string>         used = {"mods"};
		spec.before                        = TopMembers(used);
		std::vector<std::string> names;
		const int                mods = Choose(0, 7);
		for (int i = 0; i < mods; i++) {
			auto mod = Mod();
			names.push_back(mod.name);
			spec.mods.push_back(std::move(mod.members));
		}
		spec.after      = TopMembers(used);
		const auto text = ContentGen::CheatDocumentText(spec);
		RC_LOG() << text << "\n";

		const auto parsed = kyty::core::ParseCheatDocument(text, "p30.json");
		RC_ASSERT(parsed.has_value());
		RC_ASSERT(parsed->cheats.size() == names.size());
		for (std::size_t i = 0; i < names.size(); i++) {
			RC_ASSERT(parsed->cheats[i].name == names[i]);
		}

		auto document = *parsed;
		bool toggled  = false;
		for (auto& cheat: document.cheats) {
			if (OneIn(3)) {
				cheat.enabled = !cheat.enabled;
				toggled       = true;
			}
		}
		const auto serialized = kyty::core::SerializeCheatDocument(document);
		const auto again      = kyty::core::ParseCheatDocument(serialized, "p30.json");
		RC_ASSERT(again.has_value());
		RC_ASSERT(again->cheats == document.cheats);
		RC_ASSERT(again->id == document.id && again->name == document.name &&
		          again->version == document.version && again->process == document.process &&
		          again->author == document.author && again->credits == document.credits);
		if (!toggled) {
			RC_ASSERT(again->json == document.json);
		}
		const auto fixed =
		    kyty::core::ParseCheatDocument(kyty::core::SerializeCheatDocument(*again), "p30.json");
		RC_ASSERT(fixed.has_value() && fixed->json == again->json);
		RC_TAG(toggled ? "toggled" : "unchanged");
		RC_TAG(names.empty() ? "no cheats" : "cheats");
	});
}
