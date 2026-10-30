#include "launcher/core/compatibility.h"

#include "launcher/core/dataPaths.h"
#include "launcher/core/jsonErrorCapture.h"
#include "launcher/core/retrySchedule.h"
#include "launcher/core/text.h"
#include "launcher/core/titleId.h"

#include <cerrno>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <span>
#include <utility>

namespace kyty::core {

namespace {

using Json = nlohmann::json;

std::string DescribeFailure(const HttpResult& result) {
	switch (result.failure) {
		case HttpFailure::None: break;
		case HttpFailure::Network: return "network error: " + result.message;
		case HttpFailure::Status:
			return "the server answered with HTTP status " + std::to_string(result.status);
		case HttpFailure::TooLarge:
			return "the response is larger than " + std::to_string(kMaxCompatibilityBytes >> 20u) +
			       " MiB";
		case HttpFailure::Stalled: return "the transfer stalled: " + result.message;
		case HttpFailure::TimedOut:
			return "no complete response within " +
			       std::to_string(kCompatibilityTimeout.count() / 1000) + " seconds";
		case HttpFailure::Cancelled: return "the request was cancelled";
	}
	return "unknown failure";
}

} // namespace

CompatibilityStatus ParseCompatibilityStatus(std::string_view text) {
	const auto value = TrimWhitespace(text);
	if (value == "InGame" || value == "In game") {
		return CompatibilityStatus::InGame;
	}
	if (value == "MainMenu" || value == "Main menu") {
		return CompatibilityStatus::MainMenu;
	}
	if (value == "Logo") {
		return CompatibilityStatus::Logo;
	}
	if (value == "DoesntBoot" || value == "Doesn't boot") {
		return CompatibilityStatus::DoesntBoot;
	}
	return CompatibilityStatus::Unknown;
}

std::string_view CompatibilityStatusKey(CompatibilityStatus status) {
	switch (status) {
		case CompatibilityStatus::InGame: return "InGame";
		case CompatibilityStatus::MainMenu: return "MainMenu";
		case CompatibilityStatus::Logo: return "Logo";
		case CompatibilityStatus::DoesntBoot: return "DoesntBoot";
		case CompatibilityStatus::Unknown: break;
	}
	return "Unknown";
}

Expected<CompatibilityDatabase, std::string> ParseCompatibility(std::string_view json) {
	// No exceptions: a parse error gives a discarded value.
	const auto root = Json::parse(json.begin(), json.end(), nullptr, false);
	if (root.is_discarded()) {
		return MakeUnexpected("the content is not valid JSON: " + DescribeJsonError<Json>(json));
	}
	if (!root.is_object()) {
		return MakeUnexpected(std::string("the top-level JSON value is not an object"));
	}

	CompatibilityDatabase database;
	// The object's members come in byte order of their keys, so a later key wins a collision.
	for (const auto& [key, member]: root.items()) {
		auto title_id = NormalizeTitleId(key);
		if (title_id.empty()) {
			continue;
		}
		CompatibilityEntry entry;
		if (member.is_object()) {
			if (const auto status = member.find("status");
			    status != member.end() && status->is_string()) {
				entry.status = ParseCompatibilityStatus(status->get_ref<const std::string&>());
			}
			if (const auto comment = member.find("comment");
			    comment != member.end() && comment->is_string()) {
				entry.comment = comment->get_ref<const std::string&>();
			}
		}
		database.entries[std::move(title_id)] = std::move(entry);
	}
	return database;
}

CompatibilityEntry Lookup(const CompatibilityDatabase& database, std::string_view title_id) {
	const auto key = NormalizeTitleId(title_id);
	if (key.empty()) {
		return {};
	}
	const auto found = database.entries.find(key);
	return found != database.entries.end() ? found->second : CompatibilityEntry {};
}

std::string SerializeCompatibility(const CompatibilityDatabase& database) {
	Json root = Json::object();
	for (const auto& [title_id, entry]: database.entries) {
		root[title_id] = Json {{"comment", entry.comment},
		                       {"status", std::string(CompatibilityStatusKey(entry.status))}};
	}
	return root.dump(4, ' ', false, Json::error_handler_t::replace) + "\n";
}

// ---- CompatibilityModel

void CompatibilityModel::Apply(CompatibilityDatabase database) {
	m_database = std::move(database);
}

bool CompatibilityModel::ApplyLoadResult(const CompatibilityLoadResult& result) {
	if (!result.database) {
		return false;
	}
	m_database = *result.database;
	return true;
}

bool CompatibilityModel::SetStatus(std::string_view title_id, CompatibilityStatus status) {
	auto key = NormalizeTitleId(title_id);
	if (key.empty()) {
		return false;
	}
	m_database.entries[std::move(key)].status = status;
	return true;
}

bool CompatibilityModel::SetComment(std::string_view title_id, std::string comment) {
	auto key = NormalizeTitleId(title_id);
	if (key.empty()) {
		return false;
	}
	m_database.entries[std::move(key)].comment = std::move(comment);
	return true;
}

// ---- CompatibilityLoader

struct CompatibilityLoader::State: std::enable_shared_from_this<CompatibilityLoader::State> {
	State(HttpClient& http_client, Scheduler& timer_scheduler, std::filesystem::path directory,
	      Callbacks loader_callbacks)
	    : http(http_client), scheduler(timer_scheduler), data_dir(std::move(directory)),
	      callbacks(std::move(loader_callbacks)) {}

	HttpClient&           http;
	Scheduler&            scheduler;
	std::filesystem::path data_dir;
	Callbacks             callbacks;
	int                   attempt  = 0; // the number of the attempt in progress or last made
	RequestId             request  = 0;
	TimerId               timer    = 0;
	bool                  started  = false;
	bool                  finished = false;
	std::string           body;

	[[nodiscard]] std::filesystem::path CachePath() const {
		return CompatibilityCachePath(data_dir);
	}

	void Warn(const std::string& message) const {
		if (callbacks.on_warning) {
			callbacks.on_warning(message);
		}
	}

	void Finish(CompatibilityLoadResult result) {
		finished        = true;
		result.attempts = attempt;
		if (callbacks.on_finished) {
			callbacks.on_finished(std::move(result));
		}
	}

	void StartAttempt() {
		timer = 0;
		attempt++;
		body.clear();
		HttpRequest request_data;
		request_data.url                = kCompatibilityUrl;
		request_data.headers            = {{"User-Agent", "Kyty-Launcher"}};
		request_data.max_bytes          = kMaxCompatibilityBytes;
		request_data.total_timeout      = kCompatibilityTimeout;
		const std::weak_ptr<State> weak = weak_from_this();
		request                         = http.Get(
		    request_data,
		    [weak](std::span<const std::byte> chunk) {
			    const auto state = weak.lock();
			    if (!state) {
				    return false;
			    }
			    state->body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
			    return true;
		    },
		    [weak](const HttpResult& result) {
			    if (const auto state = weak.lock()) {
				    state->AttemptDone(result);
			    }
		    });
	}

	void AttemptDone(const HttpResult& result) {
		request = 0;
		std::string failure;
		if (result.failure == HttpFailure::None) {
			auto database = ParseCompatibility(body);
			if (database) {
				if (auto written = WriteFileAtomically(CachePath(), body); !written) {
					Warn("Cannot replace the compatibility cache: " + written.error().message);
				}
				body.clear();
				CompatibilityLoadResult outcome;
				outcome.source   = CompatibilityLoadResult::Source::Download;
				outcome.database = std::move(*database);
				Finish(std::move(outcome));
				return;
			}
			failure = "the downloaded content does not parse: " + database.error();
		} else {
			failure = DescribeFailure(result);
		}
		body.clear();
		Warn("Compatibility database download attempt " + std::to_string(attempt) + " of " +
		     std::to_string(kMaxDownloadAttempts) + " failed: " + failure);

		if (const auto delay = RetryDelayAfter(attempt)) {
			const std::weak_ptr<State> weak = weak_from_this();
			timer                           = scheduler.After(*delay, [weak] {
				if (const auto state = weak.lock()) {
					state->StartAttempt();
				}
			});
			return;
		}
		FallBackToCache(std::move(failure));
	}

	void FallBackToCache(std::string last_failure) {
		CompatibilityLoadResult outcome;
		outcome.last_failure = std::move(last_failure);
		const auto path      = CachePath();
		auto       bytes     = ReadFileLimited(path, kMaxCompatibilityBytes);
		if (bytes) {
			auto database = ParseCompatibility(*bytes);
			if (database) {
				Warn("Every compatibility database download attempt failed (" +
				     outcome.last_failure + "); using the cached database " + path.string());
				outcome.source   = CompatibilityLoadResult::Source::Cache;
				outcome.database = std::move(*database);
				Finish(std::move(outcome));
				return;
			}
			Warn("The cached compatibility database " + path.string() +
			     " does not parse: " + database.error());
		} else if (bytes.error().error_number != ENOENT) {
			Warn("Cannot read the cached compatibility database: " + bytes.error().message);
		}
		Warn("No compatibility database is available, every compatibility status is Unknown; "
		     "the last download attempt failed: " +
		     outcome.last_failure);
		Finish(std::move(outcome));
	}
};

CompatibilityLoader::CompatibilityLoader(HttpClient& http, Scheduler& scheduler,
                                         std::filesystem::path data_dir, Callbacks callbacks)
    : m_state(std::make_shared<State>(http, scheduler, std::move(data_dir), std::move(callbacks))) {
}

CompatibilityLoader::~CompatibilityLoader() {
	// The pending request still ends with one DoneFn call (HttpClient contract); it finds the
	// state gone and does nothing.
	if (m_state->request != 0) {
		m_state->http.Cancel(m_state->request);
	}
	if (m_state->timer != 0) {
		m_state->scheduler.Cancel(m_state->timer);
	}
}

void CompatibilityLoader::Start() {
	if (m_state->started) {
		return;
	}
	m_state->started = true;
	m_state->StartAttempt();
}

bool CompatibilityLoader::Finished() const {
	return m_state->finished;
}

std::filesystem::path CompatibilityLoader::CachePath() const {
	return m_state->CachePath();
}

// ---- --local maintainer mode

Expected<CompatibilityDatabase, std::string>
LoadLocalCompatibility(const std::filesystem::path& path) {
	auto bytes = ReadFileLimited(path, kMaxCompatibilityBytes);
	if (!bytes) {
		if (bytes.error().error_number == ENOENT) {
			return CompatibilityDatabase {};
		}
		return MakeUnexpected("Cannot read the compatibility database: " + bytes.error().message);
	}
	auto database = ParseCompatibility(*bytes);
	if (!database) {
		return MakeUnexpected("The compatibility database " + path.string() +
		                      " does not parse: " + database.error());
	}
	return database;
}

Expected<void, IoError> SaveLocalCompatibility(const std::filesystem::path& path,
                                               const CompatibilityDatabase& database) {
	return WriteFileAtomically(path, SerializeCompatibility(database));
}

} // namespace kyty::core
