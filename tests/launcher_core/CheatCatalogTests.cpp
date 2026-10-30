// Example tests of the HEN Cheats Collection file list (src/launcher/core/cheatCatalog.h, task
// 16.2; Requirements 11.4, 11.17) with FakeHttpClient and FakeScheduler only (no network): the
// URLs and the collection's name pattern, index parsing as Qt's ParseIndex, the request limits
// (2 MiB, 30 s, no stall limit), the merged and sorted result of the three indexes, every failure
// kind (network, HTTP status, 2 MiB + 1 bytes, the 30 s limit on the virtual clock), a client that
// ignores max_bytes, a new Load cancelling the previous one, Cancel, and destruction mid-run.

#include "launcher/core/cheatCatalog.h"
#include "support/contentGenerators.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/noDisplay.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;

using kyty::core::CheatCatalog;
using kyty::core::CheatCatalogResult;
using kyty::core::CheatIndexUrl;
using kyty::core::HttpFailure;
using kyty::core::HttpResult;
using kyty::core::kMaxCheatIndexBytes;
using kyty::core::ParseCheatIndex;
using kyty::core::RemoteCheatFile;
using KytyTests::FakeHttpClient;
using KytyTests::FakeResponse;
using KytyTests::FakeScheduler;
using std::chrono::milliseconds;

constexpr char kBase[] = "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/"
                         "cheats/";

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "CheatCatalogTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Names(const std::vector<RemoteCheatFile>& files) {
	std::string names;
	for (const auto& file: files) {
		names += (names.empty() ? "" : " ") + file.name;
	}
	return names;
}

void TestUrls() {
	Check(CheatIndexUrl("json") == std::string(kBase) + "json.txt", "json index");
	Check(CheatIndexUrl("mc4") == std::string(kBase) + "mc4.txt", "mc4 index");
	Check(CheatIndexUrl("shn") == std::string(kBase) + "shn.txt", "shn index");
	Check(CheatIndexUrl("JSON").empty() && CheatIndexUrl("txt").empty(), "unknown formats");

	const RemoteCheatFile json {"PPSA01234_01.000_HenCheats.json", "", "01.000", "json"};
	Check(kyty::core::RemoteCheatFileUrl(json) == std::string(kBase) + "json/" + json.name,
	      "a JSON file");
	Check(kyty::core::IsSupportedRemoteCheatFile(json), "JSON is supported");
	const RemoteCheatFile mc4 {"PPSA01234_1.0_a-b.c_d.mc4", "", "1.0", "mc4"};
	Check(kyty::core::RemoteCheatFileUrl(mc4) == std::string(kBase) + "mc4/" + mc4.name,
	      "an mc4 file");
	Check(!kyty::core::IsSupportedRemoteCheatFile(mc4), "mc4 is not supported");
	for (const auto& [name, format]: std::vector<std::pair<std::string, std::string>> {
	         {"PPSA01234_01.000_x.json", "mc4"}, // extension and format differ
	         {"PPSA01234_01.000_x.json", "txt"},
	         {"ppsa01234_01.000_x.json", "json"},
	         {"PPSA0123_01.000_x.json", "json"},
	         {"PPSA012345_01.000_x.json", "json"},
	         {"PPSA01234_01_x.json", "json"}, // a version needs a dot
	         {"PPSA01234_01._x.json", "json"},
	         {"PPSA01234_.01_x.json", "json"},
	         {"PPSA01234_1a.0_x.json", "json"},
	         {"PPSA01234_01.000_.json", "json"},
	         {"PPSA01234_01.000_x y.json", "json"},
	         {"PPSA01234_01.000_x/y.json", "json"},
	         {"PPSA01234_01.000_x.JSON", "json"},
	         {"PPSA01234_01.000_x.json ", "json"},
	         {"PPSA01234_01.000x.json", "json"},
	         {"PPSA01234-01.000_x.json", "json"},
	         {"PPSA01234", "json"},
	         {"", "json"},
	     }) {
		const RemoteCheatFile file {name, "", "", format};
		Check(kyty::core::RemoteCheatFileUrl(file).empty() &&
		          !kyty::core::IsSupportedRemoteCheatFile(file),
		      "no URL for '" + name + "' (" + format + ")");
	}
	const RemoteCheatFile versions {"CUSA00001_1.2.3_x_y.json.json", "", "", "json"};
	Check(!kyty::core::RemoteCheatFileUrl(versions).empty(),
	      "several version parts, '_' and '.' in the rest");
}

void TestParseIndex() {
	const std::string bytes = "\xEF\xBB\xBF"
	                          "PPSA01234_01.000_HenCheats.json=Game Title (EU)\r\n"
	                          "  PPSA01234_01.001_HenCheats.json = Other  \n"
	                          "\n"
	                          "PPSA05678_01.000_HenCheats.json=Another game\n"
	                          "ppsa01234_01.000_lower.json=lower case ID\n"
	                          "PPSA01234_01.000_HenCheats.json=duplicate\n"
	                          "PPSA01234_01.000_x.mc4=wrong format\n"
	                          "no separator line\n"
	                          "PPSA01234_01.002_t.json=\xFF bad byte\n"
	                          "PPSA01234_01.003_e.json=\n"
	                          "\xE3\x80\x80PPSA01234_01.004_w.json=ideographic space\xE3\x80\x80";
	const auto        index = ParseCheatIndex(bytes, "json", " ppsa01234 ");
	Check(Names(index.files) == "PPSA01234_01.000_HenCheats.json PPSA01234_01.002_t.json "
	                            "PPSA01234_01.003_e.json PPSA01234_01.004_w.json",
	      "the title's files in index order: " + Names(index.files));
	Check(index.files[0].title == "Game Title (EU)" && index.files[0].version == "01.000" &&
	          index.files[0].format == "json",
	      "the first file");
	Check(index.files[1].title == "\xEF\xBF\xBD bad byte", "ill-formed UTF-8 becomes U+FFFD");
	Check(index.files[2].title.empty(), "an empty title");
	Check(index.files[3].title == "ideographic space", "Unicode whitespace is trimmed");
	// Invalid: "name " (space before '='), lower-case ID, wrong format, no separator.
	Check(index.error == "Skipped 4 invalid cheat index entries.", "invalid lines: " + index.error);

	Check(ParseCheatIndex("PPSA01234_1.0_a.json=x", "json", "PPSA01234").error.empty(),
	      "a clean index has no error");
	for (const auto& [data, format]: std::vector<std::pair<std::string, std::string>> {
	         {"", "json"},
	         {"PPSA01234_1.0_a.json=x", "txt"},
	         {std::string(kMaxCheatIndexBytes + 1, '\n'), "json"},
	     }) {
		const auto refused = ParseCheatIndex(data, format, "PPSA01234");
		Check(refused.files.empty() && refused.error == "Invalid cheat index.",
		      "refused index (" + format + ", " + std::to_string(data.size()) + " bytes)");
	}
	Check(
	    ParseCheatIndex(std::string(kMaxCheatIndexBytes, '\n'), "json", "PPSA01234").error.empty(),
	    "exactly 2 MiB is accepted");
	const auto mc4 =
	    ParseCheatIndex("PPSA01234_1.0_a.mc4=x\nPPSA01234_1.0_a.json=y", "mc4", "PPSA01234");
	Check(Names(mc4.files) == "PPSA01234_1.0_a.mc4" && mc4.files[0].format == "mc4" &&
	          mc4.error == "Skipped 1 invalid cheat index entries.",
	      "an mc4 index");
}

void TestOrdering() {
	std::vector<RemoteCheatFile> files = {
	    {"a", "", "01.000", "json"}, {"b", "", "02.000", "mc4"},  {"c", "", "01.000", "mc4"},
	    {"d", "", "02.000", "json"}, {"e", "", "01.000", "json"},
	};
	kyty::core::PutInstalledVersionFirst(files, "01.000");
	Check(Names(files) == "a c e b d", "installed version first, order kept: " + Names(files));
	kyty::core::PutInstalledVersionFirst(files, "09.000");
	Check(Names(files) == "a c e b d", "no installed version: unchanged");
}

void TestFailureTexts() {
	using kyty::core::DescribeCheatFetchFailure;
	const auto text = [](HttpFailure failure, int status, std::string message, uint64_t limit) {
		return DescribeCheatFetchFailure(HttpResult {failure, status, std::move(message)}, limit);
	};
	Check(text(HttpFailure::None, 200, "", kMaxCheatIndexBytes).empty(), "success");
	Check(text(HttpFailure::Network, 0, "no route", 1) == "Cheat download failed: no route.",
	      "network");
	Check(text(HttpFailure::Status, 404, "", 1) ==
	          "Unexpected response from the cheat repository (HTTP 404).",
	      "status");
	Check(text(HttpFailure::TooLarge, 200, "", kMaxCheatIndexBytes) ==
	          "Cheat download exceeds the size limit of 2 MiB.",
	      "too large");
	Check(text(HttpFailure::TimedOut, 0, "", 1) == "Cheat download timed out after 30 seconds.",
	      "timed out");
	Check(text(HttpFailure::Stalled, 200, "idle", 1) == "Cheat download stalled: idle.", "stalled");
	Check(text(HttpFailure::Cancelled, 0, "", 1) == "Cheat download was cancelled.", "cancelled");
}

std::string IndexBody(const std::vector<std::string>& lines) {
	std::string body;
	for (const auto& line: lines) {
		body += line + "\n";
	}
	return body;
}

struct Harness {
	FakeScheduler                   scheduler;
	FakeHttpClient                  http {scheduler};
	std::vector<CheatCatalogResult> results;
	std::unique_ptr<CheatCatalog>   catalog = std::make_unique<CheatCatalog>(
	    http, [this](const CheatCatalogResult& result) { results.push_back(result); });

	void Add(std::string_view format, FakeResponse response) {
		http.AddResponse(CheatIndexUrl(format), std::move(response));
	}

	const CheatCatalogResult& Run(std::string_view title_id) {
		catalog->Load(title_id);
		Check(results.empty() && catalog->Loading(), "nothing is delivered inside Load()");
		scheduler.RunUntilIdle();
		Check(results.size() == 1 && !catalog->Loading(), "one result per Load");
		return results.back();
	}
};

void TestLoad() {
	Harness harness;
	harness.Add("json", ContentGen::ResponseOf(
	                        IndexBody({"PPSA01234_01.000_a.json=A", "PPSA01234_02.000_b.json=B",
	                                   "PPSA09999_01.000_z.json=other"})));
	harness.Add(
	    "mc4", ContentGen::ResponseOf(IndexBody({"PPSA01234_01.000_a.mc4=A", "bad line"}), {3, 5}));
	harness.Add("shn", ContentGen::ResponseOf(IndexBody({"PPSA01234_02.000_c.shn=C"})));
	const auto& result = harness.Run("ppsa01234");
	Check(Names(result.files) ==
	          "PPSA01234_02.000_b.json PPSA01234_02.000_c.shn PPSA01234_01.000_a.json "
	          "PPSA01234_01.000_a.mc4",
	      "merged and sorted by version, format, name: " + Names(result.files));
	Check(result.error == "MC4: Skipped 1 invalid cheat index entries.", "error: " + result.error);

	const auto& requests = harness.http.Requests();
	Check(requests.size() == 3, "three index requests");
	for (std::size_t i = 0; i < requests.size(); i++) {
		const auto& request = requests[i];
		Check(request.url == CheatIndexUrl(kyty::core::kCheatFormats[i]), "in format order");
		Check(request.max_bytes == 2u * 1024u * 1024u, "2 MiB per index");
		Check(request.total_timeout == milliseconds {30000}, "30 s per request");
		Check(request.stall_timeout == milliseconds {0}, "no stall limit");
		Check(request.headers.size() == 1 && request.headers[0].first == "User-Agent" &&
		          request.headers[0].second == "Kyty-Launcher",
		      "the Qt user agent");
	}
}

void TestFailures() {
	Harness      harness;
	FakeResponse network;
	network.network_failure = true;
	network.failure_message = "connection refused";
	harness.Add("json", network);
	FakeResponse status;
	status.status = 503;
	harness.Add("mc4", status);
	harness.Add("shn", ContentGen::ResponseOf(std::string(kMaxCheatIndexBytes + 1, '\n')));
	const auto& result = harness.Run("PPSA01234");
	Check(result.files.empty(), "no files");
	Check(result.error == "JSON: Cheat download failed: connection refused.\n"
	                      "MC4: Unexpected response from the cheat repository (HTTP 503).\n"
	                      "SHN: Cheat download exceeds the size limit of 2 MiB.",
	      "one line per failed index, in format order: " + result.error);

	// Exactly 2 MiB is accepted; an empty body is an invalid index.
	Harness limits;
	limits.Add("json", ContentGen::ResponseOf(ContentGen::BodyOfSize("PPSA01234_1.0_a.json=A\n", "",
	                                                                 kMaxCheatIndexBytes, '\n')));
	limits.Add("mc4", ContentGen::ResponseOf(""));
	FakeResponse created = ContentGen::ResponseOf("PPSA01234_1.0_b.shn=B");
	created.status       = 203;
	limits.Add("shn", created);
	const auto& accepted = limits.Run("PPSA01234");
	Check(Names(accepted.files) == "PPSA01234_1.0_a.json PPSA01234_1.0_b.shn",
	      "2 MiB and a 2xx status are accepted: " + Names(accepted.files));
	Check(accepted.error == "MC4: Invalid cheat index.", "an empty index: " + accepted.error);
}

void TestTimeout() {
	// Each index ends at its own time; 30 s or more after Load is too late.
	for (const auto& [latency, interval, ok]:
	     std::vector<std::tuple<int, int, bool>> {{29999, 0, true},
	                                              {30000, 0, false},
	                                              {10000, 9999, true},
	                                              {10000, 10000, false},
	                                              {0, 20000, false}}) {
		Harness      harness;
		FakeResponse response =
		    ContentGen::ResponseOf("PPSA01234_1.0_a.json=A\nPPSA01234_1.0_b.json=B\n"
		                           "PPSA01234_1.0_c.json=C\n",
		                           {24, 24});
		response.latency        = milliseconds {latency};
		response.chunk_interval = milliseconds {interval};
		harness.Add("json", response);
		harness.Add("mc4", ContentGen::ResponseOf("\n"));
		harness.Add("shn", ContentGen::ResponseOf("\n"));
		const auto what =
		    "latency " + std::to_string(latency) + " interval " + std::to_string(interval);
		const auto& result = harness.Run("PPSA01234");
		if (ok) {
			Check(result.files.size() == 3 && result.error.empty(), what + " is in time");
		} else {
			Check(result.files.empty() &&
			          result.error == "JSON: Cheat download timed out after 30 seconds.",
			      what + " is too late: " + result.error);
		}
		Check(harness.scheduler.Now() <= milliseconds {30000}, "nothing waits past 30 s");
	}
}

// Forwards to a FakeHttpClient without max_bytes, as a client that does not apply the limit.
class NoLimitClient final: public kyty::core::HttpClient {
public:
	explicit NoLimitClient(FakeHttpClient& inner): m_inner(inner) {}

	kyty::core::RequestId Get(const kyty::core::HttpRequest& request, ChunkFn on_chunk,
	                          DoneFn on_done) override {
		auto copy      = request;
		copy.max_bytes = 0;
		return m_inner.Get(copy, std::move(on_chunk), std::move(on_done));
	}
	void Cancel(kyty::core::RequestId id) override { m_inner.Cancel(id); }

private:
	FakeHttpClient& m_inner;
};

void TestClientSideLimit() {
	FakeScheduler                   scheduler;
	FakeHttpClient                  inner(scheduler);
	NoLimitClient                   http(inner);
	std::vector<CheatCatalogResult> results;
	CheatCatalog catalog(http,
	                     [&](const CheatCatalogResult& result) { results.push_back(result); });
	inner.AddResponse(CheatIndexUrl("json"),
	                  ContentGen::ResponseOf(std::string(kMaxCheatIndexBytes + 1, '\n'),
	                                         {kMaxCheatIndexBytes, 1}));
	inner.AddResponse(CheatIndexUrl("mc4"),
	                  ContentGen::ResponseOf(std::string(kMaxCheatIndexBytes, '\n'), {1000}));
	inner.AddResponse(CheatIndexUrl("shn"), ContentGen::ResponseOf("\n"));
	catalog.Load("PPSA01234");
	scheduler.RunUntilIdle();
	Check(results.size() == 1 &&
	          results[0].error == "JSON: Cheat download exceeds the size limit of 2 MiB.",
	      "the body is checked as it arrives: " + (results.empty() ? "" : results[0].error));
}

void TestReload() {
	// A new Load cancels the previous one, whose result never comes.
	Harness harness;
	auto    slow = ContentGen::ResponseOf("PPSA01234_1.0_old.json=old\n");
	slow.latency = milliseconds {5000};
	harness.Add("json", slow);
	harness.Add("mc4", slow);
	harness.Add("shn", slow);
	harness.catalog->Load("PPSA01234");
	harness.scheduler.AdvanceBy(milliseconds {100});
	harness.Add("json", ContentGen::ResponseOf("PPSA05678_1.0_new.json=new\n"));
	harness.Add("mc4", ContentGen::ResponseOf("\n"));
	harness.Add("shn", ContentGen::ResponseOf("\n"));
	harness.catalog->Load("PPSA05678");
	harness.scheduler.RunUntilIdle();
	Check(harness.results.size() == 1 &&
	          Names(harness.results[0].files) == "PPSA05678_1.0_new.json",
	      "only the newer Load is delivered");
	Check(harness.http.Requests().size() == 6 && harness.http.ActiveCount() == 0,
	      "the first three requests were cancelled");

	// Cancel: no result.
	Harness cancelled;
	cancelled.Add("json", slow);
	cancelled.Add("mc4", slow);
	cancelled.Add("shn", slow);
	cancelled.catalog->Load("PPSA01234");
	cancelled.catalog->Cancel();
	Check(!cancelled.catalog->Loading(), "not loading after Cancel");
	cancelled.scheduler.RunUntilIdle();
	Check(cancelled.results.empty() && cancelled.http.ActiveCount() == 0, "no result after Cancel");

	// Destroyed mid-run: the requests are cancelled and nothing calls back.
	Harness destroyed;
	destroyed.Add("json", slow);
	destroyed.Add("mc4", slow);
	destroyed.Add("shn", slow);
	destroyed.catalog->Load("PPSA01234");
	destroyed.scheduler.AdvanceBy(milliseconds {100});
	destroyed.catalog.reset();
	destroyed.scheduler.RunUntilIdle();
	Check(destroyed.results.empty() && destroyed.http.ActiveCount() == 0,
	      "no callback after destruction");

	// A Load from inside on_loaded starts the next one.
	FakeScheduler                   scheduler;
	FakeHttpClient                  http(scheduler);
	std::vector<CheatCatalogResult> results;
	std::unique_ptr<CheatCatalog>   catalog;
	catalog = std::make_unique<CheatCatalog>(http, [&](const CheatCatalogResult& result) {
		results.push_back(result);
		if (results.size() == 1) {
			catalog->Load("PPSA01234");
		}
	});
	for (int i = 0; i < 2; i++) {
		for (const auto format: kyty::core::kCheatFormats) {
			http.AddResponse(CheatIndexUrl(format), ContentGen::ResponseOf("\n"));
		}
	}
	catalog->Load("PPSA01234");
	scheduler.RunUntilIdle();
	Check(results.size() == 2 && http.Requests().size() == 6, "a reload from the callback");

	// Without a callback.
	FakeScheduler  quiet_scheduler;
	FakeHttpClient quiet_http(quiet_scheduler);
	CheatCatalog   quiet(quiet_http, {});
	quiet.Load("PPSA01234");
	quiet_scheduler.RunUntilIdle();
	Check(!quiet.Loading() && quiet_http.Requests().size() == 3, "the callback is optional");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("CheatCatalogTests");
	TestUrls();
	TestParseIndex();
	TestOrdering();
	TestFailureTexts();
	TestLoad();
	TestFailures();
	TestTimeout();
	TestClientSideLimit();
	TestReload();
	std::printf("CheatCatalogTests: all tests passed\n");
	return 0;
}
