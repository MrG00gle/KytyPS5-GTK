#include "launcher/core/cheatCatalog.h"

#include "launcher/core/text.h"
#include "launcher/core/titleId.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace kyty::core {

namespace {

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
constexpr std::size_t      kTitleIdLength = 9; // [A-Z]{4}[0-9]{5}

bool IsFormat(std::string_view format) {
	return std::find(kCheatFormats.begin(), kCheatFormats.end(), format) != kCheatFormats.end();
}

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool IsNameChar(char c) {
	return IsDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' ||
	       c == '-';
}

// [0-9]+(?:\.[0-9]+)+
bool IsVersion(std::string_view text) {
	std::size_t parts = 0;
	while (true) {
		const auto end  = text.find('.');
		const auto part = text.substr(0, end);
		if (part.empty() || !std::all_of(part.begin(), part.end(), IsDigit)) {
			return false;
		}
		parts++;
		if (end == std::string_view::npos) {
			return parts >= 2;
		}
		text.remove_prefix(end + 1);
	}
}

struct NameMatch {
	std::string_view title_id;
	std::string_view version;
	std::string_view extension;
};

// \A([A-Z]{4}[0-9]{5})_([0-9]+(?:\.[0-9]+)+)_[A-Za-z0-9_.-]+\.(json|mc4|shn)\z: the version
// cannot contain '_', so it ends at the first '_' after the title ID; the rest is one or more name
// characters followed by '.' and a format, and '.' is a name character, so only the last '.'
// matters.
std::optional<NameMatch> MatchName(std::string_view name) {
	if (name.size() <= kTitleIdLength || name[kTitleIdLength] != '_') {
		return std::nullopt;
	}
	for (std::size_t i = 0; i < kTitleIdLength; i++) {
		const char c = name[i];
		if (i < 4 ? (c < 'A' || c > 'Z') : !IsDigit(c)) {
			return std::nullopt;
		}
	}
	const auto rest      = name.substr(kTitleIdLength + 1);
	const auto separator = rest.find('_');
	if (separator == std::string_view::npos || !IsVersion(rest.substr(0, separator))) {
		return std::nullopt;
	}
	const auto tail = rest.substr(separator + 1);
	const auto dot  = tail.rfind('.');
	if (dot == std::string_view::npos || dot == 0 || !IsFormat(tail.substr(dot + 1)) ||
	    !std::all_of(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(dot), IsNameChar)) {
		return std::nullopt;
	}
	return NameMatch {name.substr(0, kTitleIdLength), rest.substr(0, separator),
	                  tail.substr(dot + 1)};
}

std::string Upper(std::string_view text) {
	std::string upper(text);
	for (auto& c: upper) {
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 'a' + 'A');
		}
	}
	return upper;
}

} // namespace

std::string CheatIndexUrl(std::string_view format) {
	if (!IsFormat(format)) {
		return {};
	}
	return std::string(kCheatRepositoryBaseUrl) + std::string(format) + ".txt";
}

std::string RemoteCheatFileUrl(const RemoteCheatFile& file) {
	const auto match = MatchName(file.name);
	if (!IsFormat(file.format) || !match || match->extension != file.format) {
		return {};
	}
	return std::string(kCheatRepositoryBaseUrl) + file.format + "/" + file.name;
}

bool IsSupportedRemoteCheatFile(const RemoteCheatFile& file) {
	return file.format == "json" && !RemoteCheatFileUrl(file).empty();
}

CheatIndex ParseCheatIndex(std::string_view bytes, std::string_view format,
                           std::string_view title_id) {
	CheatIndex index;
	if (!IsFormat(format) || bytes.empty() || bytes.size() > kMaxCheatIndexBytes) {
		index.error   = "Invalid cheat index.";
		index.problem = CheatIndexProblem::Invalid;
		return index;
	}
	const auto       id   = NormalizeTitleId(title_id);
	const auto       text = SanitizeUtf8(bytes);
	std::string_view rest = text;
	if (rest.starts_with(kByteOrderMark)) {
		rest.remove_prefix(kByteOrderMark.size());
	}
	std::set<std::string, std::less<>> seen;
	std::size_t                        invalid = 0;
	while (true) {
		const auto end  = rest.find('\n');
		const auto line = TrimWhitespace(rest.substr(0, end));
		if (!line.empty()) {
			const auto separator = line.find('=');
			const auto name      = line.substr(0, separator);
			const auto match     = MatchName(name);
			if (separator == std::string_view::npos || !match || match->extension != format) {
				invalid++;
			} else if (match->title_id == id && !seen.contains(name)) {
				seen.emplace(name);
				index.files.push_back({std::string(name),
				                       std::string(TrimWhitespace(line.substr(separator + 1))),
				                       std::string(match->version), std::string(format)});
			}
		}
		if (end == std::string_view::npos) {
			break;
		}
		rest.remove_prefix(end + 1);
	}
	if (invalid != 0) {
		index.error   = "Skipped " + std::to_string(invalid) + " invalid cheat index entries.";
		index.problem = CheatIndexProblem::SkippedEntries;
		index.skipped = invalid;
	}
	return index;
}

HttpRequest CheatRequest(std::string url, uint64_t max_bytes) {
	HttpRequest request;
	request.url           = std::move(url);
	request.headers       = {{"User-Agent", kCheatUserAgent}};
	request.max_bytes     = max_bytes;
	request.total_timeout = kCheatRequestTimeout;
	request.redirects     = RedirectPolicy::SameOrigin;
	return request;
}

CheatFetchError CheatFetchErrorFor(const HttpResult& result, uint64_t max_bytes) {
	CheatFetchError error;
	error.failure = result.failure;
	switch (result.failure) {
		case HttpFailure::None: return error;
		case HttpFailure::Network:
			error.detail  = result.message;
			error.message = "Cheat download failed: " + result.message + ".";
			return error;
		case HttpFailure::Status:
			error.status  = result.status;
			error.message = "Unexpected response from the cheat repository (HTTP " +
			                std::to_string(result.status) + ").";
			return error;
		case HttpFailure::TooLarge:
			error.limit_mib = max_bytes >> 20u;
			error.message   = "Cheat download exceeds the size limit of " +
			                  std::to_string(error.limit_mib) + " MiB.";
			return error;
		case HttpFailure::Stalled:
			error.detail  = result.message;
			error.message = "Cheat download stalled: " + result.message + ".";
			return error;
		case HttpFailure::TimedOut:
			error.seconds = kCheatRequestTimeout.count() / 1000;
			error.message =
			    "Cheat download timed out after " + std::to_string(error.seconds) + " seconds.";
			return error;
		case HttpFailure::Cancelled: break;
	}
	error.failure = HttpFailure::Cancelled;
	error.message = "Cheat download was cancelled.";
	return error;
}

std::string DescribeCheatFetchFailure(const HttpResult& result, uint64_t max_bytes) {
	return CheatFetchErrorFor(result, max_bytes).message;
}

RequestId StartCheatDownload(HttpClient& http, std::string url, uint64_t max_bytes,
                             std::function<void(std::string body, CheatFetchError error)> done) {
	struct Download {
		std::string body;
		bool        too_large = false;
	};
	auto download = std::make_shared<Download>();
	return http.Get(
	    CheatRequest(std::move(url), max_bytes),
	    [download, max_bytes](std::span<const std::byte> chunk) {
		    if (max_bytes > 0 && download->body.size() + chunk.size() > max_bytes) {
			    download->too_large = true;
			    return false;
		    }
		    download->body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
		    return true;
	    },
	    [download, max_bytes, done = std::move(done)](const HttpResult& result) {
		    if (download->too_large) {
			    HttpResult too_large {HttpFailure::TooLarge, result.status, {}};
			    done({}, CheatFetchErrorFor(too_large, max_bytes));
		    } else if (result.failure != HttpFailure::None) {
			    done({}, CheatFetchErrorFor(result, max_bytes));
		    } else {
			    done(std::move(download->body), {});
		    }
	    });
}

void PutInstalledVersionFirst(std::vector<RemoteCheatFile>& files,
                              std::string_view              installed_version) {
	std::stable_partition(files.begin(), files.end(), [installed_version](const auto& file) {
		return file.version == installed_version;
	});
}

// ---- CheatCatalog

struct CheatCatalog::State {
	State(HttpClient& client, LoadedFn loaded): http(client), on_loaded(std::move(loaded)) {}

	HttpClient&                                   http;
	LoadedFn                                      on_loaded;
	uint64_t                                      generation = 0;
	std::array<RequestId, kCheatFormats.size()>         requests {};
	std::array<std::string, kCheatFormats.size()>       errors;
	std::array<CheatIndexFailure, kCheatFormats.size()> failures;
	std::vector<RemoteCheatFile>                        files;
	std::string                                         title_id;
	std::size_t                                         pending = 0;

	void CancelRequests() {
		for (auto& request: requests) {
			if (request != 0) {
				http.Cancel(request);
			}
			request = 0;
		}
		pending = 0;
		generation++;
	}

	void IndexDone(std::size_t index, const std::string& body, CheatFetchError fetch) {
		requests[index] = 0;
		auto& failure   = failures[index];
		failure         = CheatIndexFailure {std::string(kCheatFormats[index]), {}, {}, 0};
		std::string error;
		if (fetch.failure == HttpFailure::None) {
			auto parsed = ParseCheatIndex(body, kCheatFormats[index], title_id);
			files.insert(files.end(), std::make_move_iterator(parsed.files.begin()),
			             std::make_move_iterator(parsed.files.end()));
			error           = std::move(parsed.error);
			failure.problem = parsed.problem;
			failure.skipped = parsed.skipped;
		} else {
			error         = fetch.message;
			failure.fetch = std::move(fetch);
		}
		errors[index] = std::move(error);
		if (--pending > 0) {
			return;
		}

		CheatCatalogResult result;
		result.files = std::move(files);
		files.clear();
		std::sort(result.files.begin(), result.files.end(), [](const auto& a, const auto& b) {
			if (a.version != b.version) {
				return a.version > b.version;
			}
			if (a.format != b.format) {
				return a.format < b.format;
			}
			return a.name < b.name;
		});
		for (std::size_t i = 0; i < errors.size(); i++) {
			if (!errors[i].empty()) {
				result.error +=
				    (result.error.empty() ? "" : "\n") + Upper(kCheatFormats[i]) + ": " + errors[i];
				result.failures.push_back(failures[i]);
			}
		}
		if (on_loaded) {
			on_loaded(result);
		}
	}
};

CheatCatalog::CheatCatalog(HttpClient& http, LoadedFn on_loaded)
    : m_state(std::make_shared<State>(http, std::move(on_loaded))) {}

CheatCatalog::~CheatCatalog() {
	// Each cancelled request still ends with one DoneFn call (HttpClient contract); it finds the
	// state gone and does nothing.
	m_state->CancelRequests();
}

void CheatCatalog::Load(std::string_view title_id) {
	auto& state = *m_state;
	state.CancelRequests();
	state.files.clear();
	state.errors                          = {};
	state.failures                        = {};
	state.title_id                        = std::string(title_id);
	state.pending                         = kCheatFormats.size();
	const std::weak_ptr<State> weak       = m_state;
	const auto                 generation = state.generation;
	for (std::size_t i = 0; i < kCheatFormats.size(); i++) {
		state.requests[i] = StartCheatDownload(
		    state.http, CheatIndexUrl(kCheatFormats[i]), kMaxCheatIndexBytes,
		    [weak, generation, i](const std::string& body, CheatFetchError error) {
			    const auto locked = weak.lock();
			    if (locked && locked->generation == generation) {
				    locked->IndexDone(i, body, std::move(error));
			    }
		    });
	}
}

void CheatCatalog::Cancel() {
	m_state->CancelRequests();
}

bool CheatCatalog::Loading() const {
	return m_state->pending > 0;
}

} // namespace kyty::core
