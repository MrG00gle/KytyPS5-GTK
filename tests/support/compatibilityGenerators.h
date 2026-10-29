#pragma once

// RapidCheck generators for the compatibility properties (Properties 9 and 10, task 14.3). Header
// only, and only for property TUs: RapidCheck needs exceptions, which the test support library is
// built without. The generator functions draw values with operator* on generators, so call them
// only inside a property.
//
// A database model is a list of entries with distinct canonical title IDs (upper-case ASCII, as
// NormalizeTitleId gives them), each written under a key with random letter case and leading and
// trailing White_Space (ASCII and U+00A0, U+2028, U+3000), and with a member that holds one of the
// Qt status aliases (possibly padded with whitespace), an unrecognized status, a non-string status,
// no status, or no object at all, plus an optional comment. ToJson() writes the model as JSON in a
// random member order; the expected entry of each canonical ID is known from the model.

#include "launcher/core/compatibility.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <rapidcheck.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace KytyTests::CompatGen {

// A uniform choice from [min, max). rc::gen::inRange alone narrows its range at small sizes.
template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

inline bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& PickOf(const std::vector<T>& values) {
	return values[Choose<size_t>(0, values.size())];
}

// A JSON string literal for valid UTF-8 text.
inline std::string JsonString(std::string_view text) {
	std::string out = "\"";
	for (const char c: text) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
			out += escaped;
		} else {
			out += c;
		}
	}
	return out + "\"";
}

// ---- Title IDs

// A canonical title ID: mostly 4 letters and 5 digits ("PPSA01234"), sometimes 1-12 upper-case
// letters and digits.
inline std::string GenerateTitleId() {
	static const std::vector<std::string> kPrefixes = {"PPSA", "CUSA", "PPSB", "ABCD"};
	std::string                           id;
	if (!OneIn(4)) {
		id = PickOf(kPrefixes);
		for (int i = 0; i < 5; i++) {
			id += static_cast<char>('0' + Choose(0, 10));
		}
		return id;
	}
	const int length = Choose(1, 13);
	for (int i = 0; i < length; i++) {
		id += OneIn(2) ? static_cast<char>('A' + Choose(0, 26))
		               : static_cast<char>('0' + Choose(0, 10));
	}
	return id;
}

// 0-2 White_Space code points.
inline std::string GenerateWhitespace() {
	static const std::vector<std::string> kSpaces = {
	    " ", "\t", "\n", "\r", "\v", "\f", "\xC2\xA0", "\xE2\x80\xA8", " ", " ", "\xE3\x80\x80"};
	std::string text;
	const int   count = Choose(0, 3);
	for (int i = 0; i < count; i++) {
		text += PickOf(kSpaces);
	}
	return text;
}

// The ID with each letter in random case, half of the time with whitespace around it.
inline std::string VaryTitleId(const std::string& id) {
	std::string varied;
	for (const char c: id) {
		varied += c >= 'A' && c <= 'Z' && OneIn(2) ? static_cast<char>(c - 'A' + 'a') : c;
	}
	if (OneIn(2)) {
		return varied;
	}
	return GenerateWhitespace() + varied + GenerateWhitespace();
}

// ---- Members

struct StatusValue {
	std::string                     json; // the JSON value; empty for no "status" member
	kyty::core::CompatibilityStatus expected = kyty::core::CompatibilityStatus::Unknown;
};

inline StatusValue GenerateStatusValue() {
	using kyty::core::CompatibilityStatus;
	static const std::vector<std::pair<std::string, CompatibilityStatus>> kAliases = {
	    {"InGame", CompatibilityStatus::InGame},
	    {"In game", CompatibilityStatus::InGame},
	    {"MainMenu", CompatibilityStatus::MainMenu},
	    {"Main menu", CompatibilityStatus::MainMenu},
	    {"Logo", CompatibilityStatus::Logo},
	    {"DoesntBoot", CompatibilityStatus::DoesntBoot},
	    {"Doesn't boot", CompatibilityStatus::DoesntBoot},
	};
	static const std::vector<std::string> kUnknown = {
	    "",           "Unknown",     "ingame",   "IN GAME", "In Game", "logo",
	    "Main  menu", "Doesnt boot", "Playable", "Nothing", "InGame!", "Doesn\xE2\x80\x99t boot"};
	static const std::vector<std::string> kNonString = {
	    "1", "0", "true", "null", "{}", "[\"InGame\"]", "{\"status\": \"InGame\"}"};
	switch (Choose(0, 8)) {
		case 0: return {JsonString(PickOf(kUnknown)), CompatibilityStatus::Unknown};
		case 1: return {PickOf(kNonString), CompatibilityStatus::Unknown};
		case 2: return {"", CompatibilityStatus::Unknown};
		case 3: {
			const auto& [text, status] = PickOf(kAliases);
			return {JsonString(GenerateWhitespace() + text + GenerateWhitespace()), status};
		}
		default: {
			const auto& [text, status] = PickOf(kAliases);
			return {JsonString(text), status};
		}
	}
}

struct EntryModel {
	std::string                    id;  // canonical
	std::string                    key; // as written in the JSON
	std::string                    member_json;
	kyty::core::CompatibilityEntry expected;
};

// A member for id: in 1 of 10 cases not an object (Unknown, no comment).
inline EntryModel GenerateEntry(const std::string& id) {
	static const std::vector<std::string> kNonObjects = {
	    "\"InGame\"", "3", "null", "[]", "[{\"status\": \"InGame\"}]", "false"};
	static const std::vector<std::string> kComments = {"",
	                                                   "Runs well",
	                                                   "Crashes after the intro",
	                                                   "Needs \"patch\" 1.01",
	                                                   "Back\\slash",
	                                                   "\xE4\xB8\xAD\xE6\x96\x87",
	                                                   "Line\nbreak"};
	EntryModel                            entry;
	entry.id  = id;
	entry.key = VaryTitleId(id);
	if (OneIn(10)) {
		entry.member_json = PickOf(kNonObjects);
		return entry;
	}
	std::vector<std::string> members;
	const auto               status = GenerateStatusValue();
	entry.expected.status           = status.expected;
	if (!status.json.empty()) {
		members.push_back("\"status\": " + status.json);
	}
	switch (Choose(0, 4)) {
		case 0: break;
		case 1: members.push_back("\"comment\": 7"); break;
		default:
			entry.expected.comment = PickOf(kComments);
			members.push_back("\"comment\": " + JsonString(entry.expected.comment));
			break;
	}
	if (OneIn(5)) {
		members.push_back("\"extra\": [1, 2]");
	}
	std::string json = "{";
	for (size_t i = 0; i < members.size(); i++) {
		json += (i == 0 ? "" : ", ") + members[i];
	}
	entry.member_json = json + "}";
	return entry;
}

struct DatabaseModel {
	std::vector<EntryModel> entries; // distinct canonical IDs
	// Members whose key is empty after trimming: never entries.
	std::vector<std::string> blank_keys;

	[[nodiscard]] kyty::core::CompatibilityEntry
	ExpectedEntry(const std::string& canonical_id) const {
		for (const auto& entry: entries) {
			if (entry.id == canonical_id) {
				return entry.expected;
			}
		}
		return {};
	}
};

// 0-max_entries entries with distinct IDs; sometimes a member with a blank key.
inline DatabaseModel GenerateDatabase(int max_entries = 20) {
	DatabaseModel              model;
	std::map<std::string, int> used;
	const int                  count = Choose(0, max_entries + 1);
	for (int i = 0; i < count; i++) {
		const auto id = GenerateTitleId();
		if (used[id]++ == 0) {
			model.entries.push_back(GenerateEntry(id));
		}
	}
	if (OneIn(4)) {
		model.blank_keys.push_back(GenerateWhitespace());
	}
	return model;
}

// The model as a JSON object, members in a random order, with random whitespace between tokens.
inline std::string ToJson(const DatabaseModel& model) {
	std::vector<std::string> members;
	for (const auto& entry: model.entries) {
		members.push_back(JsonString(entry.key) + ":" + entry.member_json);
	}
	for (const auto& key: model.blank_keys) {
		members.push_back(JsonString(key) + ": {\"status\": \"InGame\"}");
	}
	auto order = members;
	for (size_t i = order.size(); i > 1; i--) {
		std::swap(order[i - 1], order[Choose<size_t>(0, i)]);
	}
	const std::string gap  = OneIn(2) ? "" : "\n  ";
	std::string       json = "{";
	for (size_t i = 0; i < order.size(); i++) {
		json += (i == 0 ? "" : ",") + gap + order[i];
	}
	return json + gap + "}";
}

// ---- Loader attempts (Property 10)

enum class AttemptKind { Network, Status, Timeout, Unparseable, NotAnObject, Valid };

inline const char* AttemptName(AttemptKind kind) {
	switch (kind) {
		case AttemptKind::Network: return "network error";
		case AttemptKind::Status: return "non-success status";
		case AttemptKind::Timeout: return "timeout";
		case AttemptKind::Unparseable: return "unparseable body";
		case AttemptKind::NotAnObject: return "non-object JSON";
		case AttemptKind::Valid: return "valid body";
	}
	return "?";
}

struct Attempt {
	AttemptKind         kind   = AttemptKind::Network;
	int                 status = 200;   // Status: the code outside 200-299
	std::string         body;           // Unparseable, NotAnObject, Valid
	DatabaseModel       model;          // Valid
	int                 latency_ms = 0; // Network, Status, Unparseable, NotAnObject, Valid
	std::vector<size_t> chunk_sizes;
};

// Content that is not valid JSON: a cut database, text, or bytes that are not UTF-8.
inline std::string GenerateUnparseable() {
	switch (Choose(0, 4)) {
		case 0: {
			auto json = ToJson(GenerateDatabase(4));
			return json.substr(0, Choose<size_t>(0, json.size()));
		}
		case 1:
			return PickOf(
			    std::vector<std::string> {"", "<html>Not Found</html>", "{,}", "{\"a\" 1}", "nul"});
		case 2: return "{\"PPSA01234\": {\"status\": \"\xC3\x28\"}}";
		default: return ToJson(GenerateDatabase(3)) + " trailing";
	}
}

inline std::string GenerateNotAnObject() {
	return PickOf(std::vector<std::string> {"[]", "[{\"PPSA01234\": {\"status\": \"InGame\"}}]",
	                                        "1", "\"PPSA01234\"", "null", "true", "-0.5"});
}

inline Attempt GenerateAttempt(bool allow_valid) {
	Attempt   attempt;
	const int pick     = Choose(0, allow_valid ? 12 : 10);
	attempt.kind       = pick < 2    ? AttemptKind::Network
	                     : pick < 4  ? AttemptKind::Status
	                     : pick < 6  ? AttemptKind::Timeout
	                     : pick < 8  ? AttemptKind::Unparseable
	                     : pick < 10 ? AttemptKind::NotAnObject
	                                 : AttemptKind::Valid;
	attempt.latency_ms = Choose(0, 3000);
	switch (attempt.kind) {
		case AttemptKind::Network: break;
		case AttemptKind::Status:
			attempt.status = PickOf(std::vector<int> {100, 199, 301, 304, 404, 418, 500, 503, 599});
			break;
		case AttemptKind::Timeout: attempt.latency_ms = Choose(15001, 40000); break;
		case AttemptKind::Unparseable: attempt.body = GenerateUnparseable(); break;
		case AttemptKind::NotAnObject: attempt.body = GenerateNotAnObject(); break;
		case AttemptKind::Valid:
			attempt.model = GenerateDatabase();
			attempt.body  = ToJson(attempt.model);
			break;
	}
	if (!attempt.body.empty() && OneIn(2)) {
		for (int i = Choose(0, 4); i > 0; i--) {
			attempt.chunk_sizes.push_back(Choose<size_t>(0, attempt.body.size() + 1));
		}
	}
	return attempt;
}

// Four attempt outcomes (the loader uses them up to the first valid body). In a third of the
// cases none is valid.
inline std::vector<Attempt> GenerateAttempts() {
	const bool           allow_valid = !OneIn(3);
	std::vector<Attempt> attempts;
	for (int i = 0; i < 4; i++) {
		attempts.push_back(GenerateAttempt(allow_valid));
	}
	return attempts;
}

enum class CacheState { Absent, Valid, Invalid };

inline const char* CacheName(CacheState state) {
	switch (state) {
		case CacheState::Absent: return "no cache";
		case CacheState::Valid: return "valid cache";
		case CacheState::Invalid: return "invalid cache";
	}
	return "?";
}

struct Cache {
	CacheState    state = CacheState::Absent;
	std::string   bytes; // Valid, Invalid
	DatabaseModel model; // Valid
};

inline Cache GenerateCache() {
	Cache cache;
	cache.state = static_cast<CacheState>(Choose(0, 3));
	switch (cache.state) {
		case CacheState::Absent: break;
		case CacheState::Valid:
			cache.model = GenerateDatabase();
			cache.bytes = ToJson(cache.model);
			break;
		case CacheState::Invalid:
			cache.bytes = OneIn(2) ? GenerateUnparseable() : GenerateNotAnObject();
			break;
	}
	return cache;
}

} // namespace KytyTests::CompatGen
