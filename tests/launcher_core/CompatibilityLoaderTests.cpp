// Example tests of the Compatibility_Database loader and its retry schedule
// (src/launcher/core/compatibility.h and retrySchedule.h, task 14.1; Requirements 8.1-8.4, 8.6),
// with FakeHttpClient and FakeScheduler only (no network): success on attempt 1-4; every failure
// kind (network error, HTTP status, timeout, stall, body over 16 MiB, content that does not parse,
// a non-object root); exactly the delays 750, 1500, 2250 ms from FakeScheduler::RequestedDelays()
// and at most 4 requests; the cache replaced only by content that parsed; the cache fallback with
// a valid, an invalid, and an unreadable cache; the no-cache warning; a cache write fault; earlier
// statuses kept when a load has no database; Start() once; destroying the loader mid-run.

#include "launcher/core/atomicFile.h"
#include "launcher/core/compatibility.h"
#include "launcher/core/httpClient.h"
#include "launcher/core/retrySchedule.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using std::chrono::milliseconds;

using kyty::core::CompatibilityLoader;
using kyty::core::CompatibilityLoadResult;
using kyty::core::CompatibilityModel;
using kyty::core::CompatibilityStatus;
using KytyTests::FakeHttpClient;
using KytyTests::FakeResponse;
using KytyTests::FakeScheduler;
using KytyTests::TempDir;
using Source = CompatibilityLoadResult::Source;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "CompatibilityLoaderTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

bool Contains(const std::string& text, std::string_view part) {
	return text.find(part) != std::string::npos;
}

const std::string kUrl = kyty::core::kCompatibilityUrl;

const std::string kValidBody = R"({"PPSA01234": {"status": "InGame", "comment": "Runs well"}, )"
                               R"("cusa00001": {"status": "Logo"}})";
const std::string kOtherValidBody = R"({"PPSA01234": {"status": "DoesntBoot"}})";

// The ways an attempt can fail, and a part of the text that describes each.
enum class Failure { Network, Status, Timeout, Stall, TooLarge, Unparseable, NotAnObject };

struct FailureCase {
	Failure     failure;
	const char* name;
	const char* description;
};

const std::vector<FailureCase>& FailureCases() {
	static const std::vector<FailureCase> cases = {
	    {Failure::Network, "network", "network error: connection refused"},
	    {Failure::Status, "status", "HTTP status 503"},
	    {Failure::Timeout, "timeout", "no complete response within 15 seconds"},
	    {Failure::Stall, "stall", "no complete response within 15 seconds"},
	    {Failure::TooLarge, "too large", "larger than 16 MiB"},
	    {Failure::Unparseable, "unparseable", "does not parse: the content is not valid JSON"},
	    {Failure::NotAnObject, "not an object", "the top-level JSON value is not an object"},
	};
	return cases;
}

FakeResponse ResponseFor(Failure failure) {
	FakeResponse response;
	response.latency = milliseconds {40};
	switch (failure) {
		case Failure::Network:
			response.network_failure = true;
			response.failure_message = "connection refused";
			break;
		case Failure::Status:
			response.status = 503;
			response.body   = kValidBody; // a body that is not delivered
			break;
		case Failure::Timeout: response.latency = milliseconds {20000}; break;
		case Failure::Stall:
			response.body              = kValidBody;
			response.chunk_sizes       = {10};
			response.stall_after_bytes = 10;
			break;
		case Failure::TooLarge: response.body = std::string((16u << 20u) + 1u, ' '); break;
		case Failure::Unparseable: response.body = "{\"PPSA01234\": "; break;
		case Failure::NotAnObject:
			response.body = "[{\"PPSA01234\": {\"status\": \"InGame\"}}]";
			break;
	}
	return response;
}

FakeResponse ValidResponse(const std::string& body = kValidBody) {
	FakeResponse response;
	response.body        = body;
	response.latency     = milliseconds {25};
	response.chunk_sizes = {1, 7, 30};
	return response;
}

std::optional<std::string> ReadCache(const fs::path& path) {
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	return bytes ? std::optional<std::string>(*bytes) : std::nullopt;
}

// A loader on a fake client and scheduler with its data directory in a TempDir.
struct Harness {
	FakeScheduler                        scheduler;
	FakeHttpClient                       http {scheduler};
	TempDir                              dir {"kyty_compat_loader"};
	std::vector<std::string>             warnings;
	std::vector<CompatibilityLoadResult> results;
	std::unique_ptr<CompatibilityLoader> loader;

	Harness() { MakeLoader(dir.Path()); }

	void MakeLoader(const fs::path& data_dir) {
		loader = std::make_unique<CompatibilityLoader>(
		    http, scheduler, data_dir,
		    CompatibilityLoader::Callbacks {
		        [this](const std::string& warning) { warnings.push_back(warning); },
		        [this](CompatibilityLoadResult result) { results.push_back(std::move(result)); }});
	}

	[[nodiscard]] fs::path CachePath() const { return dir.Path() / "compatibility_cache.json"; }

	const CompatibilityLoadResult& Run() {
		loader->Start();
		Check(results.empty(), "nothing is delivered inside Start()");
		scheduler.RunUntilIdle();
		Check(results.size() == 1, "on_finished runs exactly once");
		Check(loader->Finished(), "Finished() after on_finished");
		Check(http.ActiveCount() == 0 && scheduler.PendingCount() == 0,
		      "no request or timer is left");
		return results.front();
	}

	void CheckRequests(size_t count, const std::string& what) const {
		Check(http.Requests().size() == count, what + ": " + std::to_string(count) +
		                                           " requests, got " +
		                                           std::to_string(http.Requests().size()));
		for (const auto& request: http.Requests()) {
			Check(request.url == kUrl, what + ": the database URL");
			Check(request.total_timeout == milliseconds {15000}, what + ": a 15 s timeout");
			Check(request.max_bytes == (16u << 20u), what + ": a 16 MiB limit");
			Check(request.stall_timeout == milliseconds {0}, what + ": no stall limit");
			Check(request.headers.size() == 1 && request.headers[0].first == "User-Agent",
			      what + ": the User-Agent header");
		}
		const std::vector<milliseconds> all = {milliseconds {750}, milliseconds {1500},
		                                       milliseconds {2250}};
		const std::vector<milliseconds> expected(
		    all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count > 0 ? count - 1 : 0));
		Check(scheduler.RequestedDelays() == expected, what + ": the retry delays");
	}
};

void TestRetrySchedule() {
	Check(kyty::core::kMaxDownloadAttempts == 4, "4 attempts");
	Check(kyty::core::RetryDelayAfter(1) == milliseconds {750} &&
	          kyty::core::RetryDelayAfter(2) == milliseconds {1500} &&
	          kyty::core::RetryDelayAfter(3) == milliseconds {2250},
	      "750, 1500, 2250 ms");
	for (const int attempt: {4, 5, 100, 0, -1}) {
		Check(!kyty::core::RetryDelayAfter(attempt),
		      "no retry after attempt " + std::to_string(attempt));
	}
}

void TestSuccessOnAttempt() {
	for (size_t success = 1; success <= 4; success++) {
		const std::string what = "success on attempt " + std::to_string(success);
		Harness           harness;
		for (size_t i = 1; i < success; i++) {
			harness.http.AddResponse(
			    kUrl, ResponseFor(FailureCases()[i % FailureCases().size()].failure));
		}
		harness.http.AddResponse(kUrl, ValidResponse());
		harness.http.AddResponse(kUrl, ValidResponse(kOtherValidBody)); // never requested
		const auto& result = harness.Run();
		harness.CheckRequests(success, what);
		Check(result.source == Source::Download && result.database.has_value(), what + ": source");
		Check(result.attempts == static_cast<int>(success), what + ": attempts");
		Check(result.last_failure.empty(), what + ": no failure");
		Check(*result.database == *kyty::core::ParseCompatibility(kValidBody),
		      what + ": the downloaded database");
		Check(ReadCache(harness.CachePath()) == kValidBody, what + ": the cache holds the body");
		Check(harness.warnings.size() == success - 1, what + ": one warning per failed attempt");
		for (size_t i = 0; i < harness.warnings.size(); i++) {
			Check(Contains(harness.warnings[i],
			               "attempt " + std::to_string(i + 1) + " of 4 failed: "),
			      what + ": " + harness.warnings[i]);
		}
		Check(harness.http.UnusedResponseCount() == 1, what + ": later responses are unused");

		CompatibilityModel model;
		Check(model.ApplyLoadResult(result) &&
		          model.StatusFor("PPSA01234").status == CompatibilityStatus::InGame &&
		          model.StatusFor(" Cusa00001").status == CompatibilityStatus::Logo,
		      what + ": the model shows the statuses");
	}
}

void TestEveryFailureKind() {
	for (const auto& [failure, name, description]: FailureCases()) {
		const std::string what = std::string("4 x ") + name;
		Harness           harness;
		for (int i = 0; i < 4; i++) {
			harness.http.AddResponse(kUrl, ResponseFor(failure));
		}
		harness.http.AddResponse(kUrl, ValidResponse()); // a fifth attempt never happens
		const auto& result = harness.Run();
		harness.CheckRequests(4, what);
		Check(result.source == Source::None && !result.database && result.attempts == 4,
		      what + ": no database");
		Check(Contains(result.last_failure, description),
		      what + ": the last failure: " + result.last_failure);
		Check(!ReadCache(harness.CachePath()), what + ": no cache is written");
		Check(harness.warnings.size() == 5, what + ": 4 attempt warnings and the final warning");
		for (int i = 0; i < 4; i++) {
			Check(Contains(harness.warnings[static_cast<size_t>(i)],
			               "attempt " + std::to_string(i + 1) +
			                   " of 4 failed: " + result.last_failure),
			      what + ": " + harness.warnings[static_cast<size_t>(i)]);
		}
		Check(harness.warnings.back() ==
		          "No compatibility database is available, every compatibility status is "
		          "Unknown; the last download attempt failed: " +
		              result.last_failure,
		      what + ": the no-cache warning: " + harness.warnings.back());
		Check(harness.http.UnusedResponseCount() == 1, what + ": at most 4 requests");

		CompatibilityModel model;
		Check(!model.ApplyLoadResult(result) &&
		          model.StatusFor("PPSA01234").status == CompatibilityStatus::Unknown,
		      what + ": every status is Unknown");
	}

	// Mixed failures: the last one is reported.
	Harness harness;
	harness.http.AddResponse(kUrl, ResponseFor(Failure::Timeout));
	harness.http.AddResponse(kUrl, ResponseFor(Failure::Unparseable));
	harness.http.AddResponse(kUrl, ResponseFor(Failure::NotAnObject));
	harness.http.AddResponse(kUrl, ResponseFor(Failure::Status));
	const auto& result = harness.Run();
	harness.CheckRequests(4, "mixed failures");
	Check(Contains(result.last_failure, "HTTP status 503"), "the last failure is the status");
	// Timeline: 15 s timeout, 750 ms, 40 ms, 1500 ms, 40 ms, 2250 ms, 40 ms.
	Check(harness.scheduler.Now() == milliseconds {15000 + 750 + 40 + 1500 + 40 + 2250 + 40},
	      "the attempts follow the schedule on the virtual clock");

	// No scripted response at all: network failures.
	Harness     unscripted;
	const auto& none = unscripted.Run();
	unscripted.CheckRequests(4, "unscripted");
	Check(Contains(none.last_failure, "network error: no scripted response"), none.last_failure);
}

void TestCache() {
	// The cache is replaced only by content that parsed.
	{
		Harness harness;
		harness.dir.WriteFile("compatibility_cache.json", kOtherValidBody);
		harness.http.AddResponse(kUrl, ResponseFor(Failure::Unparseable));
		harness.http.AddResponse(kUrl, ResponseFor(Failure::NotAnObject));
		harness.http.AddResponse(kUrl, ValidResponse());
		const auto& result = harness.Run();
		Check(result.source == Source::Download, "download after two bad bodies");
		Check(ReadCache(harness.CachePath()) == kValidBody, "the parsed body replaced the cache");
	}
	// Every attempt fails: a valid cache is delivered and keeps its bytes.
	{
		Harness harness;
		harness.dir.WriteFile("compatibility_cache.json", kOtherValidBody);
		for (int i = 0; i < 4; i++) {
			harness.http.AddResponse(
			    kUrl, ResponseFor(i % 2 == 0 ? Failure::Unparseable : Failure::Network));
		}
		const auto& result = harness.Run();
		harness.CheckRequests(4, "valid cache");
		Check(result.source == Source::Cache && result.database &&
		          *result.database == *kyty::core::ParseCompatibility(kOtherValidBody),
		      "the cached database is delivered");
		Check(Contains(result.last_failure, "network error"), "the last failure is kept");
		Check(ReadCache(harness.CachePath()) == kOtherValidBody, "the cache is unchanged");
		Check(harness.warnings.size() == 5 &&
		          Contains(harness.warnings.back(), "using the cached database") &&
		          Contains(harness.warnings.back(), harness.CachePath().string()),
		      "the fallback warning: " + harness.warnings.back());
		CompatibilityModel model;
		Check(model.ApplyLoadResult(result) &&
		          model.StatusFor("ppsa01234").status == CompatibilityStatus::DoesntBoot,
		      "the model shows the cached statuses");
	}
	// Invalid caches: Unknown statuses, a warning about the cache, the cache unchanged.
	for (const std::string& bytes:
	     {std::string("{\"PPSA1\""), std::string("[]"), std::string("\xFF\xFE"), std::string()}) {
		Harness harness;
		harness.dir.WriteFile("compatibility_cache.json", bytes);
		for (int i = 0; i < 4; i++) {
			harness.http.AddResponse(kUrl, ResponseFor(Failure::Status));
		}
		const auto& result = harness.Run();
		Check(result.source == Source::None && !result.database, "an invalid cache is not used");
		Check(harness.warnings.size() == 6, "attempt warnings, the cache warning, the final one");
		Check(Contains(harness.warnings[4], "The cached compatibility database") &&
		          Contains(harness.warnings[4], "does not parse"),
		      "the cache warning describes the parse error: " + harness.warnings[4]);
		Check(Contains(harness.warnings[5], "No compatibility database is available"),
		      "the final warning");
		Check(ReadCache(harness.CachePath()) == bytes, "the invalid cache is unchanged");
	}
	// An unreadable cache (a directory, a file over 16 MiB).
	for (const bool directory: {true, false}) {
		Harness harness;
		if (directory) {
			harness.dir.MakeDirectories("compatibility_cache.json");
		} else {
			harness.dir.WriteFile("compatibility_cache.json", std::string((16u << 20u) + 1u, ' '));
		}
		for (int i = 0; i < 4; i++) {
			harness.http.AddResponse(kUrl, ResponseFor(Failure::Network));
		}
		const auto& result = harness.Run();
		Check(result.source == Source::None, "an unreadable cache is not used");
		Check(harness.warnings.size() == 6 &&
		          Contains(harness.warnings[4], "Cannot read the cached compatibility database"),
		      "the cache read warning: " + harness.warnings[4]);
	}
}

void TestCacheWriteFailure() {
	// A fault in the atomic write: the database is still delivered, the old cache stays.
	for (const auto step: {kyty::core::AtomicFileStep::CreateTemp,
	                       kyty::core::AtomicFileStep::Write, kyty::core::AtomicFileStep::Rename}) {
		Harness harness;
		harness.dir.WriteFile("compatibility_cache.json", kOtherValidBody);
		harness.http.AddResponse(kUrl, ValidResponse());
		kyty::core::SetAtomicFileFaultForTesting(
		    [step](kyty::core::AtomicFileStep current) { return current == step ? ENOSPC : 0; });
		const auto& result = harness.Run();
		kyty::core::SetAtomicFileFaultForTesting({});
		Check(result.source == Source::Download && result.database, "delivered despite the fault");
		Check(harness.warnings.size() == 1 &&
		          Contains(harness.warnings[0], "Cannot replace the compatibility cache"),
		      "a cache write warning");
		Check(ReadCache(harness.CachePath()) == kOtherValidBody, "the old cache is unchanged");
	}
	// A data directory that does not exist.
	Harness harness;
	harness.MakeLoader(harness.dir.Path() / "missing");
	harness.http.AddResponse(kUrl, ValidResponse());
	const auto& result = harness.Run();
	Check(result.source == Source::Download && harness.warnings.size() == 1 &&
	          Contains(harness.warnings[0], "Cannot replace the compatibility cache"),
	      "a missing data directory is a warning only");
	Check(harness.loader->CachePath() ==
	          harness.dir.Path() / "missing" / kyty::core::kCompatibilityCacheFileName,
	      "CachePath()");
}

void TestEarlierStatusesKept() {
	CompatibilityModel model;
	model.Apply(*kyty::core::ParseCompatibility(kOtherValidBody));
	const auto earlier = model.Database();

	Harness harness;
	for (int i = 0; i < 4; i++) {
		harness.http.AddResponse(kUrl, ResponseFor(Failure::Unparseable));
	}
	const auto& failed = harness.Run();
	Check(!model.ApplyLoadResult(failed) && model.Database() == earlier,
	      "content that does not parse keeps the earlier statuses");

	Harness next;
	next.http.AddResponse(kUrl, ValidResponse());
	Check(model.ApplyLoadResult(next.Run()) &&
	          model.StatusFor("PPSA01234").status == CompatibilityStatus::InGame,
	      "a later valid download replaces them");
}

void TestLifecycle() {
	// Start() only once.
	{
		Harness harness;
		harness.http.AddResponse(kUrl, ValidResponse());
		harness.loader->Start();
		harness.loader->Start();
		Check(harness.http.Requests().size() == 1, "a second Start() sends nothing");
		Check(!harness.loader->Finished(), "not finished before the response");
		harness.scheduler.RunUntilIdle();
		Check(harness.results.size() == 1, "one result");
		harness.loader->Start();
		harness.scheduler.RunUntilIdle();
		Check(harness.http.Requests().size() == 1 && harness.results.size() == 1,
		      "Start() after the end sends nothing");
	}
	// Destroyed while a request is pending: the request is cancelled, no callback runs.
	{
		Harness harness;
		auto    response        = ValidResponse();
		response.chunk_interval = milliseconds {10};
		harness.http.AddResponse(kUrl, response);
		harness.loader->Start();
		harness.scheduler.AdvanceBy(milliseconds {26}); // only the first chunk arrived
		harness.loader.reset();
		harness.scheduler.RunUntilIdle();
		Check(harness.results.empty() && harness.warnings.empty(), "no callback after the end");
		Check(harness.http.ActiveCount() == 0 && harness.scheduler.PendingCount() == 0,
		      "the request was cancelled");
		Check(!ReadCache(harness.CachePath()), "no cache is written");
	}
	// Destroyed while waiting for a retry: the timer is cancelled.
	{
		Harness harness;
		harness.http.AddResponse(kUrl, ResponseFor(Failure::Network));
		harness.loader->Start();
		harness.scheduler.AdvanceBy(milliseconds {100});
		Check(harness.http.Requests().size() == 1 && harness.scheduler.PendingCount() == 1,
		      "waiting 750 ms for the retry");
		harness.loader.reset();
		Check(harness.scheduler.PendingCount() == 0, "the retry timer is cancelled");
		harness.scheduler.RunUntilIdle();
		Check(harness.http.Requests().size() == 1 && harness.results.empty(), "no retry");
	}
	// Without callbacks.
	{
		FakeScheduler       scheduler;
		FakeHttpClient      http(scheduler);
		const TempDir       dir("kyty_compat_loader");
		CompatibilityLoader loader(http, scheduler, dir.Path(), {});
		loader.Start();
		scheduler.RunUntilIdle();
		Check(loader.Finished() && http.Requests().size() == 4, "callbacks are optional");
	}
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("CompatibilityLoaderTests");
	TestRetrySchedule();
	TestSuccessOnAttempt();
	TestEveryFailureKind();
	TestCache();
	TestCacheWriteFailure();
	TestEarlierStatusesKept();
	TestLifecycle();
	std::printf("CompatibilityLoaderTests: all passed\n");
	return 0;
}
