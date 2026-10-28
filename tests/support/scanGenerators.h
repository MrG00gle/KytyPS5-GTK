#pragma once

// RapidCheck generators for the scan properties (Properties 1-6, plan D17). Header only, and only
// for property TUs: RapidCheck needs exceptions, which the test support library is built without.
// The functions draw values with operator* on generators, so call them only inside a property.
//
// - GenerateTreeModel(): a directory tree model (directories, regular files, eboot.bin variants,
//   valid and invalid archives, symbolic links to anything including loops and dangling targets,
//   other entries, unreadable directories, param.json texts) and a list of possibly overlapping,
//   duplicated, aliased, or missing Game_Folders, for InMemoryScanFileSystem (BuildFileSystem).
// - SpellDirectory(): aliases of one directory (Property 4).
// - ValidSystemSoftwareVersion() / OtherSystemSoftwareVersion(): Property 5 inputs.
// - UnusableParamJson() / EntryName(): Property 6 inputs.

#include "support/fakeScanFileSystem.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <rapidcheck.h>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace KytyTests::ScanGen {

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

// ---- Tree model (Properties 1-3)

struct ModelNode {
	enum class Kind { Directory, File, ValidArchive, InvalidArchive, Symlink, Other };
	Kind                       kind = Kind::Directory;
	std::filesystem::path      path;   // absolute and lexically normal
	std::filesystem::path      target; // Symlink: absolute or relative to the link's directory
	bool                       unreadable = false; // Directory: List fails
	std::optional<std::string> param_json;         // Directory or ValidArchive: its param.json text
};

struct TreeModel {
	std::filesystem::path              root = "/games"; // a readable directory, not in nodes
	std::vector<ModelNode>             nodes;           // every parent before its children
	std::vector<std::filesystem::path> folders;         // the Game_Folders to scan
};

inline const char* KindName(ModelNode::Kind kind) {
	switch (kind) {
		case ModelNode::Kind::Directory: return "dir";
		case ModelNode::Kind::File: return "file";
		case ModelNode::Kind::ValidArchive: return "zar+";
		case ModelNode::Kind::InvalidArchive: return "zar-";
		case ModelNode::Kind::Symlink: return "link";
		case ModelNode::Kind::Other: return "other";
	}
	return "?";
}

inline std::string Describe(const TreeModel& model) {
	std::ostringstream out;
	out << "root " << model.root.string() << "\n";
	for (const auto& node: model.nodes) {
		out << "  " << KindName(node.kind) << " " << node.path.string();
		if (node.kind == ModelNode::Kind::Symlink) {
			out << " -> " << node.target.string();
		}
		if (node.unreadable) {
			out << " (unreadable)";
		}
		if (node.param_json) {
			out << " param.json=" << node.param_json->size() << " bytes";
		}
		out << "\n";
	}
	out << "folders:";
	for (const auto& folder: model.folders) {
		out << " '" << folder.string() << "'";
	}
	out << "\n";
	return out.str();
}

inline std::string ParamJsonText(int index) {
	switch (Choose(0, 5)) {
		case 0: return "[]";
		case 1: return "{\"titleName\":";
		case 2: return "{}";
		default:
			return "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{"
			       "\"titleName\":\"Title " +
			       std::to_string(index) + "\"}},\"titleId\":\"PPSA0" +
			       std::to_string(1000 + index) + "\",\"appVersion\":\"01.0" +
			       std::to_string(index % 10) +
			       "\",\"requiredSystemSoftwareVersion\":\"0x0150000000000000\"}";
	}
}

inline void GenerateDirectory(TreeModel& model, const std::filesystem::path& directory, int depth,
                              int& budget) {
	static const std::vector<std::string> kNames = {
	    "a",     "b",     "c",         "Game",    "game", "x.zar",   "y.zar",
	    "Z.ZAR", "x.zip", "eboot.bin", "dir.zar", "sub",  "\xC3\xA9" /* e acute */};
	const int             count = depth >= 4 ? 0 : depth == 0 ? Choose(3, 9) : Choose(0, 6);
	std::set<std::string> used;
	// Make a third of the directories below the root games, so most cases find entries (the
	// loop below can still add subdirectories, a game below a game).
	if (depth > 0 && budget > 0 && OneIn(3)) {
		used.insert("eboot.bin");
		budget--;
		ModelNode eboot;
		eboot.kind = ModelNode::Kind::File;
		eboot.path = directory / "eboot.bin";
		model.nodes.push_back(std::move(eboot));
	}
	for (int i = 0; i < count && budget > 0; i++) {
		const auto& name = PickOf(kNames);
		if (!used.insert(name).second) {
			continue;
		}
		budget--;
		ModelNode node;
		node.path           = directory / name;
		const bool zar_name = name.ends_with(".zar") || name.ends_with(".ZAR");
		if (name == "eboot.bin") {
			switch (Choose(0, 8)) {
				case 0: node.kind = ModelNode::Kind::Directory; break;
				case 1: node.kind = ModelNode::Kind::Symlink; break;
				case 2: node.kind = ModelNode::Kind::Other; break;
				default: node.kind = ModelNode::Kind::File; break;
			}
		} else if (zar_name) {
			switch (Choose(0, 7)) {
				case 0: node.kind = ModelNode::Kind::Directory; break;
				case 1: node.kind = ModelNode::Kind::Symlink; break;
				case 2:
				case 3: node.kind = ModelNode::Kind::InvalidArchive; break;
				default: node.kind = ModelNode::Kind::ValidArchive; break;
			}
		} else {
			switch (Choose(0, 9)) {
				case 0: node.kind = ModelNode::Kind::File; break;
				case 1:
				case 2: node.kind = ModelNode::Kind::Symlink; break;
				case 3: node.kind = ModelNode::Kind::Other; break;
				case 4:
					node.kind = ModelNode::Kind::ValidArchive;
					break; // a valid archive not named .zar
				default: node.kind = ModelNode::Kind::Directory; break;
			}
		}
		if (node.kind == ModelNode::Kind::Directory) {
			node.unreadable = OneIn(8);
		}
		if ((node.kind == ModelNode::Kind::Directory ||
		     node.kind == ModelNode::Kind::ValidArchive) &&
		    !OneIn(3)) {
			node.param_json = ParamJsonText(static_cast<int>(model.nodes.size()));
		}
		const auto path    = node.path;
		const bool recurse = node.kind == ModelNode::Kind::Directory;
		model.nodes.push_back(std::move(node));
		if (recurse) {
			GenerateDirectory(model, path, depth + 1, budget);
		}
	}
}

inline TreeModel GenerateTreeModel() {
	TreeModel model;
	int       budget = Choose(4, 50);
	GenerateDirectory(model, model.root, 0, budget);

	std::vector<std::filesystem::path> all_paths {model.root};
	std::vector<std::filesystem::path> directories {model.root};
	std::vector<std::filesystem::path> links;
	std::vector<std::filesystem::path> others;
	for (const auto& node: model.nodes) {
		all_paths.push_back(node.path);
		if (node.kind == ModelNode::Kind::Directory) {
			directories.push_back(node.path);
		} else if (node.kind == ModelNode::Kind::Symlink) {
			links.push_back(node.path);
		} else {
			others.push_back(node.path);
		}
	}

	// Link targets: any node (directories, games, archives, other links), the parent, a dangling
	// name, the link itself (a loop), or an ancestor (a cycle through the tree).
	for (auto& node: model.nodes) {
		if (node.kind != ModelNode::Kind::Symlink) {
			continue;
		}
		switch (Choose(0, 7)) {
			case 0:
			case 1:
			case 2: node.target = PickOf(all_paths); break;
			case 3: node.target = ".."; break;
			case 4: node.target = "missing-target"; break;
			case 5: node.target = node.path.filename(); break;
			default: node.target = node.path.parent_path().parent_path(); break;
		}
	}

	const int count = Choose(1, 5);
	for (int i = 0; i < count; i++) {
		std::filesystem::path folder;
		switch (Choose(-2, 10)) {
			case -2:
			case -1:
			case 0: folder = model.root; break;
			case 1:
				if (!links.empty()) {
					folder = PickOf(links);
					break;
				}
				[[fallthrough]];
			case 2: folder = model.root / ("missing-" + std::to_string(Choose(0, 3))); break;
			case 3:
				if (!others.empty()) {
					folder = PickOf(others);
					break;
				}
				[[fallthrough]];
			case 4:
				if (!model.folders.empty()) {
					folder = PickOf(model.folders);
					break;
				}
				[[fallthrough]];
			default: folder = PickOf(directories); break;
		}
		if (OneIn(5)) {
			folder /= ""; // a trailing separator
		}
		model.folders.push_back(folder);
	}
	return model;
}

inline void BuildFileSystem(const TreeModel& model, InMemoryScanFileSystem& fs,
                            InMemoryMetadataReader& metadata) {
	fs.AddDirectory(model.root);
	for (const auto& node: model.nodes) {
		switch (node.kind) {
			case ModelNode::Kind::Directory:
				fs.AddDirectory(node.path);
				if (node.unreadable) {
					fs.SetUnreadable(node.path);
				}
				break;
			case ModelNode::Kind::File: fs.AddFile(node.path); break;
			case ModelNode::Kind::ValidArchive: fs.AddArchive(node.path, true); break;
			case ModelNode::Kind::InvalidArchive: fs.AddArchive(node.path, false); break;
			case ModelNode::Kind::Symlink: fs.AddSymlink(node.path, node.target); break;
			case ModelNode::Kind::Other: fs.AddOther(node.path); break;
		}
		if (node.param_json) {
			metadata.SetParamJson(node.path, *node.param_json);
		}
	}
}

// ---- Folder spellings (Property 4)

// dir is an absolute path with no symbolic link in it (it need not exist). Returns a spelling of
// it: the path itself, with a trailing separator, with a "/./" or "<dir>/../<dir>" segment,
// relative to the working directory (a physical path), through one of the aliases (links whose
// target is dir), or through one of the parent aliases (links whose target is dir's parent). A
// missing dir must come without aliases: the lexical comparison would not see through them.
inline std::string SpellDirectory(const std::filesystem::path&              dir,
                                  const std::vector<std::filesystem::path>& aliases,
                                  const std::vector<std::filesystem::path>& parent_aliases) {
	std::string text;
	switch (Choose(0, 7)) {
		case 0: text = dir.string() + "/"; break;
		case 1: text = (dir.parent_path() / "." / dir.filename()).string(); break;
		case 2: text = (dir / ".." / dir.filename()).string(); break;
		case 3: {
			std::error_code ec;
			const auto      cwd = std::filesystem::current_path(ec);
			text                = ec ? dir.string() : dir.lexically_relative(cwd).string();
			if (text.empty()) {
				text = dir.string();
			}
			break;
		}
		case 4:
			if (!aliases.empty()) {
				text = PickOf(aliases).string() + (OneIn(2) ? "/" : "");
				break;
			}
			[[fallthrough]];
		case 5:
			if (!parent_aliases.empty()) {
				text = (PickOf(parent_aliases) / dir.filename()).string();
				break;
			}
			[[fallthrough]];
		default: text = dir.string(); break;
	}
	return text;
}

// ---- System software versions (Property 5)

inline std::string Digits(int count, const std::string& alphabet) {
	std::string text;
	for (int i = 0; i < count; i++) {
		text += alphabet[Choose<size_t>(0, alphabet.size())];
	}
	return text;
}

inline const std::string& DecimalAlphabet() {
	static const std::string alphabet = "0123456789";
	return alphabet;
}

inline const std::string& HexAlphabet() {
	static const std::string alphabet = "0123456789abcdefABCDEF";
	return alphabet;
}

struct EncodedVersion {
	std::string raw;
	std::string expected;
};

// "0x" + MMmmpp + ten hex digits, and its formatted form.
inline EncodedVersion ValidSystemSoftwareVersion() {
	const auto     major = Digits(2, DecimalAlphabet());
	const auto     minor = Digits(2, DecimalAlphabet());
	const auto     patch = OneIn(2) ? std::string("00") : Digits(2, DecimalAlphabet());
	EncodedVersion version;
	version.raw      = "0x" + major + minor + patch + Digits(10, HexAlphabet());
	version.expected = std::to_string((major[0] - '0') * 10 + (major[1] - '0')) + "." + minor +
	                   (patch == "00" ? std::string() : "." + patch);
	return version;
}

// A string that is usually not of the encoded form: a valid one with the prefix, the length, a
// decimal digit, or a hex digit broken, or arbitrary text.
inline std::string OtherSystemSoftwareVersion() {
	std::string raw = ValidSystemSoftwareVersion().raw;
	switch (Choose(0, 9)) {
		case 0:
			raw.replace(0, 2, PickOf(std::vector<std::string> {"0X", "1x", "x", "", "00x", " 0x"}));
			break;
		case 1: raw.erase(Choose<size_t>(0, raw.size()), 1); break;
		case 2:
			raw.insert(Choose<size_t>(0, raw.size() + 1), 1,
			           PickOf(std::vector<std::string> {"0", "a", "F"})[0]);
			break;
		case 3:
			raw[Choose<size_t>(2, 8)] =
			    PickOf(std::vector<std::string> {"a", "F", "x", " ", "-", "+"})[0];
			break;
		case 4:
			raw[Choose<size_t>(8, 18)] =
			    PickOf(std::vector<std::string> {"g", "G", "x", " ", "-", "\xC3"})[0];
			break;
		case 5: raw += " "; break;
		case 6: raw = ""; break;
		case 7: raw = raw.substr(0, Choose<size_t>(0, raw.size())); break;
		default: raw = *rc::gen::container<std::string>(rc::gen::inRange<char>(32, 127)); break;
	}
	return raw;
}

// ---- Unusable param.json (Property 6)

struct UnusableJson {
	std::string bytes;
	std::string category; // for RC_TAG
};

inline UnusableJson UnusableParamJson() {
	static const std::string kObject =
	    "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":"
	    "\"Real Title\"}},\"titleId\":\"PPSA01234\",\"appVersion\":\"01.000\","
	    "\"requiredSystemSoftwareVersion\":\"0x0150000000000000\"}";
	switch (Choose(0, 4)) {
		case 0: {
			// A valid object padded with whitespace to more than 1 MiB.
			const size_t size  = (size_t {1} << 20u) + Choose<size_t>(1, 4097);
			std::string  bytes = kObject;
			bytes.append(size - bytes.size(), Choose(0, 2) == 0 ? ' ' : '\n');
			return {std::move(bytes), "larger than 1 MiB"};
		}
		case 1: {
			switch (Choose(0, 8)) {
				case 0:
					return {kObject.substr(0, Choose<size_t>(0, kObject.size())), "invalid JSON"};
				case 1: return {kObject + Digits(Choose(1, 4), "}],x"), "invalid JSON"};
				case 2: return {"{'titleName': 'x'}", "invalid JSON"};
				case 3: return {"{titleName: \"x\"}", "invalid JSON"};
				case 4: return {"{\"titleName\":\"\xFF\xFE\"}", "invalid JSON"};
				case 5: return {"{\"a\":1,}", "invalid JSON"};
				case 6: return {"", "invalid JSON"};
				default:
					return {"#" + *rc::gen::container<std::string>(rc::gen::arbitrary<char>()),
					        "invalid JSON"};
			}
		}
		default: {
			static const std::vector<std::string> kRoots = {"[]",
			                                                "[" + kObject + "]",
			                                                "\"" + std::string("PPSA01234") + "\"",
			                                                "0",
			                                                "-1.5e3",
			                                                "true",
			                                                "false",
			                                                "null",
			                                                " [ {} ] "};
			return {PickOf(kRoots), "not an object"};
		}
	}
}

// A file or directory name: 1-12 characters from letters, digits, space, '-', '_', and '.', not
// starting with '.'.
inline std::string EntryName() {
	static const std::string alphabet =
	    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
	std::string name = Digits(1, alphabet.substr(0, alphabet.size() - 1));
	name += Digits(Choose(0, 12), alphabet);
	return name;
}

} // namespace KytyTests::ScanGen
