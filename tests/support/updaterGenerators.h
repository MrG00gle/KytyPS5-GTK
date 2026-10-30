#pragma once

// Fixtures for the Updater core tests (task 17.5): release documents, byte streams, and result
// messages, for tests/updater/*Tests.cpp and the updater properties (Properties 41, 42, and 44).
// Header only and free of RapidCheck and of any JSON library, because the unit tests build without
// exceptions: the properties draw their inputs themselves and pass them to these builders.
//
// - Release documents: ReleaseSpec and AssetSpec hold each member as raw JSON text (or leave it
//   out), so a test can write valid, missing, and wrongly typed members; ReleaseDocument() joins
//   them into a GitHub release object, and ValidRelease() is a release that ParseRelease accepts.
// - Digests: DigestHex() and DigestFromHex() convert kyty::updater::Sha256Bytes.
// - Byte streams: PatternBytes() makes deterministic content; SplitBytes() cuts it into chunks.
// - Messages: Utf8Character() encodes a code point; RepeatToSize() fills a message to a size.
// - Trees: SnapshotTree() records every entry below a directory, so a test can check that an
//   installation directory is byte-identical before and after an operation.

#include "updater/core/sha256.h"
#include "updater/core/updatePolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace KytyTests::UpdaterGen {

// ---- JSON text

// text as a JSON string literal: quotes, backslashes, and control characters escaped; other bytes
// are copied (text must be valid UTF-8 for the parser to accept it).
inline std::string JsonString(std::string_view text) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string           out    = "\"";
	for (const char c: text) {
		const auto byte = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (byte < 0x20) {
			out += "\\u00";
			out += kHex[byte >> 4u];
			out += kHex[byte & 0xFu];
		} else {
			out += c;
		}
	}
	out += '"';
	return out;
}

// ---- Digests

inline std::string DigestHex(const kyty::updater::Sha256Bytes& digest, bool upper_case = false) {
	const char* hex = upper_case ? "0123456789ABCDEF" : "0123456789abcdef";
	std::string text;
	for (const uint8_t byte: digest) {
		text += hex[byte >> 4u];
		text += hex[byte & 0xFu];
	}
	return text;
}

// 64 hexadecimal digits (either case) to bytes; nullopt for anything else.
inline std::optional<kyty::updater::Sha256Bytes> DigestFromHex(std::string_view hex) {
	if (hex.size() != 64) {
		return std::nullopt;
	}
	const auto digit = [](char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	};
	kyty::updater::Sha256Bytes digest {};
	for (size_t i = 0; i < digest.size(); i++) {
		const int high = digit(hex[2 * i]);
		const int low  = digit(hex[2 * i + 1]);
		if (high < 0 || low < 0) {
			return std::nullopt;
		}
		digest[i] = static_cast<uint8_t>(high * 16 + low);
	}
	return digest;
}

inline kyty::updater::Sha256Bytes DigestOf(std::string_view bytes) {
	return kyty::updater::Sha256Digest(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

// ---- Release documents

// One element of "assets". Each member is raw JSON text; nullopt leaves the member out. When
// element is set, it is the whole element instead (a value that is not an object).
struct AssetSpec {
	std::optional<std::string> name;
	std::optional<std::string> digest;
	std::optional<std::string> size;
	std::optional<std::string> element;
};

struct ReleaseSpec {
	std::optional<std::string> tag_name;
	std::optional<std::string> published_at;
	std::optional<std::string> draft;
	std::optional<std::string> prerelease;
	// The "assets" member: raw JSON text that replaces the array, or the array of these elements.
	std::optional<std::string> assets_json;
	bool                       has_assets = true;
	std::vector<AssetSpec>     assets;
	// Members GitHub also sends, written first (the parser ignores them).
	std::vector<std::pair<std::string, std::string>> other_members = {
	    {"url", JsonString("https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/1")},
	    {"id", "1"},
	    {"body", JsonString("Release notes")}};
};

inline std::string AssetText(const AssetSpec& asset) {
	if (asset.element) {
		return *asset.element;
	}
	std::string text = "{\"content_type\":\"application/gzip\"";
	for (const auto& [key, value]:
	     {std::pair {"name", &asset.name}, std::pair {"digest", &asset.digest},
	      std::pair {"size", &asset.size}}) {
		if (*value) {
			text += ",\"";
			text += key;
			text += "\":" + **value;
		}
	}
	return text + "}";
}

inline std::string ReleaseDocument(const ReleaseSpec& spec) {
	std::string text   = "{";
	bool        first  = true;
	const auto  member = [&](std::string_view key, const std::string& value) {
		text += first ? "" : ",";
		first = false;
		text += JsonString(key) + ":" + value;
	};
	for (const auto& [key, value]: spec.other_members) {
		member(key, value);
	}
	for (const auto& [key, value]:
	     {std::pair {"tag_name", &spec.tag_name}, std::pair {"published_at", &spec.published_at},
	      std::pair {"draft", &spec.draft}, std::pair {"prerelease", &spec.prerelease}}) {
		if (*value) {
			member(key, **value);
		}
	}
	if (spec.assets_json) {
		member("assets", *spec.assets_json);
	} else if (spec.has_assets) {
		std::string array = "[";
		for (size_t i = 0; i < spec.assets.size(); i++) {
			array += (i == 0 ? "" : ",") + AssetText(spec.assets[i]);
		}
		member("assets", array + "]");
	}
	return text + "}";
}

// An asset that ParseRelease accepts for tag: the right name, a sha256: digest, and size.
inline AssetSpec ValidAsset(std::string_view tag, const kyty::updater::Sha256Bytes& digest,
                            uint64_t size) {
	AssetSpec asset;
	asset.name   = JsonString(kyty::updater::AssetNameForTag(tag));
	asset.digest = JsonString("sha256:" + DigestHex(digest));
	asset.size   = std::to_string(size);
	return asset;
}

// A published stable release with a source archive and the update package.
inline ReleaseSpec ValidRelease(std::string_view tag, std::string_view published_at,
                                const kyty::updater::Sha256Bytes& digest, uint64_t size) {
	ReleaseSpec spec;
	spec.tag_name     = JsonString(tag);
	spec.published_at = JsonString(published_at);
	spec.draft        = "false";
	spec.prerelease   = "false";
	AssetSpec source;
	source.name   = JsonString(std::string(tag) + "-source.tar.gz");
	source.digest = JsonString("sha256:" + std::string(64, '0'));
	source.size   = "10";
	spec.assets   = {source, ValidAsset(tag, digest, size)};
	return spec;
}

// ---- Byte streams

// size bytes of content that differs at every offset modulo 251 and between seeds.
inline std::string PatternBytes(size_t size, uint32_t seed = 0) {
	std::string bytes(size, '\0');
	uint32_t    state = seed * 2654435761u + 1;
	for (size_t i = 0; i < size; i++) {
		state    = state * 1103515245u + 12345u;
		bytes[i] = static_cast<char>((state >> 16u) ^ (i % 251));
	}
	return bytes;
}

// bytes cut into consecutive chunks of the given sizes (zero sizes give empty chunks); whatever is
// left after the sizes is one last chunk.
inline std::vector<std::string_view> SplitBytes(std::string_view           bytes,
                                                const std::vector<size_t>& sizes) {
	std::vector<std::string_view> chunks;
	size_t                        offset = 0;
	for (const size_t size: sizes) {
		const size_t count = std::min(size, bytes.size() - offset);
		chunks.push_back(bytes.substr(offset, count));
		offset += count;
	}
	if (offset < bytes.size()) {
		chunks.push_back(bytes.substr(offset));
	}
	return chunks;
}

inline std::span<const std::byte> AsBytes(std::string_view text) {
	return std::as_bytes(std::span(text.data(), text.size()));
}

// ---- Messages

// The UTF-8 encoding of code_point (U+0000-U+10FFFF, surrogates included as they are encoded).
inline std::string Utf8Character(uint32_t code_point) {
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

// prefix followed by copies of unit until the text is exactly size bytes or one more copy would
// pass size; then filled with "x" to size.
inline std::string RepeatToSize(std::string prefix, std::string_view unit, size_t size) {
	std::string text = std::move(prefix);
	while (!unit.empty() && text.size() + unit.size() <= size) {
		text += unit;
	}
	if (text.size() < size) {
		text.append(size - text.size(), 'x');
	}
	return text;
}

// True when text ends at a UTF-8 character boundary: no lead byte of a multi-byte sequence is
// missing its continuation bytes at the end (text itself must be valid UTF-8 up to there).
inline bool EndsAtUtf8Boundary(std::string_view text) {
	size_t continuation = 0;
	for (size_t i = text.size(); i > 0; i--) {
		const auto byte = static_cast<unsigned char>(text[i - 1]);
		if ((byte & 0xC0u) == 0x80u) {
			continuation++;
			continue;
		}
		const size_t expected = byte >= 0xF0 ? 3 : byte >= 0xE0 ? 2 : byte >= 0xC0 ? 1 : 0;
		return continuation == expected;
	}
	return continuation == 0;
}

// ---- Trees

// Every entry below root (not following links): "D" for a directory, "F:<bytes>" for a file,
// "L:<target>" for a symbolic link, "?" otherwise; keyed by the path relative to root.
inline std::map<std::string, std::string> SnapshotTree(const std::filesystem::path& root) {
	namespace fs = std::filesystem;
	std::map<std::string, std::string> tree;
	std::error_code                    ec;
	for (auto it = fs::recursive_directory_iterator(root, ec);
	     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		const auto      path     = it->path();
		const auto      relative = path.lexically_relative(root).string();
		std::error_code entry_ec;
		const auto      status = fs::symlink_status(path, entry_ec);
		if (entry_ec) {
			tree[relative] = "?";
			continue;
		}
		if (status.type() == fs::file_type::symlink) {
			tree[relative] = "L:" + fs::read_symlink(path, entry_ec).string();
		} else if (status.type() == fs::file_type::directory) {
			tree[relative] = "D";
		} else if (status.type() == fs::file_type::regular) {
			std::ifstream in(path, std::ios::binary);
			tree[relative] = "F:" + std::string(std::istreambuf_iterator<char>(in),
			                                    std::istreambuf_iterator<char>());
		} else {
			tree[relative] = "?";
		}
	}
	return tree;
}

// The whole content of a file, or nullopt when it cannot be opened.
inline std::optional<std::string> ReadFile(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return std::nullopt;
	}
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace KytyTests::UpdaterGen
