// Feature: linux-gtk-modernization, Property 3: Scanner matches the reference model
//
// Validates Requirements 7.2, 7.10, and 7.15. Each case generates a tree model (scanGenerators.h:
// nested directories, eboot.bin as file, directory, link, or other entry, games below games, valid
// and invalid archives with .zar, .ZAR, and other names, symbolic links to anything including
// loops and dangling targets, unreadable directories) and up to four possibly overlapping,
// duplicated, aliased, or missing folders. The scan on InMemoryScanFileSystem must equal a
// reference model that walks the tree description directly (it resolves only the folders through
// links, with its own resolver, and recurses over the model's real nodes):
// - the same entries (canonical path, kind, legacy path, metadata), each canonical path once;
// - exactly one warning per missing folder or unreadable directory;
// - after removing one folder, exactly the entries no remaining folder contains are gone.

#include "launcher/core/cancelToken.h"
#include "launcher/core/gameScanner.h"
#include "support/fakeScanFileSystem.h"
#include "support/property.h"
#include "support/scanGenerators.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs      = std::filesystem;
namespace ScanGen = KytyTests::ScanGen;

using kyty::core::GameEntry;
using ScanGen::ModelNode;

struct ExpectedEntry {
	GameEntry::Kind kind;
	std::string     legacy_relative_path;
};

struct ReferenceResult {
	std::map<std::string, ExpectedEntry> entries;  // by canonical path
	std::set<std::string>                warnings; // canonical (or lexical) path of each warning
};

class ReferenceModel {
public:
	explicit ReferenceModel(const ScanGen::TreeModel& model): m_model(model) {
		for (const auto& node: model.nodes) {
			m_nodes.emplace(node.path.string(), &node);
		}
	}

	ReferenceResult Run(const std::vector<fs::path>& folders) const {
		ReferenceResult expected;
		for (const auto& folder: folders) {
			const auto root = Resolve(folder);
			if (!root) {
				expected.warnings.insert(KytyTests::NormalScanPath(folder).string());
				continue;
			}
			if (!IsDirectory(*root)) {
				expected.warnings.insert(root->string());
				continue;
			}
			Walk(*root, *root, expected);
		}
		return expected;
	}

private:
	[[nodiscard]] bool Exists(const fs::path& path) const {
		return path == "/" || path == m_model.root || m_nodes.contains(path.string());
	}

	[[nodiscard]] const ModelNode* Find(const fs::path& path) const {
		const auto it = m_nodes.find(path.string());
		return it == m_nodes.end() ? nullptr : it->second;
	}

	[[nodiscard]] bool IsDirectory(const fs::path& path) const {
		if (path == "/" || path == m_model.root) {
			return true;
		}
		const auto* node = Find(path);
		return node != nullptr && node->kind == ModelNode::Kind::Directory;
	}

	// realpath() over the model: a stack of pending names, links expanded in place.
	[[nodiscard]] std::optional<fs::path> Resolve(const fs::path& path) const {
		std::vector<std::string> pending;
		const auto               push = [&pending](const fs::path& text) {
			std::vector<std::string> names;
			for (const auto& element: text.relative_path()) {
				if (!element.empty() && element != ".") {
					names.push_back(element.string());
				}
			}
			pending.insert(pending.end(), names.rbegin(), names.rend());
		};
		const auto last_text       = path.empty() ? std::string() : (--path.end())->string();
		const bool needs_directory = last_text.empty() || last_text == ".";
		push(path);

		fs::path current = "/";
		int      links   = 0;
		while (!pending.empty()) {
			const auto name = pending.back();
			pending.pop_back();
			if (name == "..") {
				current = current.parent_path();
				continue;
			}
			const auto next = current / name;
			if (!Exists(next)) {
				return std::nullopt;
			}
			const auto* node = Find(next);
			if (node != nullptr && node->kind == ModelNode::Kind::Symlink) {
				if (++links > 40) {
					return std::nullopt;
				}
				if (node->target.is_absolute()) {
					current = "/";
				}
				push(node->target);
				continue;
			}
			current = next;
			if (!pending.empty() && !IsDirectory(current)) {
				return std::nullopt;
			}
		}
		if (needs_directory && !IsDirectory(current)) {
			return std::nullopt;
		}
		return current;
	}

	[[nodiscard]] std::vector<const ModelNode*> Children(const fs::path& directory) const {
		std::vector<const ModelNode*> children;
		for (const auto& node: m_model.nodes) {
			if (node.path.parent_path() == directory) {
				children.push_back(&node);
			}
		}
		return children;
	}

	void Walk(const fs::path& directory, const fs::path& root, ReferenceResult& expected) const {
		if (directory == "/") {
			// The only child of "/" is the model root.
			Walk(m_model.root, root, expected);
			return;
		}
		const auto* self = Find(directory);
		if (self != nullptr && self->unreadable) {
			expected.warnings.insert(directory.string());
			return;
		}
		const auto add = [&](const fs::path& path, GameEntry::Kind kind) {
			expected.entries.try_emplace(
			    path.string(),
			    ExpectedEntry {kind, path.lexically_relative(root).generic_string()});
		};
		bool is_game = false;
		for (const auto* child: Children(directory)) {
			const auto name = child->path.filename().string();
			if (child->kind == ModelNode::Kind::ValidArchive && name.ends_with(".zar")) {
				add(child->path, GameEntry::Kind::Archive);
			}
			if (child->kind == ModelNode::Kind::File && name == "eboot.bin") {
				is_game = true;
			}
		}
		if (is_game) {
			add(directory, GameEntry::Kind::Directory);
			return;
		}
		for (const auto* child: Children(directory)) {
			if (child->kind == ModelNode::Kind::Directory) {
				Walk(child->path, root, expected);
			}
		}
	}

	const ScanGen::TreeModel&               m_model;
	std::map<std::string, const ModelNode*> m_nodes;
};

kyty::core::ScanResult Scan(const std::vector<fs::path>&             folders,
                            KytyTests::InMemoryScanFileSystem&       files,
                            const KytyTests::InMemoryMetadataReader& metadata) {
	const kyty::core::CancelToken cancel;
	return kyty::core::ScanGameFolders(folders, files, metadata, cancel);
}

void CheckAgainst(const kyty::core::ScanResult& result, const ReferenceResult& expected,
                  KytyTests::InMemoryScanFileSystem&       files,
                  const KytyTests::InMemoryMetadataReader& metadata) {
	RC_ASSERT(!result.cancelled);
	std::map<std::string, ExpectedEntry> actual;
	for (const auto& entry: result.entries) {
		RC_ASSERT(actual
		              .try_emplace(entry.path.string(),
		                           ExpectedEntry {entry.kind, entry.legacy_relative_path})
		              .second);
		const auto read = metadata.Read(entry.path, entry.kind);
		RC_ASSERT(entry.title_name == read.title_name);
		RC_ASSERT(entry.title_id == read.title_id);
		RC_ASSERT(entry.game_version == read.game_version);
		RC_ASSERT(entry.firmware_version == read.firmware_version);
		RC_ASSERT(entry.metadata_available == read.metadata_available);
	}
	RC_ASSERT(actual.size() == expected.entries.size());
	for (const auto& [path, entry]: expected.entries) {
		const auto it = actual.find(path);
		RC_ASSERT(it != actual.end());
		RC_ASSERT(it->second.kind == entry.kind);
		RC_ASSERT(it->second.legacy_relative_path == entry.legacy_relative_path);
	}

	std::set<std::string> warned;
	for (const auto& warning: result.warnings) {
		const auto canonical = files.Canonical(warning.path);
		warned.insert(canonical ? canonical->string()
		                        : KytyTests::NormalScanPath(warning.path).string());
		RC_ASSERT(!warning.reason.empty());
	}
	RC_ASSERT(warned == expected.warnings);
	RC_ASSERT(result.warnings.size() == expected.warnings.size());
}

} // namespace

KYTY_PROPERTY(3, "Scanner matches the reference model") {
	return rc::check(name, []() {
		const auto model = ScanGen::GenerateTreeModel();
		RC_LOG() << ScanGen::Describe(model);

		KytyTests::InMemoryScanFileSystem files;
		KytyTests::InMemoryMetadataReader metadata;
		ScanGen::BuildFileSystem(model, files, metadata);
		files.SetListingOrder(*rc::gen::arbitrary<uint64_t>());
		const ReferenceModel reference(model);

		const auto full     = Scan(model.folders, files, metadata);
		const auto expected = reference.Run(model.folders);
		CheckAgainst(full, expected, files, metadata);

		if (model.folders.size() > 1) {
			auto       remaining = model.folders;
			const auto removed   = ScanGen::Choose<size_t>(0, remaining.size());
			remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(removed));
			const auto partial          = Scan(remaining, files, metadata);
			const auto expected_partial = reference.Run(remaining);
			CheckAgainst(partial, expected_partial, files, metadata);

			// Exactly the entries that no remaining folder contains are gone.
			std::set<std::string> full_paths;
			std::set<std::string> partial_paths;
			for (const auto& entry: full.entries) {
				full_paths.insert(entry.path.string());
			}
			for (const auto& entry: partial.entries) {
				partial_paths.insert(entry.path.string());
				RC_ASSERT(full_paths.contains(entry.path.string()));
			}
			for (const auto& path: full_paths) {
				RC_ASSERT(partial_paths.contains(path) == expected_partial.entries.contains(path));
			}
		}
		RC_TAG(expected.entries.empty() ? "no entries" : "entries");
		RC_TAG(expected.warnings.empty() ? "no warnings" : "warnings");
	});
}
