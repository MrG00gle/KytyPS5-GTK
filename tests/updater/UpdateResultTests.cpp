// Example tests of kyty::updater::WriteUpdateResult and ReadAndClearPreviousResult
// (src/updater/core/updateResult.h, task 17.1; Requirement 13.14): the file format, the 64 KiB cut
// at a UTF-8 boundary, a symbolic link, a directory, or a FIFO at the path, malformed and oversized
// files, a second read that finds nothing, write failures that leave no temporary file, and
// results replacing older ones.

#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/updateResult.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::updater::PreviousResult;
using kyty::updater::ReadAndClearPreviousResult;
using kyty::updater::WriteUpdateResult;
using KytyTests::TempDir;

constexpr std::string_view kUnreadable = "The previous update result could not be read: ";
constexpr size_t           kLimit      = kyty::updater::kMaxUpdateResultMessageBytes;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "UpdateResultTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

fs::path ResultPath(const TempDir& temp) {
	return temp.Path() / kyty::updater::kUpdateResultFileName;
}

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
}

// The names in directory other than the result file itself: temporary files must not stay.
std::vector<std::string> OtherEntries(const fs::path& directory) {
	std::vector<std::string> names;
	std::error_code          ec;
	for (auto it = fs::directory_iterator(directory, ec); !ec && it != fs::directory_iterator();
	     it.increment(ec)) {
		const auto name = it->path().filename().string();
		if (name != kyty::updater::kUpdateResultFileName) {
			names.push_back(name);
		}
	}
	return names;
}

void Write(const TempDir& temp, const PreviousResult& result, const std::string& name) {
	const auto written = WriteUpdateResult(temp.Path(), result);
	Check(written.has_value(),
	      name + ": written (" + (written ? std::string() : written.error()) + ")");
}

// Reads once (expecting expected) and again (expecting nothing), and checks the path is gone.
void CheckReadOnce(const TempDir& temp, const std::optional<PreviousResult>& expected,
                   const std::string& name) {
	const auto first = ReadAndClearPreviousResult(temp.Path());
	Check(first.has_value() == expected.has_value(), name + ": a result exactly when expected");
	if (expected) {
		Check(first->success == expected->success, name + ": status");
		Check(first->message == expected->message,
		      name + ": message '" + first->message.substr(0, 200) + "', expected '" +
		          expected->message.substr(0, 200) + "'");
	}
	Check(!Exists(ResultPath(temp)), name + ": nothing stays at the path");
	Check(!ReadAndClearPreviousResult(temp.Path()).has_value(),
	      name + ": the second read is empty");
}

PreviousResult Unreadable(const std::string& reason) {
	return PreviousResult {false, std::string(kUnreadable) + reason};
}

void TestFormat() {
	const TempDir                                             temp("kyty_result");
	const std::vector<std::pair<PreviousResult, std::string>> cases = {
	    {{true, "Installed 2026-10-30-8d525f7."}, "success\nInstalled 2026-10-30-8d525f7."},
	    {{false, "Install failed:\ncannot rename a to b"},
	     "failure\nInstall failed:\ncannot rename a to b"},
	    {{true, ""}, "success\n"},
	    {{false, ""}, "failure\n"},
	    {{false, "trailing\n"}, "failure\ntrailing\n"},
	    {{true, std::string("nul\0byte", 8)}, std::string("success\nnul\0byte", 16)},
	    {{false, "\xC3\xA9t\xC3\xA9 \xF0\x9F\x98\x80"},
	     "failure\n\xC3\xA9t\xC3\xA9 \xF0\x9F\x98\x80"},
	};
	for (const auto& [result, bytes]: cases) {
		const std::string name = "format of '" + result.message + "'";
		Write(temp, result, name);
		Check(UpdaterGen::ReadFile(ResultPath(temp)) == bytes, name + ": file bytes");
		Check(OtherEntries(temp.Path()).empty(), name + ": no temporary file stays");
		CheckReadOnce(temp, result, name);
	}

	// A newer result replaces an older one.
	Write(temp, {false, "old"}, "old");
	Write(temp, {true, "new"}, "new");
	CheckReadOnce(temp, PreviousResult {true, "new"}, "replaced");

	// Nothing there, no installation directory, or a file instead of it.
	CheckReadOnce(temp, std::nullopt, "no result");
	Check(!ReadAndClearPreviousResult(temp.Path() / "missing").has_value(), "missing directory");
	const auto file = temp.WriteFile("file", "x");
	Check(!ReadAndClearPreviousResult(file).has_value(), "a file instead of the directory");
}

void TestCut() {
	const TempDir     temp("kyty_result");
	const std::string a(kLimit, 'a');
	struct Case {
		std::string message;
		size_t      kept; // bytes of message that stay
		const char* name;
	};
	const std::vector<Case> cases = {
	    {a, kLimit, "exactly 64 KiB"},
	    {a + "b", kLimit, "one byte over"},
	    {std::string(kLimit - 1, 'a') + "\xC3\xA9", kLimit - 1, "a 2-byte character across"},
	    {std::string(kLimit - 2, 'a') + "\xE2\x82\xAC", kLimit - 2, "a 3-byte character across"},
	    {std::string(kLimit - 1, 'a') + "\xE2\x82\xAC", kLimit - 1, "a 3-byte character after one"},
	    {std::string(kLimit - 3, 'a') + "\xF0\x9F\x98\x80", kLimit - 3,
	     "a 4-byte character across"},
	    {std::string(kLimit - 1, 'a') + "\xF0\x9F\x98\x80", kLimit - 1, "a 4-byte character late"},
	    {std::string(kLimit - 4, 'a') + "\xF0\x9F\x98\x80" + "z", kLimit,
	     "a 4-byte character at the end"},
	    {std::string(kLimit - 2, 'a') + "\xC3\xA9" + "z", kLimit, "a 2-byte character at the end"},
	    // Not UTF-8: at most three continuation bytes are dropped.
	    {std::string(kLimit - 6, 'a') + std::string(10, '\x80'), kLimit - 3, "continuation bytes"},
	    {UpdaterGen::PatternBytes(1 << 20, 9), 0, "1 MiB of arbitrary bytes"},
	};
	for (const auto& test: cases) {
		Write(temp, {false, test.message}, test.name);
		const auto result = ReadAndClearPreviousResult(temp.Path());
		Check(result.has_value() && !result->success, std::string(test.name) + ": read back");
		if (test.kept > 0) {
			Check(result->message == test.message.substr(0, test.kept),
			      std::string(test.name) + ": " + std::to_string(result->message.size()) +
			          " bytes kept, expected " + std::to_string(test.kept));
		} else {
			Check(result->message.size() <= kLimit && result->message.size() >= kLimit - 3 &&
			          test.message.starts_with(result->message),
			      std::string(test.name) + ": a prefix of at most 64 KiB");
		}
		Check(UpdaterGen::EndsAtUtf8Boundary(result->message) || test.kept == 0 ||
		          std::string_view(test.name) == "continuation bytes",
		      std::string(test.name) + ": ends at a UTF-8 boundary");
	}
}

void TestMalformedFiles() {
	const TempDir     temp("kyty_result");
	const std::string first_line = "its first line is not \"success\" or \"failure\"";
	const std::vector<std::pair<std::string, PreviousResult>> cases = {
	    {"", Unreadable(first_line)},
	    {"success", Unreadable(first_line)},
	    {"failure", Unreadable(first_line)},
	    {"SUCCESS\nok", Unreadable(first_line)},
	    {"success\r\nok", Unreadable(first_line)},
	    {" success\nok", Unreadable(first_line)},
	    {"successful\nok", Unreadable(first_line)},
	    {"done\nok", Unreadable(first_line)},
	    {"\nsuccess\n", Unreadable(first_line)},
	    {"success\n" + std::string(kLimit, 'm'), PreviousResult {true, std::string(kLimit, 'm')}},
	    {"failure\n" + std::string(kLimit + 1, 'm'), Unreadable("it is larger than 65544 bytes")},
	    {"success\n" + std::string(3 * kLimit, 'm'), Unreadable("it is larger than 65544 bytes")},
	};
	for (const auto& [bytes, expected]: cases) {
		temp.WriteFile(kyty::updater::kUpdateResultFileName, bytes);
		CheckReadOnce(temp, expected,
		              "file '" + bytes.substr(0, 20) + "' (" + std::to_string(bytes.size()) +
		                  " bytes)");
	}
}

void TestPathKinds() {
	const TempDir temp("kyty_result");
	const auto    path = ResultPath(temp);

	// A symbolic link is never followed: reading deletes the link, writing replaces it.
	const auto target = temp.WriteFile("target", "success\nfrom the link target");
	temp.MakeSymlink(target, kyty::updater::kUpdateResultFileName);
	CheckReadOnce(temp, Unreadable("it is a symbolic link"), "a link to a result file");
	Check(UpdaterGen::ReadFile(target) == "success\nfrom the link target", "the link target stays");
	temp.MakeSymlink("missing", kyty::updater::kUpdateResultFileName);
	CheckReadOnce(temp, Unreadable("it is a symbolic link"), "a dangling link");
	temp.MakeSymlink(target, kyty::updater::kUpdateResultFileName);
	Write(temp, {true, "written over a link"}, "over a link");
	std::error_code ec;
	Check(fs::symlink_status(path, ec).type() == fs::file_type::regular, "the link is replaced");
	Check(UpdaterGen::ReadFile(target) == "success\nfrom the link target",
	      "writing never goes through the link");
	CheckReadOnce(temp, PreviousResult {true, "written over a link"}, "after replacing a link");

	// An empty directory is removed by both; a non-empty one stays and is reported every time.
	temp.MakeDirectories(kyty::updater::kUpdateResultFileName);
	CheckReadOnce(temp, Unreadable("it is not a regular file"), "an empty directory");
	temp.MakeDirectories(kyty::updater::kUpdateResultFileName);
	Write(temp, {false, "written over a directory"}, "over an empty directory");
	CheckReadOnce(temp, PreviousResult {false, "written over a directory"}, "after a directory");
	temp.WriteFile(fs::path(kyty::updater::kUpdateResultFileName) / "inside", "x");
	for (int i = 0; i < 2; i++) {
		const auto result = ReadAndClearPreviousResult(temp.Path());
		Check(result == Unreadable("it is not a regular file"),
		      "a non-empty directory is reported");
		Check(fs::is_directory(fs::symlink_status(path, ec)), "a non-empty directory stays");
	}
	const auto refused = WriteUpdateResult(temp.Path(), {true, "x"});
	Check(!refused &&
	          refused.error().starts_with("Cannot write " + path.string() + ": cannot rename "),
	      "writing over a non-empty directory fails: " +
	          (refused ? std::string() : refused.error()));
	Check(OtherEntries(temp.Path()) == std::vector<std::string> {"target"},
	      "no temporary file stays after a failed rename");
	fs::remove_all(path, ec);

	// A FIFO: reading neither blocks nor keeps it.
	Check(mkfifo(path.c_str(), 0600) == 0, "mkfifo");
	CheckReadOnce(temp, Unreadable("it is not a regular file"), "a FIFO");
}

void TestWriteFailures() {
	const TempDir temp("kyty_result");
	const auto    missing = WriteUpdateResult(temp.Path() / "missing", {true, "x"});
	Check(!missing &&
	          missing.error() ==
	              "Cannot write " + (temp.Path() / "missing" / ".kyty-update-result").string() +
	                  ": cannot create a temporary file in " + (temp.Path() / "missing").string() +
	                  ": " + std::generic_category().message(ENOENT),
	      "a missing directory: " + (missing ? std::string() : missing.error()));
	Check(!Exists(temp.Path() / "missing"), "nothing is created");

	// A real write error (the file size limit): the older result stays, no temporary file stays.
	Write(temp, {false, "older"}, "older");
	rlimit saved {};
	Check(getrlimit(RLIMIT_FSIZE, &saved) == 0, "getrlimit");
	const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
	rlimit     limit            = saved;
	limit.rlim_cur              = 100;
	Check(setrlimit(RLIMIT_FSIZE, &limit) == 0, "setrlimit");
	const auto too_large = WriteUpdateResult(temp.Path(), {true, std::string(1000, 'x')});
	Check(setrlimit(RLIMIT_FSIZE, &saved) == 0, "restore the file size limit");
	std::signal(SIGXFSZ, previous_handler);
	Check(!too_large && too_large.error().find(": cannot write ") != std::string::npos &&
	          too_large.error().ends_with(std::generic_category().message(EFBIG)),
	      "EFBIG: " + (too_large ? std::string() : too_large.error()));
	Check(OtherEntries(temp.Path()).empty(), "EFBIG: no temporary file stays");
	CheckReadOnce(temp, PreviousResult {false, "older"}, "the older result stays");
}

} // namespace

int main() {
	TestFormat();
	TestCut();
	TestMalformedFiles();
	TestPathKinds();
	TestWriteFailures();
	std::printf("UpdateResultTests: all tests passed\n");
	return 0;
}
