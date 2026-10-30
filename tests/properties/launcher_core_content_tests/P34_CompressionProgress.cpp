// Feature: linux-gtk-modernization, Property 34: Compression progress and cleanup
//
// Validates Requirements 11.8, 11.10, and 11.16. Each case draws a small source tree and a packer
// behavior: in three cases of four a scripted packer (tests/support/contentGenerators.h) with 0-12
// arbitrary (done, total) progress calls (growing, shrinking, repeated, zero totals, done above
// total, values near 2^64) that ends in success, cancellation requested before call k, or failure
// after k calls, and that writes a partial archive and leaves it or not, plus an injected
// verifier answer for a successful pack (it refuses one archive in four); in one case of four the
// real Common::PackArchive with the default verifier (Common::OpenArchive), cancelled before
// progress call k or run to the end. ArchiveJob::Run must report non-decreasing integer
// percentages from 0 to 100, starting at 0, with 100 only for Completed; for the scripted packer
// exactly the reference sequence (each new maximum of min(99, floor(100 * done / total)), 0 for a
// zero total, then 100 after a successful and verified pack). On cancellation or failure no
// <dir>.zar is left and the source tree is byte-identical to before; cancellation reports no
// error, a packer failure reports "Could not compress <source directory>.".
//
// The verification guard (orchestrator decision, option 2) changed two expectations: a pack that
// the verifier refuses is a failure of kind VerifyFailed (no 100, the archive deleted, the source
// unchanged, Source() names the source directory and Error() is empty, because the dialog words
// that failure itself), and the real packer on a tree whose files are all zero bytes (its archive
// cannot be opened) gives VerifyFailed instead of Completed.

#include "common/archive.h"
#include "launcher/core/archiveJob.h"
#include "launcher/core/cancelToken.h"
#include "support/contentGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace ContentGen = KytyTests::ContentGen;

using ContentGen::PackerScript;
using kyty::core::ArchiveJob;
using kyty::core::ArchiveOutcome;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

// 1-6 files in a few directories; non_empty counts the files with at least one byte.
void WriteSource(const KytyTests::TempDir& temp, std::size_t& non_empty) {
	static const std::vector<std::string> kNames = {"eboot.bin", "sce_sys/param.json", "a/b/c.bin",
	                                                "a/d.txt",   "x y/\xC3\xA9.dat",   "z"};
	non_empty                                    = 0;
	const int count                              = Choose(1, 7);
	for (int i = 0; i < count; i++) {
		std::string bytes;
		const int   size = OneIn(4) ? 0 : Choose(1, 2000);
		for (int b = 0; b < size; b++) {
			bytes += static_cast<char>(Choose(0, 256));
		}
		non_empty += size > 0 ? 1 : 0;
		temp.WriteFile(fs::path("Game") / kNames[static_cast<std::size_t>(i)], bytes);
	}
	if (OneIn(2)) {
		temp.MakeDirectories("Game/empty");
	}
}

std::pair<std::uint64_t, std::uint64_t> ProgressCall(std::uint64_t& done, std::uint64_t total) {
	constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
	switch (Choose(0, 8)) {
		case 0: return {Choose<std::uint64_t>(0, 100), 0};
		case 1: return {Choose<std::uint64_t>(kMax - 100, kMax), kMax};
		case 2: return {total + Choose<std::uint64_t>(1, 100), total};
		case 3: return {Choose<std::uint64_t>(0, total + 1), total}; // shrinking or repeated
		default:
			done = std::min(total, done + Choose<std::uint64_t>(0, total / 3 + 2));
			return {done, total};
	}
}

PackerScript Script() {
	PackerScript        script;
	const std::uint64_t total = OneIn(5) ? Choose<std::uint64_t>(1, 10)
	                                     : Choose<std::uint64_t>(1, std::uint64_t {1} << 40u);
	std::uint64_t       done  = 0;
	const int           count = Choose(0, 13);
	for (int i = 0; i < count; i++) {
		script.calls.push_back(ProgressCall(done, total));
	}
	const int end = Choose(0, 3);
	if (end == 1 && !script.calls.empty()) {
		script.end = PackerScript::End::Cancel;
		script.at  = Choose<std::size_t>(0, script.calls.size());
	} else if (end == 2) {
		script.end = PackerScript::End::Fail;
		script.at  = Choose<std::size_t>(0, script.calls.size() + 1);
	}
	script.create_archive = !OneIn(4);
	script.leave_partial  = OneIn(2);
	return script;
}

// The percentages ArchiveJob must report for script when the verifier answers verified.
std::vector<int> ReferencePercents(const PackerScript& script, bool verified) {
	std::vector<int> percents = {0};
	std::size_t      made     = script.calls.size();
	if (script.end != PackerScript::End::Success) {
		made = std::min(script.at, script.calls.size());
	}
	for (std::size_t i = 0; i < made; i++) {
		const auto [done, total] = script.calls[i];
		int percent              = 0;
		if (total != 0) {
			percent = done >= total
			              ? 100
			              : static_cast<int>(static_cast<unsigned __int128>(done) * 100 / total);
		}
		percent = std::min(percent, 99);
		if (percent > percents.back()) {
			percents.push_back(percent);
		}
	}
	if (script.end == PackerScript::End::Success && verified) {
		percents.push_back(100);
	}
	return percents;
}

void CheckPercents(const std::vector<int>& percents, bool completed) {
	RC_ASSERT(!percents.empty());
	RC_ASSERT(percents.front() == 0);
	for (std::size_t i = 1; i < percents.size(); i++) {
		RC_ASSERT(percents[i] >= percents[i - 1]);
	}
	RC_ASSERT(percents.back() >= 0 && percents.back() <= 100);
	RC_ASSERT((percents.back() == 100) == completed);
}

} // namespace

KYTY_PROPERTY(34, "Compression progress and cleanup") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p34");
		std::size_t              non_empty = 0;
		WriteSource(temp, non_empty);
		const auto source  = temp.Path() / "Game";
		const auto before  = ContentGen::SnapshotTree(source);
		const auto failure = "Could not compress " + source.string() + ".";

		kyty::core::CancelToken cancel;
		std::vector<int>        percents;
		const auto              record = [&percents](int percent) { percents.push_back(percent); };

		if (OneIn(4)) {
			// The real packer: one progress call per non-empty file (each is below 1 MiB), and the
			// default verifier.
			// Half of the runs are never cancelled (cancel_at == non_empty).
			const std::size_t cancel_at =
			    OneIn(2) ? non_empty : Choose<std::size_t>(0, non_empty + 1);
			std::size_t                     calls = 0;
			const kyty::core::ArchivePacker packer =
			    [&](const fs::path& directory, const fs::path& archive,
			        const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
				    return Common::PackArchive(directory, archive,
				                               [&](std::uint64_t done, std::uint64_t total) {
					                               if (calls++ == cancel_at) {
						                               cancel.Cancel();
					                               }
					                               return progress(done, total);
				                               });
			    };
			auto job = ArchiveJob::Prepare(source, packer);
			RC_ASSERT(job.has_value());
			job->Run(record, cancel);
			const bool cancelled = cancel_at < non_empty;
			// A tree without file data packs "successfully" into an archive that cannot be opened.
			const bool unreadable = non_empty == 0;
			RC_LOG() << non_empty << " non-empty files, cancel at " << cancel_at << "\n";
			const auto expected = cancelled    ? ArchiveOutcome::Cancelled
			                      : unreadable ? ArchiveOutcome::VerifyFailed
			                                   : ArchiveOutcome::Completed;
			RC_ASSERT(job->Outcome() == expected);
			RC_ASSERT(job->Error().empty());
			RC_ASSERT(job->Source() == source);
			CheckPercents(percents, expected == ArchiveOutcome::Completed);
			RC_ASSERT(fs::exists(fs::symlink_status(job->Archive())) ==
			          (expected == ArchiveOutcome::Completed));
			RC_ASSERT(ContentGen::SnapshotTree(source) == before);
			RC_TAG(cancelled    ? "real packer: cancelled"
			       : unreadable ? "real packer: no file data, verify failed"
			                    : "real packer: completed");
			return;
		}

		const auto            script   = Script();
		const bool            verified = !OneIn(4);
		std::vector<fs::path> verify_calls;
		auto                  log = std::make_shared<ContentGen::PackerLog>();
		auto job = ArchiveJob::Prepare(source,
		                               ContentGen::ScriptedPacker(
		                                   script, [&cancel] { cancel.Cancel(); }, log),
		                               [&verify_calls, verified](const fs::path& archive) {
			                               verify_calls.push_back(archive);
			                               return verified;
		                               });
		RC_ASSERT(job.has_value());
		job->Run(record, cancel);
		RC_LOG() << script.calls.size() << " calls, end " << static_cast<int>(script.end) << " at "
		         << script.at << ", verified " << verified << "\n";

		RC_ASSERT(percents == ReferencePercents(script, verified));
		const bool packed    = script.end == PackerScript::End::Success;
		const bool completed = packed && verified;
		CheckPercents(percents, completed);
		// The verifier sees the archive once, only after a successful pack.
		RC_ASSERT(verify_calls ==
		          (packed ? std::vector<fs::path> {job->Archive()} : std::vector<fs::path> {}));
		const bool archive = fs::exists(fs::symlink_status(job->Archive()));
		switch (script.end) {
			case PackerScript::End::Success:
				if (verified) {
					RC_ASSERT(job->Outcome() == ArchiveOutcome::Completed);
					RC_ASSERT(archive == script.create_archive);
					RC_TAG("success");
				} else {
					RC_ASSERT(job->Outcome() == ArchiveOutcome::VerifyFailed);
					RC_ASSERT(job->Source() == source);
					RC_ASSERT(!archive);
					RC_TAG("verify failed");
				}
				RC_ASSERT(job->Error().empty());
				break;
			case PackerScript::End::Cancel:
				RC_ASSERT(job->Outcome() == ArchiveOutcome::Cancelled);
				RC_ASSERT(job->Error().empty());
				RC_ASSERT(!archive);
				RC_ASSERT(log->refused);
				RC_TAG("cancelled");
				break;
			case PackerScript::End::Fail:
				RC_ASSERT(job->Outcome() == ArchiveOutcome::Failed);
				RC_ASSERT(job->Error() == failure);
				RC_ASSERT(!archive);
				RC_TAG("failed");
				break;
		}
		RC_ASSERT(ContentGen::SnapshotTree(source) == before);
		RC_CLASSIFY(script.create_archive && script.leave_partial && !completed,
		            "partial archive left by the packer");
	});
}
