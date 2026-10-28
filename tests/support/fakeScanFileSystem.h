#pragma once

// Scan file systems for tests (plan D17):
// - FaultInjectingScanFileSystem wraps another ScanFileSystem (usually the host one) and fails
//   List, Canonical, or ArchiveHasRootEboot for chosen paths, so unreadable directories are
//   simulated without changing permissions (the Fedora CI container runs as root).
// - InMemoryScanFileSystem is a tree model with directories, files, valid and invalid archives,
//   symbolic links, other entries, unreadable directories, and a listing order chosen by a seed;
//   canonical paths are computed in the model. The properties use it to stay fast.
// - InMemoryMetadataReader returns ParseParamJson() of param.json text set per entry path.
// None of them throws; paths are compared in lexically normal form without a trailing separator.

#include "launcher/core/gameMetadata.h"
#include "launcher/core/scanFileSystem.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace KytyTests {

// "/a/./b/" gives "/a/b"; the form the fakes use to compare paths.
std::filesystem::path NormalScanPath(const std::filesystem::path& path);

class FaultInjectingScanFileSystem final: public kyty::core::ScanFileSystem {
public:
	explicit FaultInjectingScanFileSystem(kyty::core::ScanFileSystem& inner): m_inner(inner) {}
	~FaultInjectingScanFileSystem() override = default;

	// List(dir) fails with error.
	void FailList(const std::filesystem::path& dir, std::string error = "Permission denied");
	// Canonical(path) gives nullopt.
	void FailCanonical(const std::filesystem::path& path);
	// ArchiveHasRootEboot(zar) gives false.
	void FailArchiveHasRootEboot(const std::filesystem::path& zar);
	// Called with the directory before every List (for example to cancel a scan part way).
	void SetBeforeList(std::function<void(const std::filesystem::path&)> hook);

	// The arguments of every call, in order.
	[[nodiscard]] const std::vector<std::filesystem::path>& ListCalls() const {
		return m_list_calls;
	}
	[[nodiscard]] const std::vector<std::filesystem::path>& ArchiveCalls() const {
		return m_archive_calls;
	}

	kyty::core::Expected<std::vector<kyty::core::FsEntry>, std::string>
	     List(const std::filesystem::path& dir) override;
	bool ArchiveHasRootEboot(const std::filesystem::path& zar) override;
	std::optional<std::filesystem::path> Canonical(const std::filesystem::path& path) override;

private:
	kyty::core::ScanFileSystem&                       m_inner;
	std::map<std::filesystem::path, std::string>      m_list_failures;
	std::set<std::filesystem::path>                   m_canonical_failures;
	std::set<std::filesystem::path>                   m_archive_failures;
	std::function<void(const std::filesystem::path&)> m_before_list;
	std::vector<std::filesystem::path>                m_list_calls;
	std::vector<std::filesystem::path>                m_archive_calls;
};

class InMemoryScanFileSystem final: public kyty::core::ScanFileSystem {
public:
	// The root "/" exists as a readable directory.
	InMemoryScanFileSystem();
	~InMemoryScanFileSystem() override = default;

	// Each Add takes an absolute path, creates missing parents as directories, and replaces a node
	// that already has that path (a replaced directory keeps its children).
	void AddDirectory(const std::filesystem::path& path);
	void AddFile(const std::filesystem::path& path);
	// A regular file that ArchiveHasRootEboot accepts exactly when has_root_eboot is true.
	void AddArchive(const std::filesystem::path& path, bool has_root_eboot);
	// target is absolute, or relative to the link's directory; it need not exist.
	void AddSymlink(const std::filesystem::path& path, const std::filesystem::path& target);
	// An entry of type Other (a FIFO, socket, or device).
	void AddOther(const std::filesystem::path& path);
	// List(dir) of this directory fails with error; resolving paths through it still works.
	void SetUnreadable(const std::filesystem::path& dir, std::string error = "Permission denied");

	// 0 (the default): every listing is in name order. Otherwise each listing is ordered by a hash
	// of (seed, directory, name), a different permutation per seed and directory.
	void SetListingOrder(uint64_t seed) { m_seed = seed; }

	[[nodiscard]] size_t ListCount() const { return m_list_count; }

	kyty::core::Expected<std::vector<kyty::core::FsEntry>, std::string>
	     List(const std::filesystem::path& dir) override;
	bool ArchiveHasRootEboot(const std::filesystem::path& zar) override;
	std::optional<std::filesystem::path> Canonical(const std::filesystem::path& path) override;

private:
	struct Node {
		enum class Kind { Directory, File, Archive, Symlink, Other } kind = Kind::Directory;
		bool                       archive_has_root_eboot                 = false;
		std::filesystem::path      target;
		std::optional<std::string> unreadable;
		std::set<std::string>      children;
	};

	Node& Put(const std::filesystem::path& path, Node::Kind kind);
	// The resolved path (every link followed, at most 40), or the errno of the failure.
	kyty::core::Expected<std::filesystem::path, int> Resolve(const std::filesystem::path& path,
	                                                         int& links) const;

	std::map<std::filesystem::path, Node> m_nodes;
	uint64_t                              m_seed       = 0;
	size_t                                m_list_count = 0;
};

class InMemoryMetadataReader final: public kyty::core::MetadataReader {
public:
	InMemoryMetadataReader()           = default;
	~InMemoryMetadataReader() override = default;

	// The param.json text of the entry at entry_path (compared in normal form).
	void SetParamJson(const std::filesystem::path& entry_path, std::string bytes);

	// ParseParamJson(text, FallbackTitle(entry_path, kind)), or the fallback metadata when no text
	// is set (a missing param.json).
	[[nodiscard]] kyty::core::GameMetadata Read(const std::filesystem::path& entry_path,
	                                            kyty::core::GameEntry::Kind  kind) const override;

	// Every Read argument, in order.
	[[nodiscard]] const std::vector<std::filesystem::path>& Reads() const { return m_reads; }

private:
	std::map<std::filesystem::path, std::string> m_param_json;
	mutable std::vector<std::filesystem::path>   m_reads;
};

} // namespace KytyTests
