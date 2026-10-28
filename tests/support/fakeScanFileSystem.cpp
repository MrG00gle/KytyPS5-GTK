#include "support/fakeScanFileSystem.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace KytyTests {

namespace {

using kyty::core::Expected;
using kyty::core::FsEntry;
using kyty::core::MakeUnexpected;

constexpr int kMaxSymlinks = 40;

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

[[noreturn]] void Fail(const std::string& what) {
	std::fprintf(stderr, "InMemoryScanFileSystem: %s\n", what.c_str());
	std::abort();
}

// splitmix64 finalizer.
uint64_t Mix(uint64_t value) {
	value += 0x9E3779B97F4A7C15ull;
	value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
	value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
	return value ^ (value >> 31u);
}

// FNV-1a over the text, mixed with the seed.
uint64_t Hash(uint64_t seed, const std::string& text) {
	uint64_t hash = 0xCBF29CE484222325ull ^ Mix(seed);
	for (const char c: text) {
		hash ^= static_cast<unsigned char>(c);
		hash *= 0x100000001B3ull;
	}
	return Mix(hash);
}

} // namespace

std::filesystem::path NormalScanPath(const std::filesystem::path& path) {
	auto normal = path.lexically_normal();
	if (!normal.has_filename() && normal.has_relative_path()) {
		normal = normal.parent_path();
	}
	return normal;
}

// ---- FaultInjectingScanFileSystem

void FaultInjectingScanFileSystem::FailList(const std::filesystem::path& dir, std::string error) {
	m_list_failures[NormalScanPath(dir)] = std::move(error);
}

void FaultInjectingScanFileSystem::FailCanonical(const std::filesystem::path& path) {
	m_canonical_failures.insert(NormalScanPath(path));
}

void FaultInjectingScanFileSystem::FailArchiveHasRootEboot(const std::filesystem::path& zar) {
	m_archive_failures.insert(NormalScanPath(zar));
}

void FaultInjectingScanFileSystem::SetBeforeList(
    std::function<void(const std::filesystem::path&)> hook) {
	m_before_list = std::move(hook);
}

Expected<std::vector<FsEntry>, std::string>
FaultInjectingScanFileSystem::List(const std::filesystem::path& dir) {
	m_list_calls.push_back(dir);
	if (m_before_list) {
		m_before_list(dir);
	}
	const auto failure = m_list_failures.find(NormalScanPath(dir));
	if (failure != m_list_failures.end()) {
		return MakeUnexpected(failure->second);
	}
	return m_inner.List(dir);
}

bool FaultInjectingScanFileSystem::ArchiveHasRootEboot(const std::filesystem::path& zar) {
	m_archive_calls.push_back(zar);
	if (m_archive_failures.contains(NormalScanPath(zar))) {
		return false;
	}
	return m_inner.ArchiveHasRootEboot(zar);
}

std::optional<std::filesystem::path>
FaultInjectingScanFileSystem::Canonical(const std::filesystem::path& path) {
	if (m_canonical_failures.contains(NormalScanPath(path))) {
		return std::nullopt;
	}
	return m_inner.Canonical(path);
}

// ---- InMemoryScanFileSystem

InMemoryScanFileSystem::InMemoryScanFileSystem() {
	m_nodes["/"] = Node {};
}

InMemoryScanFileSystem::Node& InMemoryScanFileSystem::Put(const std::filesystem::path& path,
                                                          Node::Kind                   kind) {
	const auto normal = NormalScanPath(path);
	if (!normal.is_absolute() || normal == normal.root_path()) {
		Fail("not an absolute path below the root: " + path.string());
	}
	// Create the parents as directories.
	std::filesystem::path current = normal.root_path();
	for (const auto& component: normal.relative_path().parent_path()) {
		auto& parent = m_nodes[current];
		if (parent.kind != Node::Kind::Directory) {
			Fail("a parent is not a directory: " + path.string());
		}
		parent.children.insert(component.native());
		current /= component;
		m_nodes.try_emplace(current);
	}
	auto& parent = m_nodes[current];
	if (parent.kind != Node::Kind::Directory) {
		Fail("the parent is not a directory: " + path.string());
	}
	parent.children.insert(normal.filename().native());

	auto& node = m_nodes[normal];
	if (kind != Node::Kind::Directory && !node.children.empty()) {
		Fail("a directory with children cannot change its kind: " + path.string());
	}
	node.kind = kind;
	return node;
}

void InMemoryScanFileSystem::AddDirectory(const std::filesystem::path& path) {
	Put(path, Node::Kind::Directory);
}

void InMemoryScanFileSystem::AddFile(const std::filesystem::path& path) {
	Put(path, Node::Kind::File);
}

void InMemoryScanFileSystem::AddArchive(const std::filesystem::path& path, bool has_root_eboot) {
	Put(path, Node::Kind::Archive).archive_has_root_eboot = has_root_eboot;
}

void InMemoryScanFileSystem::AddSymlink(const std::filesystem::path& path,
                                        const std::filesystem::path& target) {
	Put(path, Node::Kind::Symlink).target = target;
}

void InMemoryScanFileSystem::AddOther(const std::filesystem::path& path) {
	Put(path, Node::Kind::Other);
}

void InMemoryScanFileSystem::SetUnreadable(const std::filesystem::path& dir, std::string error) {
	const auto normal = NormalScanPath(dir);
	auto&      node =
	    normal == normal.root_path() ? m_nodes[normal] : Put(normal, Node::Kind::Directory);
	node.unreadable = std::move(error);
}

Expected<std::filesystem::path, int>
InMemoryScanFileSystem::Resolve(const std::filesystem::path& path, int& links) const {
	if (!path.is_absolute()) {
		return MakeUnexpected(ENOENT);
	}
	// The names to walk; a trailing "/" or "/." means the last one must be a directory.
	std::vector<std::filesystem::path> components;
	bool                               trailing_directory = false;
	for (const auto& component: path.relative_path()) {
		trailing_directory = component.empty() || component == ".";
		if (!trailing_directory) {
			components.push_back(component);
		}
	}

	std::filesystem::path current = path.root_path();
	for (size_t index = 0; index < components.size(); index++) {
		const auto& component = components[index];
		if (component == "..") {
			current = current.parent_path();
			continue;
		}
		// current is always an existing directory here.
		const auto candidate = current / component;
		const auto it        = m_nodes.find(candidate);
		if (it == m_nodes.end()) {
			return MakeUnexpected(ENOENT);
		}
		const Node& node = it->second;
		if (node.kind == Node::Kind::Symlink) {
			if (++links > kMaxSymlinks) {
				return MakeUnexpected(ELOOP);
			}
			const auto target   = node.target.is_absolute() ? node.target : current / node.target;
			auto       resolved = Resolve(target, links);
			if (!resolved) {
				return resolved;
			}
			current = std::move(*resolved);
		} else {
			current = candidate;
		}
		// Every component but the last must be a directory.
		const bool last = index + 1 == components.size();
		if ((!last || trailing_directory) && m_nodes.at(current).kind != Node::Kind::Directory) {
			return MakeUnexpected(ENOTDIR);
		}
	}
	return current;
}

Expected<std::vector<FsEntry>, std::string>
InMemoryScanFileSystem::List(const std::filesystem::path& dir) {
	m_list_count++;
	int        links    = 0;
	const auto resolved = Resolve(dir, links);
	if (!resolved) {
		return MakeUnexpected(ErrorText(resolved.error()));
	}
	const Node& node = m_nodes.at(*resolved);
	if (node.kind != Node::Kind::Directory) {
		return MakeUnexpected(ErrorText(ENOTDIR));
	}
	if (node.unreadable) {
		return MakeUnexpected(*node.unreadable);
	}

	std::vector<FsEntry> entries;
	entries.reserve(node.children.size());
	for (const auto& name: node.children) {
		FsEntry entry;
		entry.name = name;
		switch (m_nodes.at(*resolved / name).kind) {
			case Node::Kind::Directory: entry.type = FsEntry::Directory; break;
			case Node::Kind::File:
			case Node::Kind::Archive: entry.type = FsEntry::File; break;
			case Node::Kind::Symlink: entry.type = FsEntry::Symlink; break;
			case Node::Kind::Other: entry.type = FsEntry::Other; break;
		}
		entries.push_back(std::move(entry));
	}
	if (m_seed != 0) {
		const auto prefix = resolved->native() + "/";
		std::sort(entries.begin(), entries.end(), [&](const FsEntry& a, const FsEntry& b) {
			const auto ha = Hash(m_seed, prefix + a.name);
			const auto hb = Hash(m_seed, prefix + b.name);
			return ha != hb ? ha < hb : a.name < b.name;
		});
	}
	return entries;
}

bool InMemoryScanFileSystem::ArchiveHasRootEboot(const std::filesystem::path& zar) {
	int        links    = 0;
	const auto resolved = Resolve(zar, links);
	if (!resolved) {
		return false;
	}
	const Node& node = m_nodes.at(*resolved);
	return node.kind == Node::Kind::Archive && node.archive_has_root_eboot;
}

std::optional<std::filesystem::path>
InMemoryScanFileSystem::Canonical(const std::filesystem::path& path) {
	int        links    = 0;
	const auto resolved = Resolve(path, links);
	if (!resolved) {
		return std::nullopt;
	}
	return *resolved;
}

// ---- InMemoryMetadataReader

void InMemoryMetadataReader::SetParamJson(const std::filesystem::path& entry_path,
                                          std::string                  bytes) {
	m_param_json[NormalScanPath(entry_path)] = std::move(bytes);
}

kyty::core::GameMetadata InMemoryMetadataReader::Read(const std::filesystem::path& entry_path,
                                                      kyty::core::GameEntry::Kind  kind) const {
	m_reads.push_back(entry_path);
	const auto fallback = kyty::core::FallbackTitle(entry_path, kind);
	const auto it       = m_param_json.find(NormalScanPath(entry_path));
	if (it == m_param_json.end()) {
		kyty::core::GameMetadata metadata;
		metadata.title_name = fallback;
		return metadata;
	}
	return kyty::core::ParseParamJson(it->second, fallback);
}

} // namespace KytyTests
