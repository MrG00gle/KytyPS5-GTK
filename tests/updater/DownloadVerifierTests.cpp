// Example tests of kyty::updater::DownloadVerifier (src/updater/core/downloadVerifier.h, task 17.1;
// Requirements 13.7, 13.8): an exact download in chunks of every kind, a corrupted byte, a
// truncated and an extended download, a wrong published digest, injected and real write errors,
// Abort(), destruction before Finish(), an output path that cannot be created, a zero-size asset,
// and an installation directory that stays byte-identical whenever the download is rejected.

#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/downloadVerifier.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::updater::DownloadFailureKind;
using kyty::updater::DownloadVerifier;
using kyty::updater::ReleaseAsset;
using KytyTests::TempDir;
using UpdaterGen::AsBytes;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "DownloadVerifierTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
}

ReleaseAsset AssetFor(std::string_view bytes) {
	return ReleaseAsset {"v1-Linux-x86_64.tar.gz", UpdaterGen::DigestOf(bytes), bytes.size()};
}

// Appends every chunk; the error of the first failing Append, or "" when all succeeded.
std::string AppendAll(DownloadVerifier& verifier, const std::vector<std::string_view>& chunks) {
	for (const auto chunk: chunks) {
		const auto status = verifier.Append(AsBytes(chunk));
		if (!status) {
			return status.error();
		}
	}
	return "";
}

std::string FinishError(DownloadVerifier& verifier) {
	const auto status = verifier.Finish();
	return status ? std::string() : status.error();
}

void TestExact() {
	const std::string                      bytes     = UpdaterGen::PatternBytes(100000, 1);
	const std::vector<std::vector<size_t>> chunkings = {{},
	                                                    {1},
	                                                    {0, 0, 5, 0, 99995},
	                                                    {65536, 0, 1},
	                                                    {99999},
	                                                    {4096, 4096, 4096, 4096},
	                                                    {50000, 50000}};
	for (size_t i = 0; i < chunkings.size(); i++) {
		const std::string name = "chunking " + std::to_string(i);
		const TempDir     temp("kyty_verifier");
		const auto        out = temp.Path() / "package-Linux-x86_64.tar.gz";
		{
			DownloadVerifier verifier(AssetFor(bytes), out);
			Check(verifier.Path() == out, name + ": Path()");
			Check(AppendAll(verifier, UpdaterGen::SplitBytes(bytes, chunkings[i])).empty(),
			      name + ": every chunk is accepted");
			Check(verifier.Received() == bytes.size(), name + ": Received()");
			Check(FinishError(verifier).empty(), name + ": Finish() accepts the file");
			Check(FinishError(verifier).empty(), name + ": Finish() again");
			Check(verifier.LastFailure().kind == DownloadFailureKind::None,
			      name + ": no failure before");
			const auto late = verifier.Append(AsBytes("x"));
			Check(!late && late.error() == "The download is already finished." &&
			          verifier.LastFailure().kind == DownloadFailureKind::Finished,
			      name + ": Append() after Finish() fails");
			verifier.Abort();
		}
		Check(UpdaterGen::ReadFile(out) == bytes, name + ": the file stays with the bytes");
	}
}

void TestRejectedStreams() {
	const std::string bytes = UpdaterGen::PatternBytes(70000, 2);
	struct Case {
		std::string         name;
		std::string         received;
		ReleaseAsset        asset;
		bool                fails_in_append;
		std::string         error;
		DownloadFailureKind kind;
	};
	std::string corrupted = bytes;
	corrupted[12345] ^= 0x20;
	std::string first_corrupted = bytes;
	first_corrupted[0] ^= 0x01;
	std::string last_corrupted = bytes;
	last_corrupted.back() ^= static_cast<char>(0x80);
	auto wrong_digest = AssetFor(bytes);
	wrong_digest.sha256[31] ^= 0x01;
	const std::string       digest = std::string(kyty::updater::kDownloadDigestMessage);
	const std::string       large  = std::string(kyty::updater::kDownloadTooLargeMessage);
	const std::vector<Case> cases  = {
	    {"corrupted in the middle", corrupted, AssetFor(bytes), false, digest,
	     DownloadFailureKind::Digest},
	    {"corrupted first byte", first_corrupted, AssetFor(bytes), false, digest,
	     DownloadFailureKind::Digest},
	    {"corrupted last byte", last_corrupted, AssetFor(bytes), false, digest,
	     DownloadFailureKind::Digest},
	    {"a wrong published digest", bytes, wrong_digest, false, digest,
	     DownloadFailureKind::Digest},
	    {"one byte short", bytes.substr(0, bytes.size() - 1), AssetFor(bytes), false,
	     "The downloaded file is smaller than the release asset (69999 of 70000 bytes).",
	     DownloadFailureKind::TooSmall},
	    {"empty", "", AssetFor(bytes), false,
	     "The downloaded file is smaller than the release asset (0 of 70000 bytes).",
	     DownloadFailureKind::TooSmall},
	    {"one byte extra", bytes + "x", AssetFor(bytes), true, large,
	     DownloadFailureKind::TooLarge},
	    {"twice the size", bytes + bytes, AssetFor(bytes), true, large,
	     DownloadFailureKind::TooLarge},
	};
	for (const auto& test: cases) {
		for (const std::vector<size_t>& sizes:
		     {std::vector<size_t> {}, std::vector<size_t> {1000, 7, 0, 30000}}) {
			const std::string name =
			    test.name + " in " + std::to_string(sizes.size() + 1) + " chunks";
			const TempDir temp("kyty_verifier");
			temp.WriteFile("install/kyty_launcher", "launcher");
			temp.WriteFile("install/assets/sounds/a.wav", "wav");
			const auto workspace = temp.MakeDirectories("install/.kyty-update-abc123");
			const auto before    = UpdaterGen::SnapshotTree(temp.Path() / "install");
			const auto out       = workspace / "package-Linux-x86_64.tar.gz";
			{
				DownloadVerifier verifier(test.asset, out);
				const auto       append_error =
				    AppendAll(verifier, UpdaterGen::SplitBytes(test.received, sizes));
				if (test.fails_in_append) {
					Check(append_error == test.error, name + ": Append() fails with '" +
					                                      test.error + "', got '" + append_error +
					                                      "'");
					Check(!Exists(out), name + ": the file is deleted at once");
					Check(verifier.Received() <= test.asset.size,
					      name + ": nothing beyond the size");
					const auto again = verifier.Append(AsBytes("y"));
					Check(!again && again.error() == test.error, name + ": the failure sticks");
				} else {
					Check(append_error.empty(), name + ": every chunk is accepted");
				}
				const auto finish_error = FinishError(verifier);
				Check(finish_error == test.error, name + ": Finish() fails with '" + test.error +
				                                      "', got '" + finish_error + "'");
				Check(!Exists(out), name + ": the file is deleted");
				// Phase 9 V1: the failure's kind and, for a short file, its two numbers.
				const auto& failure = verifier.LastFailure();
				Check(failure.kind == test.kind, name + ": the failure's kind");
				Check(test.kind != DownloadFailureKind::TooSmall ||
				          (failure.received == test.received.size() &&
				           failure.size == test.asset.size),
				      name + ": the received bytes and the size");
			}
			Check(UpdaterGen::SnapshotTree(temp.Path() / "install") == before,
			      name + ": the installation directory is byte-identical");
		}
	}
}

void TestWriteFaults() {
	const std::string bytes = UpdaterGen::PatternBytes(10000, 3);
	for (const int failing_call: {1, 2, 5}) {
		const std::string name = "write fault at call " + std::to_string(failing_call);
		const TempDir     temp("kyty_verifier");
		const auto        out   = temp.Path() / "package";
		int               calls = 0;
		kyty::updater::SetDownloadWriteFaultForTesting([&calls, failing_call] {
			calls++;
			return calls == failing_call ? ENOSPC : 0;
		});
		{
			DownloadVerifier verifier(AssetFor(bytes), out);
			const auto       error =
			    AppendAll(verifier, UpdaterGen::SplitBytes(bytes, {0, 2000, 2000, 2000, 2000}));
			const auto expected =
			    "Cannot write " + out.string() + ": " + std::generic_category().message(ENOSPC);
			Check(error == expected, name + ": '" + error + "'");
			Check(calls == failing_call, name + ": the hook runs once per non-empty chunk");
			Check(!Exists(out), name + ": the file is deleted");
			Check(FinishError(verifier) == expected, name + ": Finish() reports the write error");
			Check(verifier.LastFailure().kind == DownloadFailureKind::Write &&
			          verifier.LastFailure().error_number == ENOSPC,
			      name + ": the write failure's errno");
		}
		kyty::updater::SetDownloadWriteFaultForTesting({});
	}

	// A real write error: the file size limit makes write() fail with EFBIG.
	const TempDir temp("kyty_verifier");
	const auto    out = temp.Path() / "package";
	rlimit        saved {};
	Check(getrlimit(RLIMIT_FSIZE, &saved) == 0, "getrlimit");
	const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
	{
		DownloadVerifier verifier(AssetFor(bytes), out);
		rlimit           limit = saved;
		limit.rlim_cur         = 100;
		Check(setrlimit(RLIMIT_FSIZE, &limit) == 0, "setrlimit");
		const auto status = verifier.Append(AsBytes(bytes));
		Check(setrlimit(RLIMIT_FSIZE, &saved) == 0, "restore the file size limit");
		Check(!status && status.error() == "Cannot write " + out.string() + ": " +
		                                       std::generic_category().message(EFBIG),
		      "EFBIG: '" + (status ? std::string() : status.error()) + "'");
		Check(!Exists(out), "EFBIG: the file is deleted");
	}
	std::signal(SIGXFSZ, previous_handler);
}

void TestAbortAndDestruction() {
	const std::string bytes = UpdaterGen::PatternBytes(5000, 4);
	const TempDir     temp("kyty_verifier");
	const auto        out = temp.Path() / "package";
	{
		DownloadVerifier verifier(AssetFor(bytes), out);
		Check(Exists(out), "the constructor creates the file");
		Check(verifier.Append(AsBytes(std::string_view(bytes).substr(0, 2000))).has_value(),
		      "first half");
		verifier.Abort();
		Check(!Exists(out), "Abort() deletes the file");
		const auto append = verifier.Append(AsBytes("z"));
		Check(!append && append.error() == kyty::updater::kDownloadAbortedMessage,
		      "Append() after Abort()");
		Check(FinishError(verifier) == kyty::updater::kDownloadAbortedMessage &&
		          verifier.LastFailure().kind == DownloadFailureKind::Aborted,
		      "Finish() after Abort()");
		verifier.Abort();
	}
	{
		DownloadVerifier verifier(AssetFor(bytes), out);
		Check(verifier.Append(AsBytes(std::string_view(bytes).substr(0, 10))).has_value(),
		      "a part");
	}
	Check(!Exists(out), "destruction before Finish() deletes a partial file");
	{
		const DownloadVerifier verifier(AssetFor(bytes), out);
	}
	Check(!Exists(out), "destruction without any data deletes the empty file");
	{
		DownloadVerifier verifier(AssetFor(bytes), out);
		Check(verifier.Append(AsBytes(bytes)).has_value() && verifier.Finish().has_value(),
		      "complete");
		verifier.Abort();
		Check(UpdaterGen::ReadFile(out) == bytes, "Abort() after Finish() keeps the file");
	}
}

void TestCreationFailures() {
	const std::string bytes = "content";
	const TempDir     temp("kyty_verifier");
	const auto        existing = temp.WriteFile("existing", "keep me");
	const auto        link     = temp.MakeSymlink("missing-target", "dangling");
	struct Case {
		fs::path path;
		int      error_number;
	};
	for (const Case& test: {Case {existing, EEXIST}, Case {link, EEXIST},
	                        Case {temp.Path() / "missing" / "package", ENOENT},
	                        Case {existing / "package", ENOTDIR}}) {
		const auto expected = "Cannot create " + test.path.string() + ": " +
		                      std::generic_category().message(test.error_number);
		{
			DownloadVerifier verifier(AssetFor(bytes), test.path);
			const auto       append = verifier.Append(AsBytes(bytes));
			Check(!append && append.error() == expected, "Append(): '" + expected + "'");
			Check(verifier.LastFailure().kind == DownloadFailureKind::Create &&
			          verifier.LastFailure().error_number == test.error_number,
			      "the creation failure's errno: '" + expected + "'");
			Check(FinishError(verifier) == expected, "Finish(): '" + expected + "'");
			verifier.Abort();
		}
		Check(UpdaterGen::ReadFile(existing) == "keep me",
		      test.path.string() + ": the existing file stays");
	}
	std::error_code ec;
	Check(fs::symlink_status(link, ec).type() == fs::file_type::symlink, "the dangling link stays");
	Check(!Exists(temp.Path() / "missing-target"), "nothing is created through the link");
}

void TestZeroSize() {
	const TempDir temp("kyty_verifier");
	{
		DownloadVerifier verifier(AssetFor(""), temp.Path() / "empty");
		Check(verifier.Append({}).has_value(), "an empty chunk fits a zero-size asset");
		Check(verifier.Finish().has_value(), "a zero-size asset with the empty digest is accepted");
	}
	Check(UpdaterGen::ReadFile(temp.Path() / "empty") == "", "the empty file stays");
	{
		DownloadVerifier verifier(AssetFor(""), temp.Path() / "one");
		const auto       append = verifier.Append(AsBytes("x"));
		Check(!append && append.error() == kyty::updater::kDownloadTooLargeMessage,
		      "one byte is too large for a zero-size asset");
	}
	Check(!Exists(temp.Path() / "one"), "the rejected file is deleted");
}

} // namespace

int main() {
	TestExact();
	TestRejectedStreams();
	TestWriteFaults();
	TestAbortAndDestruction();
	TestCreationFailures();
	TestZeroSize();
	std::printf("DownloadVerifierTests: all tests passed\n");
	return 0;
}
