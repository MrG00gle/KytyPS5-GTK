// Example tests of WriteFileAtomically and ReadFileLimited (src/launcher/core/atomicFile.h,
// Requirements 9.15 and 12.8). Failures are injected through SetAtomicFileFaultForTesting or come
// from paths that fail for every user (a missing directory, a directory as the target); no test
// changes permissions to provoke a failure. The modes of new files are checked under umask 077,
// 022, and 002 (plan D28); every test that depends on the umask sets it and restores it.

#include "launcher/core/atomicFile.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::core::AtomicFileStep;
using kyty::core::ReadFileLimited;
using kyty::core::SetAtomicFileFaultForTesting;
using kyty::core::WriteFileAtomically;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "AtomicFileTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ReadAll(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	Check(static_cast<bool>(in), "open " + path.string());
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::set<std::string> Names(const fs::path& directory) {
	std::set<std::string> names;
	std::error_code       ec;
	for (const auto& entry: fs::directory_iterator(directory, ec)) {
		names.insert(entry.path().filename().string());
	}
	Check(!ec, "list " + directory.string());
	return names;
}

unsigned Mode(const fs::path& path) {
	struct stat info {};
	Check(::stat(path.c_str(), &info) == 0, "stat " + path.string());
	return info.st_mode & 0777;
}

// Sets the process umask and restores the previous one when it goes out of scope.
class UmaskScope {
public:
	explicit UmaskScope(mode_t mask): m_previous(::umask(mask)) {}
	~UmaskScope() { ::umask(m_previous); }
	UmaskScope(const UmaskScope&)            = delete;
	UmaskScope& operator=(const UmaskScope&) = delete;

private:
	mode_t m_previous;
};

mode_t CurrentUmask() {
	const mode_t mask = ::umask(0);
	::umask(mask);
	return mask;
}

std::string Octal(unsigned mode) {
	char text[8];
	std::snprintf(text, sizeof(text), "%04o", mode);
	return text;
}

// True for ".<name>.tmp-" followed by 16 lower-case hexadecimal digits.
bool IsTempName(const std::string& file, const std::string& name) {
	const std::string prefix = "." + name + ".tmp-";
	return file.size() == prefix.size() + 16 && file.starts_with(prefix) &&
	       file.find_first_not_of("0123456789abcdef", prefix.size()) == std::string::npos;
}

const char* StepName(AtomicFileStep step) {
	switch (step) {
		case AtomicFileStep::CreateTemp: return "CreateTemp";
		case AtomicFileStep::Write: return "Write";
		case AtomicFileStep::Sync: return "Sync";
		case AtomicFileStep::Close: return "Close";
		case AtomicFileStep::Rename: return "Rename";
	}
	return "?";
}

void TestNewFile() {
	const TempDir    dir;
	const UmaskScope umask_022(022);
	const auto       target = dir.Path() / "new.txt";
	const auto       result = WriteFileAtomically(target, "hello");
	Check(result.has_value(), "writing a new file succeeds");
	Check(ReadAll(target) == "hello", "the new file has the bytes");
	Check(Mode(target) == 0644, "a new file gets mode 0644 under umask 022");
	Check(Names(dir.Path()) == std::set<std::string> {"new.txt"}, "no temporary file is left");

	const auto empty = dir.Path() / "empty.txt";
	Check(WriteFileAtomically(empty, "").has_value(), "writing an empty file succeeds");
	Check(fs::file_size(empty) == 0, "the empty file is empty");

	std::string large(3 * 1024 * 1024 + 17, '\0');
	for (size_t i = 0; i < large.size(); i++) {
		large[i] = static_cast<char>(i * 31 + 7);
	}
	const auto large_path = dir.Path() / "large.bin";
	Check(WriteFileAtomically(large_path, large).has_value(), "writing a large file succeeds");
	Check(ReadAll(large_path) == large, "the large file has every byte");
}

void TestReplace() {
	const TempDir dir;
	const auto    target = dir.WriteFile("settings.ini", "old contents that are longer");
	Check(WriteFileAtomically(target, "new").has_value(), "replacing a file succeeds");
	Check(ReadAll(target) == "new", "the replaced file has only the new bytes");
	Check(Names(dir.Path()) == std::set<std::string> {"settings.ini"}, "no temporary file is left");
}

void TestModeKept() {
	const TempDir   dir;
	const auto      target = dir.WriteFile("private.ini", "old");
	std::error_code ec;
	fs::permissions(target, fs::perms(0640), ec);
	Check(!ec && Mode(target) == 0640, "set mode 0640");
	Check(WriteFileAtomically(target, "new").has_value(), "replacing a 0640 file succeeds");
	Check(Mode(target) == 0640, "the replaced file keeps mode 0640");
	fs::permissions(target, fs::perms(0600), ec);
	Check(WriteFileAtomically(target, "newer").has_value(), "replacing a 0600 file succeeds");
	Check(Mode(target) == 0600 && ReadAll(target) == "newer", "the replaced file keeps mode 0600");
}

// Plan D28 (Y3): a new file gets 0666 minus the umask, a replaced file keeps its mode, and the
// temporary file is .<name>.tmp-<16 hexadecimal digits> with the final mode before the rename.
void TestUmaskModes() {
	const mode_t original = CurrentUmask();
	const struct {
		mode_t   mask;
		unsigned mode;
	} cases[] = {{077, 0600}, {022, 0644}, {002, 0664}};
	for (const auto& c: cases) {
		const std::string what = "umask " + Octal(c.mask);
		const TempDir     dir;
		const UmaskScope  scope(c.mask);
		const auto        target = dir.Path() / "new.ini";

		// The temporary file, seen from the hook just before the rename.
		std::vector<std::string> temp_names;
		unsigned                 temp_mode = 0;
		SetAtomicFileFaultForTesting([&](AtomicFileStep current) {
			if (current == AtomicFileStep::Rename) {
				for (const auto& name: Names(dir.Path())) {
					temp_names.push_back(name);
					temp_mode = Mode(dir.Path() / name);
				}
			}
			return 0;
		});
		const auto result = WriteFileAtomically(target, "new");
		SetAtomicFileFaultForTesting(nullptr);
		Check(result.has_value(), what + ": writing a new file succeeds");
		Check(Mode(target) == c.mode,
		      what + ": a new file gets " + Octal(c.mode) + ", got " + Octal(Mode(target)));
		Check(temp_names.size() == 1 && IsTempName(temp_names[0], "new.ini"),
		      what + ": one temporary file .new.ini.tmp-<16 hex digits>, got " +
		          (temp_names.empty() ? std::string("none") : temp_names[0]));
		Check(temp_mode == c.mode, what + ": the temporary file has the final mode");
		Check(Names(dir.Path()) == std::set<std::string> {"new.ini"},
		      what + ": no temporary file is left");

		// An existing 0640 file keeps 0640.
		std::error_code ec;
		fs::permissions(target, fs::perms(0640), ec);
		Check(!ec, what + ": set mode 0640");
		Check(WriteFileAtomically(target, "replaced").has_value(), what + ": replace");
		Check(Mode(target) == 0640 && ReadAll(target) == "replaced",
		      what + ": the replaced file keeps 0640, got " + Octal(Mode(target)));

		// A fault at any step leaves the target and its mode, and no temporary file.
		for (const auto step:
		     {AtomicFileStep::CreateTemp, AtomicFileStep::Write, AtomicFileStep::Sync,
		      AtomicFileStep::Close, AtomicFileStep::Rename}) {
			SetAtomicFileFaultForTesting(
			    [step](AtomicFileStep current) { return current == step ? EIO : 0; });
			const auto failed  = WriteFileAtomically(target, "lost");
			const auto created = WriteFileAtomically(dir.Path() / "other.ini", "lost");
			SetAtomicFileFaultForTesting(nullptr);
			const std::string at = what + ", a fault at " + StepName(step);
			Check(!failed.has_value() && !created.has_value(), at + ": the writes fail");
			Check(ReadAll(target) == "replaced" && Mode(target) == 0640,
			      at + ": the target and its mode are unchanged");
			Check(Names(dir.Path()) == std::set<std::string> {"new.ini"},
			      at + ": no temporary file and no new file are left");
		}
	}
	Check(CurrentUmask() == original, "the umask is restored");
}

void TestSymlinkWrittenThrough() {
	const TempDir dir;
	const auto    real = dir.WriteFile("data/real.ini", "old");
	const auto    link = dir.MakeSymlink("data/real.ini", "link.ini");
	Check(WriteFileAtomically(link, "through the link").has_value(),
	      "writing through a link succeeds");
	Check(fs::is_symlink(link) && fs::read_symlink(link) == "data/real.ini", "the link stays");
	Check(ReadAll(real) == "through the link", "the file the link names is replaced");
	Check(Names(dir.Path() / "data") == std::set<std::string> {"real.ini"},
	      "no temporary file is left next to the real file");

	// A chain: absolute link -> relative link in another directory -> file.
	const auto inner = dir.MakeSymlink("../data/real.ini", "links/inner.ini");
	const auto outer = dir.MakeSymlink(inner, "outer.ini");
	Check(WriteFileAtomically(outer, "chain").has_value(), "writing through a chain succeeds");
	Check(fs::is_symlink(outer) && fs::is_symlink(inner), "both links stay");
	Check(ReadAll(real) == "chain", "the chain's final file is replaced");

	// A dangling link creates the file it names.
	const auto dangling = dir.MakeSymlink("created.ini", "dangling.ini");
	Check(WriteFileAtomically(dangling, "created").has_value(), "writing through a dangling link");
	Check(fs::is_symlink(dangling) && ReadAll(dir.Path() / "created.ini") == "created",
	      "the dangling link's file is created");

	// A loop cannot be resolved.
	dir.MakeSymlink("loop-b", "loop-a");
	dir.MakeSymlink("loop-a", "loop-b");
	const auto loop = WriteFileAtomically(dir.Path() / "loop-a", "x");
	Check(!loop.has_value() && loop.error().error_number == ELOOP, "a symbolic link loop is ELOOP");
	Check(loop.error().path == dir.Path() / "loop-a", "the error names the target");
}

void TestRelativeTarget() {
	const TempDir   dir;
	std::error_code ec;
	const auto      previous = fs::current_path(ec);
	Check(!ec, "current_path");
	fs::current_path(dir.Path(), ec);
	Check(!ec, "chdir to the temporary directory");
	const auto result = WriteFileAtomically("relative.txt", "relative");
	fs::current_path(previous, ec);
	Check(!ec, "chdir back");
	Check(result.has_value(),
	      "a target without a directory part is written in the working directory");
	Check(ReadAll(dir.Path() / "relative.txt") == "relative", "the relative target has the bytes");
}

void TestNoFileName() {
	const TempDir dir;
	const auto    result = WriteFileAtomically(dir.Path() / "", "x");
	Check(!result.has_value() && result.error().error_number == EISDIR,
	      "a path without a file name is EISDIR");
	Check(Names(dir.Path()).empty(), "nothing is created");
}

void TestFaultAtEachStep() {
	const AtomicFileStep steps[] = {AtomicFileStep::CreateTemp, AtomicFileStep::Write,
	                                AtomicFileStep::Sync, AtomicFileStep::Close,
	                                AtomicFileStep::Rename};
	for (const auto step: steps) {
		const std::string name = StepName(step);
		const TempDir     dir;
		const auto        target = dir.WriteFile("settings.ini", "previous bytes");
		std::error_code   ec;
		fs::permissions(target, fs::perms(0640), ec);
		std::vector<AtomicFileStep> seen;
		SetAtomicFileFaultForTesting([&](AtomicFileStep current) {
			seen.push_back(current);
			return current == step ? ENOSPC : 0;
		});
		const auto result = WriteFileAtomically(target, "replacement bytes");
		SetAtomicFileFaultForTesting(nullptr);

		Check(!result.has_value(), name + ": the write fails");
		Check(result.error().error_number == ENOSPC, name + ": the injected errno is reported");
		Check(result.error().path == target, name + ": the error names the target");
		Check(result.error().message.find(std::generic_category().message(ENOSPC)) !=
		          std::string::npos,
		      name + ": the message has the error text: " + result.error().message);
		Check(!seen.empty() && seen.back() == step, name + ": the steps stop at the fault");
		Check(ReadAll(target) == "previous bytes", name + ": the target is unchanged");
		Check(Mode(target) == 0640, name + ": the target's mode is unchanged");
		Check(Names(dir.Path()) == std::set<std::string> {"settings.ini"},
		      name + ": the temporary file is removed");

		// The same fault when there is no target yet leaves no file at all.
		const auto missing = dir.Path() / "missing.ini";
		SetAtomicFileFaultForTesting(
		    [step](AtomicFileStep current) { return current == step ? EIO : 0; });
		const auto missing_result = WriteFileAtomically(missing, "x");
		SetAtomicFileFaultForTesting(nullptr);
		Check(!missing_result.has_value() && missing_result.error().error_number == EIO,
		      name + ": a new file fails too");
		Check(!fs::exists(missing), name + ": no new file is created");
		Check(Names(dir.Path()) == std::set<std::string> {"settings.ini"},
		      name + ": no temporary file is left for a new file");
	}

	// Without a fault the hook sees every step once, in order.
	const TempDir               dir;
	std::vector<AtomicFileStep> seen;
	SetAtomicFileFaultForTesting([&](AtomicFileStep current) {
		seen.push_back(current);
		return 0;
	});
	const auto result = WriteFileAtomically(dir.Path() / "ok.ini", "ok");
	SetAtomicFileFaultForTesting(nullptr);
	Check(result.has_value(), "a hook returning 0 injects nothing");
	Check(seen == std::vector<AtomicFileStep>(std::begin(steps), std::end(steps)),
	      "the hook sees CreateTemp, Write, Sync, Close, Rename");
}

void TestRealFailures() {
	const TempDir dir;
	const auto    no_directory = WriteFileAtomically(dir.Path() / "missing" / "file.ini", "x");
	Check(!no_directory.has_value() && no_directory.error().error_number == ENOENT,
	      "a missing directory fails at CreateTemp with ENOENT");
	Check(no_directory.error().message.find("temporary file") != std::string::npos,
	      "the message names the step: " + no_directory.error().message);

	const auto directory = dir.MakeDirectories("target-is-a-directory");
	dir.WriteFile("target-is-a-directory/inside.txt", "kept");
	const auto onto_directory = WriteFileAtomically(directory, "x");
	Check(!onto_directory.has_value() && onto_directory.error().error_number == EISDIR,
	      "renaming onto a directory fails with EISDIR");
	Check(onto_directory.error().message.find("rename") != std::string::npos,
	      "the message names the rename: " + onto_directory.error().message);
	Check(fs::is_directory(directory) && ReadAll(directory / "inside.txt") == "kept",
	      "the directory is unchanged");
	Check(Names(dir.Path()) == std::set<std::string> {"target-is-a-directory"},
	      "the temporary file is removed after a failed rename");
}

void TestReadFileLimited() {
	const TempDir dir;
	const auto    ten = dir.WriteFile("ten.bin", "0123456789");
	const auto    at  = ReadFileLimited(ten, 10);
	Check(at.has_value() && *at == "0123456789", "a file at the limit is read");
	const auto over = ReadFileLimited(ten, 9);
	Check(!over.has_value() && over.error().error_number == EFBIG,
	      "a file over the limit is EFBIG");
	Check(over.error().path == ten, "the error names the file");
	Check(over.error().message.find("limit 9 bytes") != std::string::npos,
	      "the message names the limit: " + over.error().message);
	Check(ReadFileLimited(ten, UINT64_MAX).value_or("") == "0123456789", "no practical limit");

	const auto empty = dir.WriteFile("empty.bin", "");
	Check(ReadFileLimited(empty, 0).value_or("x").empty(), "an empty file with limit 0");
	const auto one = dir.WriteFile("one.bin", "1");
	Check(ReadFileLimited(one, 0).error().error_number == EFBIG, "one byte over limit 0");

	std::string large(200000, 'L');
	large[123456]         = '\0';
	const auto large_path = dir.WriteFile("large.bin", large);
	Check(ReadFileLimited(large_path, large.size()).value_or("") == large,
	      "a file larger than the read buffer is read whole");

	const auto missing = ReadFileLimited(dir.Path() / "missing.bin", 100);
	Check(!missing.has_value() && missing.error().error_number == ENOENT,
	      "a missing file is ENOENT");
	Check(missing.error().path == dir.Path() / "missing.bin", "the error names the missing file");

	const auto directory = ReadFileLimited(dir.Path(), 100);
	Check(!directory.has_value() && directory.error().error_number == EISDIR,
	      "a directory is EISDIR");

	// Character devices have no size: the limit applies while reading.
	Check(ReadFileLimited("/dev/null", 10).value_or("x").empty(), "/dev/null reads as empty");
	const auto endless = ReadFileLimited("/dev/zero", 100000);
	Check(!endless.has_value() && endless.error().error_number == EFBIG,
	      "an endless stream stops at the limit with EFBIG");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("AtomicFileTests");
	TestNewFile();
	TestReplace();
	TestModeKept();
	TestUmaskModes();
	TestSymlinkWrittenThrough();
	TestRelativeTarget();
	TestNoFileName();
	TestFaultAtEachStep();
	TestRealFailures();
	TestReadFileLimited();
	std::printf("AtomicFileTests: all tests passed\n");
	return 0;
}
