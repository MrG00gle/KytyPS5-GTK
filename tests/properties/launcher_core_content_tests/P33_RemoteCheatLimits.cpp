// Feature: linux-gtk-modernization, Property 33: Remote cheat fetch limits and matching
//
// Validates Requirement 11.17. Each case scripts one FakeHttpClient response, on a FakeScheduler
// virtual clock, for either one of the three file list indexes (CheatCatalog) or a cheat file
// (RemoteCheats): a body far below or within a few bytes of the limit (2 MiB for an index, 8 MiB
// for a file), split into random chunks; a latency and chunk interval far from or right at the
// 30 s limit; HTTP status 200, another 2xx, or a failure status; or a network failure. A cheat
// file's id, version, or process may not match the game. The fetch must be accepted exactly when
// it ends before 30 s, with a 2xx status, within the size limit; the requests must carry those
// limits and no stall limit; a rejection must state its reason (the first limit hit on the
// clock); a file must be importable exactly when it was accepted and matches; and the local cheat
// file in a real Data_Directory must be byte-identical afterwards.

#include "launcher/core/atomicFile.h"
#include "launcher/core/cheatCatalog.h"
#include "launcher/core/cheats.h"
#include "launcher/core/remoteCheats.h"
#include "support/contentGenerators.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;
using std::chrono::milliseconds;

constexpr int64_t kLimitMs = 30000;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& Pick(const std::vector<T>& values) {
	return values[Choose<std::size_t>(0, values.size())];
}

// The chunks FakeHttpClient delivers for a body (fakeHttpClient.h).
std::vector<std::size_t> Deliveries(std::size_t body_size, const std::vector<std::size_t>& sizes) {
	std::vector<std::size_t> chunks;
	std::size_t              delivered = 0;
	std::size_t              next      = 0;
	while (delivered < body_size) {
		while (next < sizes.size() && sizes[next] == 0) {
			next++;
		}
		std::size_t size = body_size - delivered;
		if (next < sizes.size()) {
			size = std::min(size, sizes[next]);
			next++;
		}
		chunks.push_back(size);
		delivered += size;
	}
	return chunks;
}

enum class Outcome { Accepted, Network, Status, TooLarge, TimedOut };

// When and how the scripted request ends.
Outcome Expect(const KytyTests::FakeResponse& response, uint64_t limit) {
	const int64_t latency = response.latency.count();
	if (response.network_failure) {
		return latency < kLimitMs ? Outcome::Network : Outcome::TimedOut;
	}
	if (response.status < 200 || response.status > 299) {
		return latency < kLimitMs ? Outcome::Status : Outcome::TimedOut;
	}
	const auto  chunks    = Deliveries(response.body.size(), response.chunk_sizes);
	std::size_t delivered = 0;
	int64_t     time      = latency;
	for (std::size_t k = 0; k < chunks.size(); k++) {
		time = latency + static_cast<int64_t>(k) * response.chunk_interval.count();
		if (time >= kLimitMs) {
			return Outcome::TimedOut;
		}
		delivered += chunks[k];
		if (delivered > limit) {
			return Outcome::TooLarge;
		}
	}
	return time < kLimitMs ? Outcome::Accepted : Outcome::TimedOut;
}

std::string Reason(Outcome outcome, const KytyTests::FakeResponse& response, uint64_t limit) {
	switch (outcome) {
		case Outcome::Accepted: return {};
		case Outcome::Network: return "Cheat download failed: " + response.failure_message + ".";
		case Outcome::Status:
			return "Unexpected response from the cheat repository (HTTP " +
			       std::to_string(response.status) + ").";
		case Outcome::TooLarge:
			return "Cheat download exceeds the size limit of " + std::to_string(limit >> 20u) +
			       " MiB.";
		case Outcome::TimedOut: break;
	}
	return "Cheat download timed out after 30 seconds.";
}

const char* OutcomeName(Outcome outcome) {
	switch (outcome) {
		case Outcome::Accepted: return "accepted";
		case Outcome::Network: return "network failure";
		case Outcome::Status: return "failure status";
		case Outcome::TooLarge: return "too large";
		case Outcome::TimedOut: break;
	}
	return "timed out";
}

// Status, failure, chunks, and timing around the body.
KytyTests::FakeResponse Script(std::string body, bool near_limit) {
	KytyTests::FakeResponse response;
	response.body            = std::move(body);
	const int status         = Choose(0, 20);
	response.status          = status < 17   ? 200
	                           : status < 18 ? Pick(std::vector<int> {201, 203})
	                                         : Pick(std::vector<int> {301, 404, 500, 503});
	response.network_failure = OneIn(12);
	response.failure_message = "host unreachable";
	const int chunks         = Choose(0, 4);
	for (int i = 0; i < chunks; i++) {
		response.chunk_sizes.push_back(near_limit ? Choose<std::size_t>(0, response.body.size())
		                                          : Choose<std::size_t>(0, 64));
	}
	const auto deliveries =
	    static_cast<int64_t>(Deliveries(response.body.size(), response.chunk_sizes).size());
	switch (Choose(0, 3)) {
		case 0: // fast
			response.latency        = milliseconds {Choose<int64_t>(0, 1000)};
			response.chunk_interval = milliseconds {Choose<int64_t>(0, 100)};
			break;
		case 1: // anywhere
			response.latency        = milliseconds {Choose<int64_t>(0, 32000)};
			response.chunk_interval = milliseconds {Choose<int64_t>(0, 16000)};
			break;
		default: { // ending right at the limit
			const int64_t end       = Pick(std::vector<int64_t> {29998, 29999, 30000, 30001});
			const int64_t interval  = deliveries > 1 ? Choose<int64_t>(0, end / deliveries) : 0;
			response.chunk_interval = milliseconds {interval};
			response.latency        = milliseconds {end - (deliveries - 1) * interval};
			break;
		}
	}
	return response;
}

std::size_t BodySize(uint64_t limit, std::size_t small, bool near_limit) {
	return near_limit ? static_cast<std::size_t>(limit) + Choose<std::size_t>(0, 5) - 2 : small;
}

} // namespace

KYTY_PROPERTY(33, "Remote cheat fetch limits and matching") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p33");
		const std::string        local_bytes = R"({"mods":[{"name":"mine","enabled":true}]})";
		const auto               local = temp.WriteFile("_Patches/PPSA01234.json", local_bytes);

		KytyTests::FakeScheduler  scheduler;
		KytyTests::FakeHttpClient http(scheduler);
		const bool                near_limit = OneIn(6);
		Outcome                   outcome    = Outcome::Accepted;

		if (OneIn(2)) {
			// One index of the file list.
			const auto  format = Pick(std::vector<std::string> {"json", "mc4", "shn"});
			std::string lines;
			const int   count = Choose(1, 4);
			for (int i = 0; i < count; i++) {
				lines += ContentGen::CheatIndexLine(
				             ContentGen::CheatFileName("PPSA01234", "01.000",
				                                       "n" + std::to_string(i), format),
				             "Title " + std::to_string(i)) +
				         "\n";
			}
			const auto size     = BodySize(kyty::core::kMaxCheatIndexBytes,
			                               lines.size() + Choose<std::size_t>(0, 40), near_limit);
			auto       response = Script(ContentGen::BodyOfSize(lines, "", size, '\n'), near_limit);
			outcome             = Expect(response, kyty::core::kMaxCheatIndexBytes);
			const auto reason   = Reason(outcome, response, kyty::core::kMaxCheatIndexBytes);
			RC_LOG() << format << " index, " << response.body.size() << " bytes, status "
			         << response.status << ", latency " << response.latency.count()
			         << " ms, interval " << response.chunk_interval.count()
			         << " ms: " << OutcomeName(outcome) << "\n";
			for (const auto& other: kyty::core::kCheatFormats) {
				http.AddResponse(kyty::core::CheatIndexUrl(other),
				                 other == format ? response : ContentGen::ResponseOf("\n"));
			}

			std::vector<kyty::core::CheatCatalogResult> results;
			kyty::core::CheatCatalog                    catalog(
			    http,
			    [&](const kyty::core::CheatCatalogResult& result) { results.push_back(result); });
			catalog.Load("PPSA01234");
			scheduler.RunUntilIdle();
			RC_ASSERT(results.size() == 1u);
			for (const auto& request: http.Requests()) {
				RC_ASSERT(request.max_bytes == kyty::core::kMaxCheatIndexBytes);
				RC_ASSERT(request.total_timeout == milliseconds {kLimitMs});
				RC_ASSERT(request.stall_timeout == milliseconds {0});
			}
			std::string upper = format;
			std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
				return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
			});
			if (outcome == Outcome::Accepted) {
				RC_ASSERT(results[0].error.empty());
				RC_ASSERT(results[0].files.size() == static_cast<std::size_t>(count));
				for (int i = 0; i < count; i++) {
					RC_ASSERT(results[0].files[i].name ==
					          ContentGen::CheatFileName("PPSA01234", "01.000",
					                                    "n" + std::to_string(i), format));
				}
			} else {
				RC_ASSERT(results[0].files.empty());
				RC_ASSERT(results[0].error == upper + ": " + reason);
			}
			RC_TAG("index");
		} else {
			// One cheat file.
			const kyty::core::CheatGame       game {"PPSA01234", "01.000", "eboot.bin"};
			const kyty::core::RemoteCheatFile file {"PPSA01234_01.000_HenCheats.json", "Game",
			                                        "01.000", "json"};
			std::string id = "PPSA01234", version = "01.000", process = "EBOOT.BIN";
			std::string mismatch;
			switch (OneIn(3) ? Choose(0, 3) : -1) {
				case 0:
					id       = "PPSA05678";
					mismatch = "The cheat file does not match this game's title ID.";
					break;
				case 1:
					version  = "01.001";
					mismatch = "The cheat file does not match this game's version (01.000).";
					break;
				case 2:
					process  = "other.elf";
					mismatch = "The cheat file does not match this game's executable (eboot.bin).";
					break;
				default: break;
			}
			const auto document = ContentGen::CheatDocumentText(
			    ContentGen::RemoteCheatSpec(id, version, process, {"Infinite HP", "Speed"}));
			const auto size =
			    BodySize(kyty::core::kMaxRemoteCheatFileBytes,
			             document.size() + 10 + Choose<std::size_t>(0, 40), near_limit);
			auto response =
			    Script(ContentGen::BodyOfSize("{\"pad\":\"", "\"," + document.substr(1), size, 'x'),
			           near_limit);
			outcome           = Expect(response, kyty::core::kMaxRemoteCheatFileBytes);
			const auto reason = Reason(outcome, response, kyty::core::kMaxRemoteCheatFileBytes);
			RC_LOG() << "cheat file, " << response.body.size() << " bytes, status "
			         << response.status << ", latency " << response.latency.count()
			         << " ms, interval " << response.chunk_interval.count() << " ms, mismatch '"
			         << mismatch << "': " << OutcomeName(outcome) << "\n";
			http.AddResponse(kyty::core::RemoteCheatFileUrl(file), std::move(response));

			std::vector<kyty::core::RemoteCheatResult> results;
			kyty::core::RemoteCheats remote(http, [&](const kyty::core::RemoteCheatResult& result) {
				results.push_back(result);
			});
			RC_ASSERT(remote.Fetch(file, game).has_value());
			scheduler.RunUntilIdle();
			RC_ASSERT(results.size() == 1u);
			RC_ASSERT(http.Requests().size() == 1u);
			RC_ASSERT(http.Requests()[0].max_bytes == kyty::core::kMaxRemoteCheatFileBytes);
			RC_ASSERT(http.Requests()[0].total_timeout == milliseconds {kLimitMs});
			RC_ASSERT(http.Requests()[0].stall_timeout == milliseconds {0});
			const auto& result = results[0];
			if (outcome == Outcome::Accepted) {
				RC_ASSERT(result.document.has_value());
				RC_ASSERT(result.importable == mismatch.empty());
				RC_ASSERT(result.error == mismatch);
			} else {
				RC_ASSERT(!result.document.has_value() && !result.importable);
				RC_ASSERT(result.error == reason);
			}
			RC_TAG(mismatch.empty() ? "cheat file" : "cheat file, mismatch");
		}
		RC_TAG(OutcomeName(outcome));
		if (near_limit) {
			RC_TAG("near the size limit");
		}

		const auto after = kyty::core::ReadFileLimited(local, kyty::core::kMaxCheatFileBytes);
		// RC_ASSERT does not short-circuit &&: the value is checked before it is read.
		RC_ASSERT(after.has_value());
		if (after.has_value()) {
			RC_ASSERT(*after == local_bytes);
		}
		RC_ASSERT(http.ActiveCount() == 0u);
	});
}
