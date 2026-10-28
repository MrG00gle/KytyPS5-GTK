#pragma once

// A temporary directory for a test: created with mkdtemp() under
// std::filesystem::temp_directory_path() and removed with everything in it by the destructor. The
// helpers abort the test with a message on failure; they never throw.

#include <filesystem>
#include <string_view>

namespace KytyTests {

class TempDir {
public:
	explicit TempDir(std::string_view prefix = "kyty_test");
	~TempDir();

	TempDir(const TempDir&)            = delete;
	TempDir& operator=(const TempDir&) = delete;

	[[nodiscard]] const std::filesystem::path& Path() const { return m_path; }

	// Writes bytes to Path() / relative, creating the parent directories; returns the full path.
	std::filesystem::path WriteFile(const std::filesystem::path& relative,
	                                std::string_view             bytes) const;

	// Creates Path() / relative and its parents; returns the full path.
	std::filesystem::path MakeDirectories(const std::filesystem::path& relative) const;

	// Creates the symbolic link Path() / relative_link with the text target (relative or absolute,
	// it need not exist), creating the link's parent directories; returns the link's full path.
	std::filesystem::path MakeSymlink(const std::filesystem::path& target,
	                                  const std::filesystem::path& relative_link) const;

private:
	std::filesystem::path m_path;
};

} // namespace KytyTests
