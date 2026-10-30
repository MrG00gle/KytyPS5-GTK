#include "installFileOps.h"

namespace Kyty::Updater {
namespace {
namespace fs = std::filesystem;

class FileSystemOps final: public InstallFileOps {
public:
	void Rename(const fs::path& from, const fs::path& to) override { fs::rename(from, to); }
	bool CreateDirectory(const fs::path& path) override { return fs::create_directory(path); }
	void CreateDirectories(const fs::path& path) override { fs::create_directories(path); }
	void Remove(const fs::path& path) override { fs::remove(path); }
	void RemoveAll(const fs::path& path) override { fs::remove_all(path); }
};
} // namespace

InstallFileOps& DefaultFileOps() {
	static FileSystemOps ops;
	return ops;
}
} // namespace Kyty::Updater
