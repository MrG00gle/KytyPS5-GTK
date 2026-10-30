// Feature: linux-gtk-modernization, Property 35: ZAR archive round trip
//
// Validates Requirement 11.12. Each case writes a directory tree of at most 20 regular files and
// 64 KiB in total (zero-byte files, nested and empty subdirectories, names with spaces, dots, upper
// and lower case, and non-ASCII UTF-8; names stay unique within a directory when ASCII letters are
// compared without case, because ZArchive compares them so, and avoid '\', which ZArchive treats
// as a separator) and packs it with Common::PackArchive. Reading the archive back through
// Common::File::GetDirEntries and Common::File must give exactly the relative paths of the source's
// regular files with byte-identical contents and every source directory; the progress calls report
// 0 < done <= total with done never decreasing and reaching the total.
//
// Every drawn tree holds at least one byte of file data (a draw without any gets bytes in one of
// its files, or one more file). A known packer-level limitation of Requirement 11.12: for a tree
// without file data (only empty directories or zero-byte files) Common::PackArchive reports success
// but writes an archive that ZArchive cannot open (it has no compressed block). The fix would be
// in src/common (rule 11; open question for the user), so the launcher guards against it instead:
// ArchiveJob verifies the archive and fails with VerifyFailed (orchestrator decision, option 2),
// which tests/launcher_core/ArchiveJobTests.cpp checks for both kinds of data-less trees.

#include "common/archive.h"
#include "common/file.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

constexpr std::size_t kMaxFiles = 20;
constexpr std::size_t kMaxBytes = 64 * 1024;

std::string Name() {
	static const std::vector<std::string> kPieces = {"a",
	                                                 "B",
	                                                 "data",
	                                                 "Save",
	                                                 "eboot",
	                                                 ".bin",
	                                                 ".",
	                                                 " ",
	                                                 "-",
	                                                 "_",
	                                                 "(1)",
	                                                 "\xC3\xA9",
	                                                 "\xC3\x9C",
	                                                 "\xE6\x97\xA5\xE6\x9C\xAC",
	                                                 "\xF0\x9F\x8E\xAE",
	                                                 "\xD0\x96",
	                                                 "#",
	                                                 "~"};
	std::string                           name;
	do {
		name.clear();
		const int count = Choose(1, 5);
		for (int i = 0; i < count; i++) {
			name += kPieces[Choose<std::size_t>(0, kPieces.size())];
		}
	} while (name == "." || name == "..");
	return name;
}

std::string Folded(std::string name) {
	for (auto& c: name) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return name;
}

std::string RandomBytes(std::size_t size) {
	std::string content;
	for (std::size_t b = 0; b < size; b++) {
		content += static_cast<char>(Choose(0, 256));
	}
	return content;
}

struct Tree {
	std::map<std::string, std::string> files; // generic relative path -> bytes
	std::set<std::string>              directories;
};

void Fill(const fs::path& relative, int depth, Tree& tree, std::size_t& bytes) {
	std::set<std::string> folded;
	const int             count = Choose(0, 6);
	for (int i = 0; i < count && tree.files.size() < kMaxFiles; i++) {
		const auto name = Name();
		if (!folded.insert(Folded(name)).second) {
			continue;
		}
		const auto path = relative / name;
		if (depth < 3 && OneIn(3)) {
			tree.directories.insert(path.generic_string());
			Fill(path, depth + 1, tree, bytes);
			continue;
		}
		const std::size_t room = kMaxBytes - bytes;
		const std::size_t size =
		    OneIn(4) || room == 0 ? 0
		                          : Choose<std::size_t>(1, std::min<std::size_t>(room, 8192) + 1);
		bytes += size;
		tree.files[path.generic_string()] = RandomBytes(size);
	}
}

// Gives a tree without file data some: bytes for one of its (zero-byte) files, or, when it has no
// file, one more file at the root, named with a character that Name() never uses so it cannot
// collide with a drawn name. True when the tree was changed.
bool EnsureFileData(Tree& tree, std::size_t& bytes) {
	if (bytes != 0) {
		return false;
	}
	const std::size_t size = Choose<std::size_t>(1, 4097);
	if (tree.files.empty()) {
		tree.files["\xE2\x98\x85payload.bin"] = RandomBytes(size);
	} else {
		auto file = tree.files.begin();
		std::advance(file, Choose<std::size_t>(0, tree.files.size()));
		file->second = RandomBytes(size);
	}
	bytes = size;
	return true;
}

// True when some directory of the tree has no entry.
bool HasEmptyDirectory(const Tree& tree) {
	for (const auto& directory: tree.directories) {
		const auto prefix = directory + "/";
		const auto inside = [&prefix](const std::string& path) { return path.starts_with(prefix); };
		if (std::none_of(tree.directories.begin(), tree.directories.end(), inside) &&
		    std::none_of(tree.files.begin(), tree.files.end(),
		                 [&inside](const auto& file) { return inside(file.first); })) {
			return true;
		}
	}
	return false;
}

void ReadBack(const fs::path& archive, const std::string& relative, Tree& tree) {
	const auto directory = Common::MakeArchivePath(archive, relative);
	for (const auto& entry: Common::File::GetDirEntries(directory)) {
		const auto path = relative.empty() ? entry.name : relative + "/" + entry.name;
		if (!entry.is_file) {
			tree.directories.insert(path);
			ReadBack(archive, path, tree);
			continue;
		}
		Common::File file(directory / entry.name, Common::File::Mode::Read);
		RC_ASSERT(!file.IsInvalid());
		const auto data  = file.ReadWholeBuffer();
		tree.files[path] = std::string(reinterpret_cast<const char*>(data.data()), data.size());
	}
}

} // namespace

KYTY_PROPERTY(35, "ZAR archive round trip") {
	return rc::check(name, []() {
		Tree        source;
		std::size_t bytes = 0;
		Fill("", 0, source, bytes);
		const bool data_added = EnsureFileData(source, bytes);

		const KytyTests::TempDir temp("kyty_p35");
		temp.MakeDirectories("source");
		for (const auto& directory: source.directories) {
			temp.MakeDirectories(fs::path("source") / directory);
		}
		std::uint64_t total = 0;
		for (const auto& [path, content]: source.files) {
			temp.WriteFile(fs::path("source") / path, content);
			total += content.size();
		}
		RC_LOG() << source.files.size() << " files, " << source.directories.size()
		         << " directories, " << bytes << " bytes\n";

		const auto    archive = temp.Path() / "packed.zar";
		std::uint64_t last    = 0;
		bool          ordered = true;
		RC_ASSERT(Common::PackArchive(
		    temp.Path() / "source", archive, [&](std::uint64_t done, std::uint64_t all) {
			    ordered = ordered && done > 0 && done <= all && all == total && done >= last;
			    last    = done;
			    return true;
		    }));
		RC_ASSERT(ordered);
		RC_ASSERT(total > 0U);
		RC_ASSERT(last == total);
		RC_ASSERT(Common::OpenArchive(archive) != nullptr);

		Tree read;
		ReadBack(archive, "", read);
		RC_ASSERT(read.files == source.files);
		RC_ASSERT(read.directories == source.directories);

		RC_CLASSIFY(data_added, "data added to a data-less draw");
		RC_CLASSIFY(!source.directories.empty(), "subdirectories");
		RC_CLASSIFY(HasEmptyDirectory(source), "empty subdirectories");
		bool zero_byte = false;
		for (const auto& [path, content]: source.files) {
			zero_byte = zero_byte || content.empty();
		}
		RC_CLASSIFY(zero_byte, "zero-byte files");
	});
}
