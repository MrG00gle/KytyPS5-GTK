#include "launcher/core/saveData.h"

#include "launcher/core/dataPaths.h"
#include "launcher/core/titleId.h"

#include <algorithm>
#include <utility>

namespace kyty::core {

namespace {

namespace fs = std::filesystem;

class HostRemover final: public PathRemover {
public:
	std::error_code Remove(const fs::path& path) override {
		std::error_code ec;
		fs::remove(path, ec);
		return ec;
	}
};

bool IsOneComponent(std::string_view name) {
	return !name.empty() && name != "." && name != ".." &&
	       name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

void RemoveEntry(const fs::path& path, bool directory, PathRemover& remover,
                 std::vector<RemovalFailure>& failures);

// Removes the entries of directory, deepest first. False when it could not be listed: the failure
// is recorded, unless the directory no longer exists, and the directory itself must not be removed.
bool RemoveContents(const fs::path& directory, PathRemover& remover,
                    std::vector<RemovalFailure>& failures) {
	// List first, then remove, so the removals do not disturb the iteration.
	std::vector<std::pair<fs::path, bool>> children;
	std::error_code                        ec;
	for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code status_ec;
		children.emplace_back(it->path(),
		                      it->symlink_status(status_ec).type() == fs::file_type::directory);
	}
	if (ec) {
		if (ec != std::errc::no_such_file_or_directory) {
			failures.push_back({directory, ec});
		}
		return false;
	}
	std::sort(children.begin(), children.end());
	for (const auto& [child, child_is_directory]: children) {
		RemoveEntry(child, child_is_directory, remover, failures);
	}
	return true;
}

// Removes path; a directory (not a link to one) after its contents.
void RemoveEntry(const fs::path& path, bool directory, PathRemover& remover,
                 std::vector<RemovalFailure>& failures) {
	if (directory && !RemoveContents(path, remover, failures)) {
		return;
	}
	if (const auto ec = remover.Remove(path)) {
		failures.push_back({path, ec});
	}
}

} // namespace

PathRemover& HostPathRemover() {
	static HostRemover remover;
	return remover;
}

Expected<fs::path, std::string> SaveDataPath(const fs::path& data_dir, std::string_view title_id) {
	auto title = NormalizeTitleId(title_id);
	if (!IsOneComponent(title)) {
		return MakeUnexpected("The title ID \"" + title +
		                      "\" does not name a save data directory.");
	}
	return SaveDataDirectory(data_dir) / title;
}

Expected<std::vector<fs::path>, std::string> ListSaveData(const fs::path&  data_dir,
                                                          std::string_view title_id) {
	auto directory = SaveDataPath(data_dir, title_id);
	if (!directory) {
		return MakeUnexpected(std::move(directory).error());
	}
	std::vector<fs::path> directories;
	std::error_code       ec;
	if (fs::is_directory(*directory, ec)) {
		directories.push_back(std::move(*directory));
	}
	return directories;
}

Expected<SaveDataRemoval, std::string>
RemoveSaveData(const fs::path& data_dir, std::string_view title_id, PathRemover& remover) {
	auto directory = SaveDataPath(data_dir, title_id);
	if (!directory) {
		return MakeUnexpected(std::move(directory).error());
	}
	SaveDataRemoval removal;
	removal.directory = std::move(*directory);
	std::error_code ec;
	const auto      type = fs::symlink_status(removal.directory, ec).type();
	if (type == fs::file_type::not_found) {
		return removal;
	}
	if (ec) {
		removal.failures.push_back({removal.directory, ec});
		return removal;
	}
	RemoveEntry(removal.directory, type == fs::file_type::directory, remover, removal.failures);
	return removal;
}

} // namespace kyty::core
