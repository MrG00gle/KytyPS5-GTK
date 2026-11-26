// Example tests of the remote cheat file download (src/launcher/core/remoteCheats.h, task 16.2;
// Requirements 11.5, 11.17) with FakeHttpClient and FakeScheduler only (no network): an importable
// file, a file of another title ID, version, or executable (previewed, not importable), a file
// that does not parse, every failure kind (network, HTTP status, 8 MiB + 1 bytes, the 30 s limit
// on the virtual clock), exactly 8 MiB, MC4 and SHN files (tests/fixtures/cheats) converted to
// cheat JSON, a trainer that is not XML or uses addressing features, files without a URL, a newer
// Fetch cancelling the previous one, Cancel, destruction mid-download, and an unchanged local
// cheat file.

#include "launcher/core/atomicFile.h"
#include "launcher/core/cheats.h"
#include "launcher/core/remoteCheats.h"
#include "support/contentGenerators.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;

using kyty::core::CheatGame;
using kyty::core::kMaxRemoteCheatFileBytes;
using kyty::core::RemoteCheatFile;
using kyty::core::RemoteCheatResult;
using kyty::core::RemoteCheats;
using KytyTests::FakeHttpClient;
using KytyTests::FakeResponse;
using KytyTests::FakeScheduler;
using std::chrono::milliseconds;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "RemoteCheatsTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

const RemoteCheatFile kFile {"PPSA01234_01.000_HenCheats.json", "Game", "01.000", "json"};
const CheatGame       kGame {"ppsa01234", "01.000", "eboot.bin"};

std::string Url(const RemoteCheatFile& file = kFile) {
	return kyty::core::RemoteCheatFileUrl(file);
}

std::string ValidBody(std::string_view id = "PPSA01234", std::string_view version = "01.000",
                      std::string_view process = "eboot.bin") {
	return ContentGen::CheatDocumentText(
	    ContentGen::RemoteCheatSpec(id, version, process, {"Infinite HP", "Speed"}));
}

struct Harness {
	FakeScheduler                  scheduler;
	FakeHttpClient                 http {scheduler};
	std::vector<RemoteCheatResult> results;
	std::unique_ptr<RemoteCheats>  remote = std::make_unique<RemoteCheats>(
	    http, [this](const RemoteCheatResult& result) { results.push_back(result); });

	const RemoteCheatResult& Run(FakeResponse response, const RemoteCheatFile& file = kFile,
	                             const CheatGame& game = kGame) {
		const auto before = results.size();
		http.AddResponse(Url(file), std::move(response));
		Check(remote->Fetch(file, game).has_value(), "the fetch starts");
		Check(results.size() == before && remote->Fetching(),
		      "nothing is delivered inside Fetch()");
		scheduler.RunUntilIdle();
		Check(results.size() == before + 1 && !remote->Fetching(), "one result per Fetch");
		return results.back();
	}
};

void TestImportable() {
	Harness     harness;
	const auto& result = harness.Run(ContentGen::ResponseOf(ValidBody(), {10, 100}));
	Check(result.importable && result.error.empty(), "a matching file is importable");
	Check(!result.problem && result.fetch.failure == kyty::core::HttpFailure::None,
	      "no problem and no download failure (phase 9 V1)");
	Check(result.file == kFile, "the result names the file");
	Check(result.document && result.document->cheats.size() == 2 &&
	          result.document->cheats[0].name == "Infinite HP" &&
	          result.document->credits == std::vector<std::string> {"Tester"},
	      "the document for the preview");
	Check(harness.http.Requests().size() == 1, "one request");
	const auto& request = harness.http.Requests()[0];
	Check(request.url == "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/"
	                     "cheats/json/PPSA01234_01.000_HenCheats.json",
	      "the file URL: " + request.url);
	Check(request.max_bytes == 8u * 1024u * 1024u &&
	          request.total_timeout == milliseconds {30000} &&
	          request.stall_timeout == milliseconds {0},
	      "8 MiB, 30 s, no stall limit");
	Check(request.redirects == kyty::core::RedirectPolicy::SameOrigin,
	      "same-origin redirects only (the Qt launcher's SameOriginRedirectPolicy)");
}

void TestMismatches() {
	struct Case {
		std::string body;
		std::string error;
	};
	for (const auto& [body, error]: std::vector<Case> {
	         {ValidBody("PPSA05678"), "The cheat file does not match this game's title ID."},
	         {ValidBody("PPSA01234", "01.001"),
	          "The cheat file does not match this game's version (01.000)."},
	         {ValidBody("PPSA01234", "01.000", "game.elf"),
	          "The cheat file does not match this game's executable (eboot.bin)."},
	     }) {
		Harness     harness;
		const auto& result = harness.Run(ContentGen::ResponseOf(body));
		Check(!result.importable && result.error == error && result.document.has_value(),
		      "previewed but not importable: " + result.error);
		Check(result.problem && result.problem->message == error && result.problem->file.empty(),
		      "the mismatch is the result's problem: " + error);
	}

	Harness     harness;
	const auto& result = harness.Run(ContentGen::ResponseOf("{\"mods\":[{\"name\":1}]}"));
	Check(!result.importable && !result.document &&
	          result.error == kFile.name + ": Invalid cheat entry 1 (it has no name).",
	      "a file that does not parse names the file: " + result.error);
	Check(result.problem &&
	          result.problem->kind == kyty::core::CheatProblemKind::EntryWithoutName &&
	          result.problem->file == kFile.name && result.problem->message == result.error,
	      "the parse problem names the file");
}

void TestFailures() {
	FakeResponse network;
	network.network_failure = true;
	network.failure_message = "host not found";
	FakeResponse status;
	status.status              = 404;
	FakeResponse late          = ContentGen::ResponseOf(ValidBody());
	late.latency               = milliseconds {30000};
	FakeResponse slow_chunks   = ContentGen::ResponseOf(ValidBody(), {1, 1});
	slow_chunks.latency        = milliseconds {15000};
	slow_chunks.chunk_interval = milliseconds {7500};
	for (const auto& [response, error]: std::vector<std::pair<FakeResponse, std::string>> {
	         {network, "Cheat download failed: host not found."},
	         {status, "Unexpected response from the cheat repository (HTTP 404)."},
	         {ContentGen::ResponseOf(std::string(kMaxRemoteCheatFileBytes + 1, ' ')),
	          "Cheat download exceeds the size limit of 8 MiB."},
	         {late, "Cheat download timed out after 30 seconds."},
	         {slow_chunks, "Cheat download timed out after 30 seconds."},
	     }) {
		Harness     harness;
		const auto& result = harness.Run(response);
		Check(!result.importable && !result.document && result.error == error,
		      "a failed download: " + result.error);
		Check(result.fetch.failure != kyty::core::HttpFailure::None &&
		          result.fetch.message == error && !result.problem,
		      "the download failure's parts: " + error);
	}

	// Exactly 8 MiB, delivered just before the limit, is accepted.
	auto body          = ContentGen::BodyOfSize("{\"pad\":\"", "\"," + ValidBody().substr(1),
	                                            kMaxRemoteCheatFileBytes, 'x');
	auto big           = ContentGen::ResponseOf(std::move(body), {1u << 20u, 3u << 20u});
	big.latency        = milliseconds {9999};
	big.chunk_interval = milliseconds {10000};
	Harness     harness;
	const auto& result = harness.Run(big);
	Check(result.importable, "8 MiB in 29.999 s is accepted: " + result.error);
}

void TestUnsupported() {
	Harness harness;
	for (const auto& file: {RemoteCheatFile {"PPSA01234_01.000_a.txt", "", "01.000", "txt"},
	                        RemoteCheatFile {"PPSA01234_01.000_a.mc4", "", "01.000", "shn"},
	                        RemoteCheatFile {"not a name.json", "", "", "json"}}) {
		const auto started = harness.remote->Fetch(file, kGame);
		Check(!started && started.error() == "This cheat file cannot be downloaded.",
		      "no download of " + file.name);
	}
	harness.scheduler.RunUntilIdle();
	Check(harness.http.Requests().empty() && harness.results.empty(), "nothing was requested");
}

std::string Fixture(std::string_view name) {
	const auto path =
	    std::filesystem::path(KYTY_TEST_SOURCE_DIR) / "tests/fixtures/cheats" / std::string(name);
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	Check(bytes.has_value(), "read " + path.string());
	return *bytes;
}

// MC4 and SHN files (upstream KytyPS5 commit 91c5820c): downloaded from their own folders of the
// collection, converted to cheat JSON, parsed, and matched against the game like JSON files.
void TestTrainers() {
	const CheatGame alpha {"PPSA01001", "01.000", "eboot.bin"};
	for (const auto& [format, names]: std::vector<std::pair<std::string, std::string>> {
	         {"shn", "Infinite Health, Max Coins & Gems"},
	         {"mc4", "Moon Jump, Unlimited Time, Freeze Enemies"}}) {
		const RemoteCheatFile file {"PPSA01001_01.000_alpha." + format, "Alpha Game", "01.000",
		                            format};
		Harness               harness;
		const auto            result =
		    harness.Run(ContentGen::ResponseOf(Fixture(file.name), {100, 1000}), file, alpha);
		Check(harness.http.Requests().size() == 1 &&
		          harness.http.Requests()[0].url ==
		              "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/"
		              "cheats/" +
		                  format + "/" + file.name,
		      format + ": the file's URL");
		Check(result.importable && result.error.empty() && !result.problem,
		      format + ": importable: " + result.error);
		std::string listed;
		for (const auto& cheat: result.document->cheats) {
			listed += (listed.empty() ? "" : ", ") + cheat.name;
		}
		Check(listed == names && result.document->id == "PPSA01001" &&
		          result.document->credits == std::vector<std::string> {"Kyty Tests"},
		      format + ": the converted cheats: " + listed);
		const auto other = harness.Run(ContentGen::ResponseOf(Fixture(file.name)), file, kGame);
		Check(!other.importable && other.document.has_value() &&
		          other.error == "The cheat file does not match this game's title ID.",
		      format + ": previewed, not importable for another game");
	}

	const RemoteCheatFile shn {"PPSA01234_01.000_a.shn", "", "01.000", "shn"};
	Harness               harness;
	const auto            invalid = harness.Run(ContentGen::ResponseOf("{\"mods\":[]}"), shn);
	Check(!invalid.importable && !invalid.document &&
	          invalid.error == "PPSA01234_01.000_a.shn: Invalid cheat XML." && invalid.problem &&
	          invalid.problem->kind == kyty::core::CheatProblemKind::InvalidXml &&
	          invalid.problem->file == shn.name,
	      "a trainer that is not XML: " + invalid.error);
	const auto section = harness.Run(
	    ContentGen::ResponseOf(
	        R"(<Trainer Cusa="PPSA01234" Version="01.000" Process="EBOOT.BIN"><Cheat Text="A">)"
	        R"(<Cheatline><Offset>0x10</Offset><Section>3</Section><ValueOn>90</ValueOn>)"
	        R"(<ValueOff>C3</ValueOff></Cheatline></Cheat></Trainer>)"),
	    shn);
	Check(!section.importable && section.document.has_value() &&
	          section.error ==
	              "This cheat file uses memory addressing features that are not supported yet.",
	      "a section-relative write is previewed, not importable: " + section.error);
	const RemoteCheatFile mc4 {"PPSA01234_01.000_a.mc4", "", "01.000", "mc4"};
	const auto            garbage = harness.Run(ContentGen::ResponseOf("not base64 of XML"), mc4);
	Check(!garbage.document && garbage.problem &&
	          garbage.problem->kind == kyty::core::CheatProblemKind::InvalidXml,
	      "an MC4 file that does not decrypt to a trainer");
}

// A KytyPS5-Modules file (upstream KytyPS5 commit d2ec91b8): downloaded from its catalog's URL; a
// file without "process" matches any executable of the game.
void TestModulesFile() {
	RemoteCheatFile file {"Unlock everything", "Game", "01.000", "json"};
	file.kind = kyty::core::RemoteCheatKind::Patch;
	file.url  = "https://kytyps5.github.io/KytyPS5-Modules/patches/json/PPSA01234/unlock.json";
	Harness    harness;
	const auto result = harness.Run(
	    ContentGen::ResponseOf(R"({"id":"PPSA01234","version":"01.000","name":"Game",)"
	                           R"("credits":["Modules"],"mods":[{"name":"Unlock all","memory":[)"
	                           R"({"offset":"0x40","on":"01","off":"00"}]}]})"),
	    file, CheatGame {"PPSA01234", "01.000", "game.elf"});
	Check(harness.http.Requests().size() == 1 && harness.http.Requests()[0].url == file.url,
	      "the file's own URL");
	Check(harness.http.Requests()[0].redirects == kyty::core::RedirectPolicy::SameOrigin &&
	          harness.http.Requests()[0].max_bytes == kMaxRemoteCheatFileBytes,
	      "the limits of every remote file");
	Check(result.importable && result.file == file && result.document &&
	          result.document->cheats.size() == 1 && result.document->process.empty(),
	      "a file without \"process\" is importable: " + result.error);
}

void TestNewerFetch() {
	Harness harness;
	auto    first = ContentGen::ResponseOf(ValidBody());
	first.latency = milliseconds {5000};
	const RemoteCheatFile second_file {"PPSA01234_01.000_Other.json", "", "01.000", "json"};
	harness.http.AddResponse(Url(), first);
	harness.http.AddResponse(Url(second_file), ContentGen::ResponseOf(ValidBody()));
	Check(harness.remote->Fetch(kFile, kGame).has_value(), "first fetch");
	harness.scheduler.AdvanceBy(milliseconds {100});
	Check(harness.remote->Fetch(second_file, kGame).has_value(), "second fetch");
	harness.scheduler.RunUntilIdle();
	Check(harness.results.size() == 1 && harness.results[0].file == second_file,
	      "only the newer fetch is delivered");
	Check(harness.http.ActiveCount() == 0, "the first request was cancelled");

	// An unsupported file also cancels the pending download.
	Harness unsupported;
	unsupported.http.AddResponse(Url(), first);
	Check(unsupported.remote->Fetch(kFile, kGame).has_value(), "fetch");
	Check(!unsupported.remote->Fetch({"not a name.json", "", "01.000", "json"}, kGame),
	      "unsupported");
	Check(!unsupported.remote->Fetching(), "nothing pending");
	unsupported.scheduler.RunUntilIdle();
	Check(unsupported.results.empty(), "the cancelled download is not delivered");

	Harness cancelled;
	cancelled.http.AddResponse(Url(), first);
	Check(cancelled.remote->Fetch(kFile, kGame).has_value(), "fetch");
	cancelled.remote->Cancel();
	cancelled.scheduler.RunUntilIdle();
	Check(cancelled.results.empty() && cancelled.http.ActiveCount() == 0, "Cancel");

	Harness destroyed;
	destroyed.http.AddResponse(Url(), first);
	Check(destroyed.remote->Fetch(kFile, kGame).has_value(), "fetch");
	destroyed.scheduler.AdvanceBy(milliseconds {100});
	destroyed.remote.reset();
	destroyed.scheduler.RunUntilIdle();
	Check(destroyed.results.empty() && destroyed.http.ActiveCount() == 0,
	      "no callback after destruction");

	FakeScheduler  scheduler;
	FakeHttpClient http(scheduler);
	RemoteCheats   quiet(http, {});
	http.AddResponse(Url(), ContentGen::ResponseOf(ValidBody()));
	Check(quiet.Fetch(kFile, kGame).has_value(), "fetch without a callback");
	scheduler.RunUntilIdle();
	Check(!quiet.Fetching(), "the callback is optional");
}

void TestLocalFileUnchanged() {
	// A rejected download writes nothing; the import is a separate, explicit step.
	const KytyTests::TempDir temp("kyty_remote_cheats");
	const auto               local = temp.WriteFile("_Patches/PPSA01234.json", "{\"mods\":[]}");
	Harness                  harness;
	const auto&              rejected = harness.Run(ContentGen::ResponseOf(ValidBody("PPSA05678")));
	Check(!rejected.importable, "rejected");
	const auto& accepted = harness.Run(ContentGen::ResponseOf(ValidBody()));
	Check(accepted.importable, "accepted");
	const auto bytes = kyty::core::ReadFileLimited(local, 1024);
	Check(bytes && *bytes == "{\"mods\":[]}", "the local cheat file is unchanged");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("RemoteCheatsTests");
	TestImportable();
	TestMismatches();
	TestFailures();
	TestUnsupported();
	TestTrainers();
	TestModulesFile();
	TestNewerFetch();
	TestLocalFileUnchanged();
	std::printf("RemoteCheatsTests: all tests passed\n");
	return 0;
}
