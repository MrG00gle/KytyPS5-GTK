// Feature: linux-gtk-modernization, Property 10: Compatibility loading, retry, and cache
//
// Validates Requirements 8.1, 8.2, 8.3, 8.4, and 8.6. Four attempt outcomes from
// compatibilityGenerators.h (network error, status outside 200-299, timeout, unparseable body,
// non-object JSON, valid body, with random latencies and chunking), an initial cache (absent,
// valid, invalid), and a model that may already show an earlier database run through a
// CompatibilityLoader on FakeHttpClient and FakeScheduler with a real temporary Data_Directory.
// The loader must make at most 4 requests and stop at the first valid body; wait exactly 750,
// 1500, and 2250 ms before attempts 2, 3, and 4; replace the cache file only with a body that
// parsed and otherwise leave its bytes (or its absence) unchanged; deliver the downloaded
// database, else a valid cache's, else none with a warning that describes the last failure; and
// the model must show those statuses, or keep the earlier ones when no database came.

#include "launcher/core/atomicFile.h"
#include "launcher/core/compatibility.h"
#include "support/compatibilityGenerators.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace CompatGen = KytyTests::CompatGen;

using std::chrono::milliseconds;

using CompatGen::AttemptKind;
using CompatGen::CacheState;
using kyty::core::CompatibilityDatabase;
using kyty::core::CompatibilityLoadResult;

KytyTests::FakeResponse ResponseFor(const CompatGen::Attempt& attempt) {
	KytyTests::FakeResponse response;
	response.latency     = milliseconds {attempt.latency_ms};
	response.body        = attempt.body;
	response.chunk_sizes = attempt.chunk_sizes;
	switch (attempt.kind) {
		case AttemptKind::Network:
			response.network_failure = true;
			response.failure_message = "host unreachable";
			break;
		case AttemptKind::Status:
			response.status = attempt.status;
			response.body   = "{}"; // a body that is never delivered
			break;
		case AttemptKind::Timeout:
			response.body = "{}"; // arrives after the 15 s limit
			break;
		case AttemptKind::Unparseable:
		case AttemptKind::NotAnObject:
		case AttemptKind::Valid: break;
	}
	return response;
}

// A part of the text that describes the failure of attempt.
std::string FailureText(const CompatGen::Attempt& attempt) {
	switch (attempt.kind) {
		case AttemptKind::Network: return "network error: host unreachable";
		case AttemptKind::Status: return "HTTP status " + std::to_string(attempt.status);
		case AttemptKind::Timeout: return "no complete response within 15 seconds";
		case AttemptKind::Unparseable: return "does not parse: the content is not valid JSON";
		case AttemptKind::NotAnObject:
			return "does not parse: the top-level JSON value is not an object";
		case AttemptKind::Valid: break;
	}
	return "";
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	return bytes ? std::optional<std::string>(*bytes) : std::nullopt;
}

CompatibilityDatabase Parse(const std::string& json) {
	auto database = kyty::core::ParseCompatibility(json);
	RC_ASSERT(database.has_value());
	return *database;
}

} // namespace

KYTY_PROPERTY(10, "Compatibility loading, retry, and cache") {
	return rc::check(name, []() {
		const auto                              attempts = CompatGen::GenerateAttempts();
		const auto                              cache    = CompatGen::GenerateCache();
		std::optional<CompatGen::DatabaseModel> earlier;
		if (CompatGen::OneIn(2)) {
			earlier = CompatGen::GenerateDatabase(6);
		}
		RC_LOG() << CompatGen::CacheName(cache.state) << ", earlier database "
		         << (earlier ? "shown" : "none") << "\n";
		for (const auto& attempt: attempts) {
			RC_LOG() << "  " << CompatGen::AttemptName(attempt.kind) << " after "
			         << attempt.latency_ms << " ms: '" << attempt.body.substr(0, 60) << "'\n";
		}

		KytyTests::FakeScheduler  scheduler;
		KytyTests::FakeHttpClient http(scheduler);
		const KytyTests::TempDir  dir("kyty_p10");
		const auto                cache_path = dir.Path() / kyty::core::kCompatibilityCacheFileName;
		if (cache.state != CacheState::Absent) {
			dir.WriteFile(cache_path.filename(), cache.bytes);
		}
		for (const auto& attempt: attempts) {
			http.AddResponse(kyty::core::kCompatibilityUrl, ResponseFor(attempt));
		}

		std::vector<std::string>             warnings;
		std::vector<CompatibilityLoadResult> results;
		kyty::core::CompatibilityLoader      loader(
		    http, scheduler, dir.Path(),
		    {[&warnings](const std::string& warning) { warnings.push_back(warning); },
		     [&results](CompatibilityLoadResult result) { results.push_back(std::move(result)); }});
		kyty::core::CompatibilityModel model;
		if (earlier) {
			model.Apply(Parse(CompatGen::ToJson(*earlier)));
		}
		const auto shown_before = model.Database();

		loader.Start();
		RC_ASSERT(results.empty());
		scheduler.RunUntilIdle();
		RC_ASSERT(results.size() == 1u);
		const auto& result = results.front();

		// At most four attempts, ending at the first valid body.
		std::optional<size_t> valid;
		for (size_t i = 0; i < attempts.size() && !valid; i++) {
			if (attempts[i].kind == AttemptKind::Valid) {
				valid = i;
			}
		}
		const size_t made = valid ? *valid + 1 : 4;
		RC_ASSERT(http.Requests().size() == made);
		RC_ASSERT(result.attempts == static_cast<int>(made));
		for (const auto& request: http.Requests()) {
			RC_ASSERT(request.url == kyty::core::kCompatibilityUrl);
			RC_ASSERT(request.total_timeout == milliseconds {15000});
		}
		// Exactly 750, 1500, 2250 ms before attempts 2, 3, 4.
		const std::vector<milliseconds> schedule = {milliseconds {750}, milliseconds {1500},
		                                            milliseconds {2250}};
		RC_ASSERT(scheduler.RequestedDelays() ==
		          std::vector<milliseconds>(
		              schedule.begin(), schedule.begin() + static_cast<std::ptrdiff_t>(made - 1)));
		RC_ASSERT(http.ActiveCount() == 0u);
		RC_ASSERT(scheduler.PendingCount() == 0u);

		// The cache holds a body that parsed, else its previous bytes.
		const auto cache_now = ReadFile(cache_path);
		if (valid) {
			RC_ASSERT(cache_now == attempts[*valid].body);
		} else if (cache.state == CacheState::Absent) {
			RC_ASSERT(!cache_now.has_value());
		} else {
			RC_ASSERT(cache_now == cache.bytes);
		}

		// The delivered database: downloaded, else the valid cache, else none with a warning.
		std::optional<CompatGen::DatabaseModel> expected;
		if (valid) {
			RC_ASSERT(result.source == CompatibilityLoadResult::Source::Download);
			RC_ASSERT(result.last_failure.empty());
			expected = attempts[*valid].model;
		} else {
			RC_ASSERT(result.last_failure.find(FailureText(attempts[3])) != std::string::npos);
			RC_ASSERT(!warnings.empty());
			if (cache.state == CacheState::Valid) {
				RC_ASSERT(result.source == CompatibilityLoadResult::Source::Cache);
				expected = cache.model;
			} else {
				RC_ASSERT(result.source == CompatibilityLoadResult::Source::None);
				RC_ASSERT(warnings.back().find("every compatibility status is Unknown") !=
				          std::string::npos);
				RC_ASSERT(warnings.back().find(result.last_failure) != std::string::npos);
			}
		}
		// One warning per failed attempt, each describing its failure.
		const size_t failed = valid ? made - 1 : made;
		for (size_t i = 0; i < failed; i++) {
			RC_ASSERT(i < warnings.size());
			RC_ASSERT(warnings[i].find(FailureText(attempts[i])) != std::string::npos);
		}
		RC_ASSERT(result.database.has_value() == expected.has_value());
		if (expected) {
			RC_ASSERT(*result.database == Parse(CompatGen::ToJson(*expected)));
		}

		// The model shows the delivered statuses, or keeps the earlier ones (8.6); with neither,
		// every status is Unknown (8.4).
		RC_ASSERT(model.ApplyLoadResult(result) == expected.has_value());
		std::set<std::string> ids;
		for (const auto* source: {&expected, &earlier}) {
			if (*source) {
				for (const auto& entry: (*source)->entries) {
					ids.insert(entry.id);
				}
			}
		}
		for (int i = 0; i < 3; i++) {
			ids.insert(CompatGen::GenerateTitleId());
		}
		for (const auto& id: ids) {
			const auto shown = model.StatusFor(CompatGen::VaryTitleId(id));
			if (expected) {
				RC_ASSERT(shown == expected->ExpectedEntry(id));
			} else if (earlier) {
				RC_ASSERT(shown == earlier->ExpectedEntry(id));
			} else {
				RC_ASSERT(shown == kyty::core::CompatibilityEntry {});
			}
		}
		if (!expected) {
			RC_ASSERT(model.Database() == shown_before);
		}

		RC_TAG(valid ? "downloaded on attempt " + std::to_string(made)
		             : std::string("all failed, ") + CompatGen::CacheName(cache.state));
	});
}
