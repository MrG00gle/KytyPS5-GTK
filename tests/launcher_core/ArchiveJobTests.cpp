// Example tests of the compression job (src/launcher/core/archiveJob.h, task 16.3; Requirements
// 11.8, 11.9, 11.10, 11.16; plan D22): the archive path, Prepare with an existing file, directory,
// or dangling link at <dir>.zar, a real Common::PackArchive run read back through Common::File, a
// real run cancelled midway, and scripted packers (tests/support/contentGenerators.h) for failures,
// partial archives, the percent sequence, and the cancellation corner cases. The verification
// guard (orchestrator decision, option 2): real packs of trees without file data (only empty
// folders, only zero-byte files), which Common::PackArchive reports as successful but ZArchive
// cannot open, and an injected verifier failure on a normal tree; each fails with VerifyFailed,
// leaves no .zar, and leaves the source byte-identical.

#include "common/archive.h"
#include "common/file.h"
#include "launcher/core/archiveJob.h"
#include "launcher/core/cancelToken.h"
#include "support/contentGenerators.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace ContentGen = KytyTests::ContentGen;

using ContentGen::PackerScript;
using kyty::core::ArchiveJob;
using kyty::core::ArchiveOutcome;
using kyty::core::ArchivePacker;
using kyty::core::ArchiveVerifier;
using kyty::core::CancelToken;

// A verifier that accepts every archive, for packers that write placeholder bytes.
bool AcceptAll(const fs::path& /*archive*/) {
	return true;
}

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ArchiveJobTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Pseudorandom(std::size_t size, std::uint32_t seed) {
	std::string bytes(size, '\0');
	for (auto& byte: bytes) {
		seed = seed * 1664525u + 1013904223u;
		byte = static_cast<char>(seed >> 24u);
	}
	return bytes;
}

// The files of the source tree used for the real packer runs.
std::map<std::string, std::string> SourceFiles() {
	return {{"eboot.bin", Pseudorandom((5u << 20u) / 2, 1)}, // 2.5 MiB: three progress calls
	        {"sce_sys/param.json", "{\"titleId\":\"PPSA01234\"}"},
	        {"empty.bin", ""},
	        {"nested/deeper/data.bin", Pseudorandom(1000, 2)}};
}

fs::path MakeSource(const KytyTests::TempDir& temp) {
	for (const auto& [name, bytes]: SourceFiles()) {
		temp.WriteFile(fs::path("Game") / name, bytes);
	}
	temp.MakeDirectories("Game/empty_dir");
	return temp.Path() / "Game";
}

struct RunRecord {
	std::vector<int> percents;
};

std::shared_ptr<RunRecord> RunJob(ArchiveJob& job, const CancelToken& cancel) {
	auto run = std::make_shared<RunRecord>();
	job.Run([run](int percent) { run->percents.push_back(percent); }, cancel);
	return run;
}

void CheckPercents(const std::vector<int>& percents, bool completed) {
	Check(!percents.empty() && percents.front() == 0, "0 is reported first");
	for (std::size_t i = 1; i < percents.size(); i++) {
		Check(percents[i] > percents[i - 1], "the percentages increase");
	}
	Check(percents.back() <= 100, "at most 100");
	Check(completed == (percents.back() == 100), "100 only when complete");
}

void TestArchivePath() {
	Check(kyty::core::ArchivePathFor("/games/Foo") == "/games/Foo.zar", "<dir>.zar");
	Check(kyty::core::ArchivePathFor("/games/Foo/") == "/games/Foo.zar", "a trailing separator");
	Check(kyty::core::ArchivePathFor("games/Foo.v1") == "games/Foo.v1.zar",
	      "relative, with a dot in the name");
}

void TestPrepare() {
	const KytyTests::TempDir temp("kyty_archive_job");
	const auto               game = temp.MakeDirectories("Game");

	auto job = ArchiveJob::Prepare(temp.Path() / "Game/");
	Check(job && job->Source() == game && job->Archive() == temp.Path() / "Game.zar" &&
	          job->Outcome() == ArchiveOutcome::Pending && job->Error().empty(),
	      "a free archive path");

	const auto file     = temp.WriteFile("Game.zar", "existing archive");
	const auto existing = ArchiveJob::Prepare(game);
	Check(!existing && existing.error() == file.string() + " already exists.",
	      "an existing file is named: " + (existing ? std::string() : existing.error()));
	Check(ContentGen::SnapshotTree(temp.Path())["Game.zar"] == "F:existing archive",
	      "the existing file is unchanged");
	std::error_code ec;
	fs::remove(file, ec);

	temp.MakeDirectories("Game.zar");
	Check(!ArchiveJob::Prepare(game), "an existing directory");
	fs::remove(temp.Path() / "Game.zar", ec);
	temp.MakeSymlink("nowhere", "Game.zar");
	const auto dangling = ArchiveJob::Prepare(game);
	Check(!dangling && dangling.error() == (temp.Path() / "Game.zar").string() + " already exists.",
	      "a dangling link counts as existing");

	const std::string long_name(252, 'g');
	const auto        long_game = temp.MakeDirectories(long_name);
	const auto        too_long  = ArchiveJob::Prepare(long_game);
	Check(!too_long && too_long.error().starts_with((temp.Path() / (long_name + ".zar")).string() +
	                                                ": cannot check whether it exists: "),
	      "a path that cannot be checked: " + (too_long ? std::string() : too_long.error()));
}

void TestRealPack() {
	const KytyTests::TempDir temp("kyty_archive_job");
	const auto               source = MakeSource(temp);
	const auto               before = ContentGen::SnapshotTree(source);
	auto                     job    = ArchiveJob::Prepare(source);
	Check(job.has_value(), "prepare");
	CancelToken cancel;
	const auto  run = RunJob(*job, cancel);
	Check(job->Outcome() == ArchiveOutcome::Completed && job->Error().empty(), "completed");
	CheckPercents(run->percents, true);
	Check(run->percents.size() >= 3, "progress while packing");
	Check(kyty::core::CanOpenArchive(job->Archive()), "the default verifier accepted the archive");
	Check(ContentGen::SnapshotTree(source) == before, "the source is unchanged");

	for (const auto& [name, bytes]: SourceFiles()) {
		Common::File file(Common::MakeArchivePath(job->Archive(), name), Common::File::Mode::Read);
		Check(!file.IsInvalid(), "open archive member " + name);
		const auto data = file.ReadWholeBuffer();
		Check(std::string(reinterpret_cast<const char*>(data.data()), data.size()) == bytes,
		      "archive member " + name);
	}
	Check(Common::File::IsDirectoryExisting(Common::MakeArchivePath(job->Archive(), "empty_dir")),
	      "the empty directory is archived");

	const auto again = RunJob(*job, cancel);
	Check(again->percents.empty() && job->Outcome() == ArchiveOutcome::Completed,
	      "a second Run does nothing");
}

void TestRealCancel() {
	const KytyTests::TempDir temp("kyty_archive_job");
	const auto               source = MakeSource(temp);
	const auto               before = ContentGen::SnapshotTree(source);
	CancelToken              cancel;
	int                      calls = 0;
	const ArchivePacker      packer =
	    [&](const fs::path& directory, const fs::path& archive,
	        const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
		    return Common::PackArchive(directory, archive,
		                               [&](std::uint64_t done, std::uint64_t total) {
			                               if (++calls == 2) {
				                               cancel.Cancel();
			                               }
			                               return progress(done, total);
		                               });
	    };
	auto job = ArchiveJob::Prepare(source, packer);
	Check(job.has_value(), "prepare");
	const auto run = RunJob(*job, cancel);
	Check(job->Outcome() == ArchiveOutcome::Cancelled && job->Error().empty(),
	      "cancelled without an error");
	Check(calls == 2, "the pack stopped at the cancelled call");
	CheckPercents(run->percents, false);
	Check(!fs::exists(fs::symlink_status(job->Archive())), "no partial archive is left");
	Check(ContentGen::SnapshotTree(source) == before, "the source is unchanged");
}

struct Scripted {
	std::shared_ptr<ContentGen::PackerLog> log = std::make_shared<ContentGen::PackerLog>();
	CancelToken                            cancel;
	std::vector<int>                       percents;
	ArchiveOutcome                         outcome = ArchiveOutcome::Pending;
	std::string                            error;
	fs::path                               archive;
	std::vector<fs::path>                  verified; // the verifier's calls
};

// Runs a job over a small source with a scripted packer; the scripted packer writes placeholder
// bytes, so the verifier answers accept instead of opening them.
void RunScript(const KytyTests::TempDir& temp, const PackerScript& script, Scripted& result,
               bool cancel_first = false, bool accept = true) {
	const auto source = temp.Path() / "Game";
	temp.WriteFile("Game/eboot.bin", "ELF");
	auto job = ArchiveJob::Prepare(source,
	                               ContentGen::ScriptedPacker(
	                                   script, [&] { result.cancel.Cancel(); }, result.log),
	                               [&result, accept](const fs::path& archive) {
		                               result.verified.push_back(archive);
		                               return accept;
	                               });
	Check(job.has_value(), "prepare");
	if (cancel_first) {
		result.cancel.Cancel();
	}
	job->Run([&](int percent) { result.percents.push_back(percent); }, result.cancel);
	result.outcome = job->Outcome();
	result.error   = job->Error();
	result.archive = job->Archive();
	Check(ContentGen::SnapshotTree(source) ==
	          std::map<std::string, std::string> {{"eboot.bin", "F:ELF"}},
	      "the source is unchanged");
}

void TestCancelBeforeStart() {
	const KytyTests::TempDir temp("kyty_archive_job");
	PackerScript             script;
	script.calls = {{1, 2}};
	Scripted result;
	RunScript(temp, script, result, true);
	Check(result.outcome == ArchiveOutcome::Cancelled && result.error.empty() &&
	          !result.log->invoked && result.verified.empty() &&
	          result.percents == std::vector<int> {0} &&
	          !fs::exists(fs::symlink_status(result.archive)),
	      "a job cancelled before it starts never calls the packer or the verifier");
}

void TestScriptedFailure() {
	for (const bool create: {true, false}) {
		const KytyTests::TempDir temp("kyty_archive_job");
		PackerScript             script;
		script.calls          = {{10, 100}, {50, 100}};
		script.end            = PackerScript::End::Fail;
		script.at             = 1;
		script.create_archive = create;
		Scripted result;
		RunScript(temp, script, result);
		Check(result.outcome == ArchiveOutcome::Failed &&
		          result.error == "Could not compress " + (temp.Path() / "Game").string() + "." &&
		          result.percents == std::vector<int> {0, 10} && result.log->calls == 1,
		      "a failure names the source directory: " + result.error);
		Check(!fs::exists(fs::symlink_status(result.archive)), "the partial archive is deleted");
		Check(result.verified.empty(), "a failed pack is not verified");
	}
}

void TestScriptedCancel() {
	const KytyTests::TempDir temp("kyty_archive_job");
	PackerScript             script;
	script.calls = {{10, 100}, {20, 100}, {30, 100}};
	script.end   = PackerScript::End::Cancel;
	script.at    = 1;
	Scripted result;
	RunScript(temp, script, result);
	Check(result.outcome == ArchiveOutcome::Cancelled && result.error.empty() &&
	          result.percents == std::vector<int> {0, 10} && result.log->calls == 2 &&
	          result.log->refused,
	      "the progress call after the cancel request stops the packer");
	Check(!fs::exists(fs::symlink_status(result.archive)),
	      "the partial archive the packer left is deleted");
	Check(result.verified.empty(), "a cancelled pack is not verified");
}

void TestPartialNotRemovable() {
	for (const bool cancelled: {false, true}) {
		const KytyTests::TempDir temp("kyty_archive_job");
		temp.WriteFile("Game/eboot.bin", "ELF");
		CancelToken         cancel;
		const ArchivePacker packer = [&](const fs::path&, const fs::path& archive,
		                                 const std::function<bool(std::uint64_t, std::uint64_t)>&) {
			// A directory that is not empty cannot be removed with std::filesystem::remove.
			Check(ContentGen::WriteHostFile(archive / "inner", "x"), "write the partial directory");
			if (cancelled) {
				cancel.Cancel();
			}
			return false;
		};
		auto job = ArchiveJob::Prepare(temp.Path() / "Game", packer);
		Check(job.has_value(), "prepare");
		job->Run({}, cancel);
		const auto expected = "Could not compress " + (temp.Path() / "Game").string() +
		                      ": cannot remove the partial archive " +
		                      (temp.Path() / "Game.zar").string() + ": ";
		Check(job->Outcome() == ArchiveOutcome::Failed && job->Error().starts_with(expected) &&
		          job->Error().ends_with(".") && fs::exists(temp.Path() / "Game.zar/inner"),
		      "a partial archive that cannot be deleted is reported: " + job->Error());
	}
}

void TestCancelCorners() {
	{
		// The packer succeeds although cancellation was requested after its last progress call.
		const KytyTests::TempDir temp("kyty_archive_job");
		temp.WriteFile("Game/eboot.bin", "ELF");
		CancelToken         cancel;
		const ArchivePacker packer =
		    [&](const fs::path&, const fs::path& archive,
		        const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
			    const bool going = progress(5, 10);
			    cancel.Cancel();
			    return going && ContentGen::WriteHostFile(archive, "complete");
		    };
		auto job = ArchiveJob::Prepare(temp.Path() / "Game", packer, AcceptAll);
		Check(job.has_value(), "prepare");
		const auto run = RunJob(*job, cancel);
		Check(job->Outcome() == ArchiveOutcome::Completed &&
		          run->percents == std::vector<int> {0, 50, 100} &&
		          fs::exists(temp.Path() / "Game.zar"),
		      "a late cancel request keeps the completed archive (as the Qt launcher)");
	}
	{
		// As above, but the archive cannot be opened: the cancellation wins, without an error.
		const KytyTests::TempDir temp("kyty_archive_job");
		temp.WriteFile("Game/eboot.bin", "ELF");
		CancelToken         cancel;
		const ArchivePacker packer =
		    [&](const fs::path&, const fs::path& archive,
		        const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
			    const bool going = progress(5, 10);
			    cancel.Cancel();
			    return going && ContentGen::WriteHostFile(archive, "not a zarchive");
		    };
		auto job = ArchiveJob::Prepare(temp.Path() / "Game", packer);
		Check(job.has_value(), "prepare");
		const auto run = RunJob(*job, cancel);
		Check(job->Outcome() == ArchiveOutcome::Cancelled && job->Error().empty() &&
		          run->percents == std::vector<int> {0, 50} &&
		          !fs::exists(temp.Path() / "Game.zar"),
		      "a late cancel request and an unreadable archive: cancelled, archive deleted");
	}
	{
		// The packer fails without another progress call after the cancel request.
		const KytyTests::TempDir temp("kyty_archive_job");
		temp.WriteFile("Game/eboot.bin", "ELF");
		CancelToken         cancel;
		const ArchivePacker packer = [&](const fs::path&, const fs::path& archive,
		                                 const std::function<bool(std::uint64_t, std::uint64_t)>&) {
			Check(ContentGen::WriteHostFile(archive, "partial"), "write the partial archive");
			cancel.Cancel();
			return false;
		};
		auto job = ArchiveJob::Prepare(temp.Path() / "Game", packer);
		Check(job.has_value(), "prepare");
		job->Run({}, cancel);
		Check(job->Outcome() == ArchiveOutcome::Cancelled && job->Error().empty() &&
		          !fs::exists(temp.Path() / "Game.zar"),
		      "a failure after a cancel request counts as the cancellation");
	}
}

void TestArchiveAppearsBeforeRun() {
	const KytyTests::TempDir temp("kyty_archive_job");
	temp.WriteFile("x/Game/eboot.bin", "ELF");
	{
		PackerScript script;
		auto         log = std::make_shared<ContentGen::PackerLog>();
		auto         job = ArchiveJob::Prepare(temp.Path() / "x/Game",
		                                       ContentGen::ScriptedPacker(script, {}, log));
		Check(job.has_value(), "prepare");
		temp.WriteFile("x/Game.zar", "someone else's archive");
		CancelToken cancel;
		const auto  run = RunJob(*job, cancel);
		Check(job->Outcome() == ArchiveOutcome::Failed &&
		          job->Error() == "Could not compress " + (temp.Path() / "x/Game").string() + ": " +
		                              (temp.Path() / "x/Game.zar").string() + " already exists." &&
		          !log->invoked && run->percents == std::vector<int> {0} &&
		          ContentGen::SnapshotTree(temp.Path() / "x")["Game.zar"] ==
		              "F:someone else's archive",
		      "an archive created after Prepare is not touched: " + job->Error());
	}
	{
		PackerScript    script;
		auto            log = std::make_shared<ContentGen::PackerLog>();
		std::error_code ec;
		fs::remove(temp.Path() / "x/Game.zar", ec);
		auto job = ArchiveJob::Prepare(temp.Path() / "x/Game",
		                               ContentGen::ScriptedPacker(script, {}, log));
		Check(job.has_value(), "prepare");
		// The parent directory becomes a link loop: the archive path cannot be checked.
		fs::rename(temp.Path() / "x", temp.Path() / "y", ec);
		Check(!ec, "move x away");
		temp.MakeSymlink("x", "x");
		CancelToken cancel;
		job->Run({}, cancel);
		Check(job->Outcome() == ArchiveOutcome::Failed &&
		          job->Error().starts_with("Could not compress " +
		                                   (temp.Path() / "x/Game").string() + ": " +
		                                   (temp.Path() / "x/Game.zar").string() +
		                                   ": cannot check whether it exists: ") &&
		          !log->invoked,
		      "an archive path that cannot be checked when Run starts: " + job->Error());
	}
}

void TestPercentSequence() {
	constexpr auto           kMax = std::numeric_limits<std::uint64_t>::max();
	const KytyTests::TempDir temp("kyty_archive_job");
	PackerScript             script;
	script.calls = {{0, 0}, {5, 0}, {1, 3},           {1, 3},       {2, 3},   {1, 3},
	                {3, 3}, {5, 3}, {kMax - 1, kMax}, {kMax, kMax}, {7, 1000}};
	Scripted result;
	RunScript(temp, script, result);
	Check(result.outcome == ArchiveOutcome::Completed &&
	          result.percents == std::vector<int> {0, 33, 66, 99, 100},
	      "floor(100 * done / total), only increases, at most 99 until the packer returns");
	Check(fs::exists(result.archive), "the packer's archive stays");
	Check(result.verified == std::vector<fs::path> {result.archive},
	      "the archive is verified once, after the pack");

	const KytyTests::TempDir refused_temp("kyty_archive_job");
	Scripted                 refused;
	RunScript(refused_temp, script, refused, false, false);
	Check(refused.outcome == ArchiveOutcome::VerifyFailed && refused.error.empty() &&
	          refused.percents == std::vector<int> {0, 33, 66, 99} &&
	          refused.verified == std::vector<fs::path> {refused.archive} &&
	          !fs::exists(fs::symlink_status(refused.archive)),
	      "a refused archive never reports 100 and is deleted");
}

void TestCanOpenArchive() {
	const KytyTests::TempDir temp("kyty_archive_job");
	temp.WriteFile("source/eboot.bin", "ELF");
	const auto archive = temp.Path() / "real.zar";
	Check(Common::PackArchive(temp.Path() / "source", archive,
	                          [](std::uint64_t, std::uint64_t) { return true; }),
	      "pack real.zar");
	Check(kyty::core::CanOpenArchive(archive), "a real archive opens");
	Check(!kyty::core::CanOpenArchive(temp.WriteFile("text.zar", "not a zarchive")),
	      "a text file does not open");
	Check(!kyty::core::CanOpenArchive(temp.Path() / "missing.zar"), "a missing file");
	Check(!kyty::core::CanOpenArchive(temp.MakeDirectories("dir.zar")), "a directory");
}

// A real pack of a tree without file data "succeeds" but its archive cannot be opened.
void TestVerifyDataLessTrees() {
	for (const bool zero_byte_files: {false, true}) {
		const KytyTests::TempDir temp("kyty_archive_job");
		if (zero_byte_files) {
			temp.WriteFile("Game/eboot.bin", "");
			temp.WriteFile("Game/sce_sys/param.json", "");
			temp.WriteFile("Game/nested/empty.bin", "");
		} else {
			temp.MakeDirectories("Game/a/b");
			temp.MakeDirectories("Game/c");
		}
		const auto  source = temp.Path() / "Game";
		const auto  before = ContentGen::SnapshotTree(source);
		const auto  what   = zero_byte_files ? "zero-byte files only" : "empty folders only";
		CancelToken cancel;
		auto        job = ArchiveJob::Prepare(source);
		Check(job.has_value(), "prepare");
		const auto run = RunJob(*job, cancel);
		Check(job->Outcome() == ArchiveOutcome::VerifyFailed && job->Error().empty() &&
		          job->Source() == source,
		      std::string(what) + ": VerifyFailed with the source path and no text");
		Check(run->percents == std::vector<int> {0}, std::string(what) + ": no 100");
		Check(!fs::exists(fs::symlink_status(job->Archive())), std::string(what) + ": no .zar");
		Check(ContentGen::SnapshotTree(source) == before,
		      std::string(what) + ": the source is unchanged");
	}
}

// An injected verifier failure on a normal tree, packed by Common::PackArchive.
void TestVerifyInjectedFailure() {
	const KytyTests::TempDir temp("kyty_archive_job");
	const auto               source = MakeSource(temp);
	const auto               before = ContentGen::SnapshotTree(source);
	std::vector<fs::path>    calls;
	bool                     readable = false;
	const ArchiveVerifier    verifier = [&](const fs::path& archive) {
		calls.push_back(archive);
		readable = kyty::core::CanOpenArchive(archive);
		return false;
	};
	auto job = ArchiveJob::Prepare(source, {}, verifier);
	Check(job.has_value(), "prepare");
	CancelToken cancel;
	const auto  run = RunJob(*job, cancel);
	Check(calls == std::vector<fs::path> {job->Archive()} && readable,
	      "the verifier saw the complete, readable archive");
	Check(job->Outcome() == ArchiveOutcome::VerifyFailed && job->Error().empty() &&
	          job->Source() == source,
	      "an injected verifier failure is VerifyFailed");
	CheckPercents(run->percents, false);
	Check(run->percents.size() >= 3, "progress while packing, then no 100");
	Check(!fs::exists(fs::symlink_status(job->Archive())), "the refused archive is deleted");
	Check(ContentGen::SnapshotTree(source) == before, "the source is unchanged");
}

// A refused archive that cannot be deleted is a Failed with the reason.
void TestVerifyRemovalFails() {
	const KytyTests::TempDir temp("kyty_archive_job");
	temp.WriteFile("Game/eboot.bin", "ELF");
	const ArchivePacker packer = [](const fs::path&, const fs::path& archive,
	                                const std::function<bool(std::uint64_t, std::uint64_t)>&) {
		return ContentGen::WriteHostFile(archive / "inner", "x");
	};
	auto job = ArchiveJob::Prepare(temp.Path() / "Game", packer);
	Check(job.has_value(), "prepare");
	CancelToken cancel;
	job->Run({}, cancel);
	const auto expected = "Could not compress " + (temp.Path() / "Game").string() +
	                      ": cannot remove the partial archive " +
	                      (temp.Path() / "Game.zar").string() + ": ";
	Check(job->Outcome() == ArchiveOutcome::Failed && job->Error().starts_with(expected) &&
	          fs::exists(temp.Path() / "Game.zar/inner"),
	      "an unreadable archive that cannot be deleted is reported: " + job->Error());
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("ArchiveJobTests");
	TestArchivePath();
	TestPrepare();
	TestRealPack();
	TestRealCancel();
	TestCancelBeforeStart();
	TestScriptedFailure();
	TestScriptedCancel();
	TestPartialNotRemovable();
	TestCancelCorners();
	TestArchiveAppearsBeforeRun();
	TestPercentSequence();
	TestCanOpenArchive();
	TestVerifyDataLessTrees();
	TestVerifyInjectedFailure();
	TestVerifyRemovalFails();
	std::printf("ArchiveJobTests: all tests passed\n");
	return 0;
}
