#include "support/tempDir.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

namespace KytyTests {

namespace {

[[noreturn]] void Fail(const std::string& what, const std::error_code& ec) {
	std::fprintf(stderr, "TempDir: %s: %s\n", what.c_str(), ec.message().c_str());
	std::abort();
}

void CreateParents(const std::filesystem::path& path) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	if (ec) {
		Fail("cannot create " + path.parent_path().string(), ec);
	}
}

} // namespace

TempDir::TempDir(std::string_view prefix) {
	std::error_code ec;
	const auto      base = std::filesystem::temp_directory_path(ec);
	if (ec) {
		Fail("no temporary directory", ec);
	}
	std::string pattern = (base / (std::string(prefix) + "_XXXXXX")).string();
	if (mkdtemp(pattern.data()) == nullptr) {
		Fail("mkdtemp " + pattern, std::error_code(errno, std::generic_category()));
	}
	m_path = pattern;
}

TempDir::~TempDir() {
	std::error_code ec;
	std::filesystem::remove_all(m_path, ec);
}

std::filesystem::path TempDir::WriteFile(const std::filesystem::path& relative,
                                         std::string_view             bytes) const {
	const auto path = m_path / relative;
	CreateParents(path);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	out.close();
	if (!out) {
		Fail("cannot write " + path.string(), std::make_error_code(std::errc::io_error));
	}
	return path;
}

std::filesystem::path TempDir::MakeDirectories(const std::filesystem::path& relative) const {
	const auto      path = m_path / relative;
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
	if (ec) {
		Fail("cannot create " + path.string(), ec);
	}
	return path;
}

std::filesystem::path TempDir::MakeSymlink(const std::filesystem::path& target,
                                           const std::filesystem::path& relative_link) const {
	const auto link = m_path / relative_link;
	CreateParents(link);
	std::error_code ec;
	std::filesystem::create_symlink(target, link, ec);
	if (ec) {
		Fail("cannot create the symbolic link " + link.string(), ec);
	}
	return link;
}

} // namespace KytyTests
