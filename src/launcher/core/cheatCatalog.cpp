#include "launcher/core/cheatCatalog.h"

#include "launcher/core/text.h"
#include "launcher/core/titleId.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace kyty::core {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
constexpr std::size_t      kTitleIdLength = 9; // [A-Z]{4}[0-9]{5}
constexpr std::string_view kModulesHost   = "kytyps5.github.io";
constexpr std::string_view kModulesPath   = "/KytyPS5-Modules/";

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

std::string Lower(std::string_view text) {
	std::string lower(text);
	for (auto& c: lower) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return lower;
}

// ---- KytyPS5-Modules URLs (RFC 3986)

bool IsAlpha(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsHex(char c) {
	return IsDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int HexValue(char c) {
	if (IsDigit(c)) {
		return c - '0';
	}
	return c >= 'a' ? c - 'a' + 10 : c - 'A' + 10;
}

bool IsUnreserved(char c) {
	return IsAlpha(c) || IsDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// The characters a path or a query holds as they are: pchar without '%', and '/' and '?'.
bool IsUrlChar(char c) {
	static constexpr std::string_view kOther = "!$&'()*+,;=:@/?";
	return IsUnreserved(c) || kOther.find(c) != std::string_view::npos;
}

bool IsEscape(std::string_view text, std::size_t i) {
	return text[i] == '%' && i + 2 < text.size() && IsHex(text[i + 1]) && IsHex(text[i + 2]);
}

// Percent-encoded unreserved characters decoded (section 6.2.2.2).
std::string DecodeUnreserved(std::string_view text) {
	std::string decoded;
	for (std::size_t i = 0; i < text.size(); i++) {
		if (IsEscape(text, i)) {
			const auto c = static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2]));
			if (IsUnreserved(c)) {
				decoded += c;
				i += 2;
				continue;
			}
		}
		decoded += text[i];
	}
	return decoded;
}

// Every byte that a URL cannot hold as it is, percent-encoded; escapes stay as they are.
std::string EncodeUrlPart(std::string_view text) {
	static constexpr std::string_view kDigits = "0123456789ABCDEF";
	std::string                       encoded;
	for (std::size_t i = 0; i < text.size(); i++) {
		const char c = text[i];
		if (IsUrlChar(c) || IsEscape(text, i)) {
			encoded += c;
		} else {
			const auto byte = static_cast<unsigned char>(c);
			encoded += '%';
			encoded += kDigits[byte >> 4u];
			encoded += kDigits[byte & 0xFu];
		}
	}
	return encoded;
}

// Section 5.2.4.
std::string RemoveDotSegments(std::string_view input) {
	std::string output;
	const auto  drop_last_segment = [&output]() {
		const auto slash = output.rfind('/');
		output.erase(slash == std::string::npos ? 0 : slash);
	};
	while (!input.empty()) {
		if (input.starts_with("../")) {
			input.remove_prefix(3);
		} else if (input.starts_with("./") || input.starts_with("/./")) {
			input.remove_prefix(2);
		} else if (input == "/.") {
			input = "/";
		} else if (input.starts_with("/../")) {
			input.remove_prefix(3);
			drop_last_segment();
		} else if (input == "/..") {
			input = "/";
			drop_last_segment();
		} else if (input == "." || input == "..") {
			input = {};
		} else {
			const auto end = input.find('/', 1);
			output.append(input.substr(0, end));
			input = end == std::string_view::npos ? std::string_view() : input.substr(end);
		}
	}
	return output;
}

// The string member key of value; "" when value is not an object or the member is absent or not a
// string (QJsonValue::toString).
std::string StringOf(const Json& value, const char* key) {
	if (!value.is_object()) {
		return {};
	}
	const auto found = value.find(key);
	return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

} // namespace

std::string CheatIndexUrl(std::string_view format) {
	if (!IsFormat(format)) {
		return {};
	}
	return std::string(kCheatRepositoryBaseUrl) + std::string(format) + ".txt";
}

std::string CheatSourceUrl(std::string_view source) {
	if (source == kModulesCheatSource) {
		return kModulesCatalogUrl;
	}
	return CheatIndexUrl(source);
}

std::string RemoteCheatFileUrl(const RemoteCheatFile& file) {
	if (!IsFormat(file.format)) {
		return {};
	}
	if (!file.url.empty()) {
		return file.url;
	}
	const auto match = MatchName(file.name);
	if (!match || match->extension != file.format) {
		return {};
	}
	return std::string(kCheatRepositoryBaseUrl) + file.format + "/" + file.name;
}

std::string ResolveModulesUrl(std::string_view reference) {
	if (const auto hash = reference.find('#'); hash != std::string_view::npos) {
		reference = reference.substr(0, hash);
	}
	// scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), before the first ':'.
	std::optional<std::string> scheme;
	if (const auto colon = reference.find(':');
	    colon != std::string_view::npos && colon > 0 && IsAlpha(reference[0])) {
		bool valid = true;
		for (std::size_t i = 1; i < colon; i++) {
			const char c = reference[i];
			valid        = valid && (IsAlpha(c) || IsDigit(c) || c == '+' || c == '-' || c == '.');
		}
		if (valid) {
			scheme = Lower(reference.substr(0, colon));
			reference.remove_prefix(colon + 1);
		}
	}
	std::optional<std::string> authority;
	if (reference.starts_with("//")) {
		reference.remove_prefix(2);
		const auto end = reference.find_first_of("/?");
		authority      = Lower(reference.substr(0, end));
		reference      = end == std::string_view::npos ? std::string_view() : reference.substr(end);
	}
	const auto question = reference.find('?');
	auto       path     = DecodeUnreserved(reference.substr(0, question));
	const auto query =
	    question == std::string_view::npos ? std::string_view() : reference.substr(question);
	if ((scheme && (*scheme != "https" || !authority)) ||
	    (authority && *authority != kModulesHost)) {
		return {};
	}
	if (!authority) {
		if (path.empty()) {
			// The catalog's folder itself, as for an empty reference.
			return {};
		}
		if (!path.starts_with('/')) {
			path = std::string(kModulesPath) + path;
		}
	}
	path = RemoveDotSegments(path);
	if (!path.starts_with(kModulesPath) || path.size() == kModulesPath.size()) {
		return {};
	}
	return "https://" + std::string(kModulesHost) + EncodeUrlPart(path) + EncodeUrlPart(query);
}

bool IsSupportedRemoteCheatFile(const RemoteCheatFile& file) {
	return !RemoteCheatFileUrl(file).empty();
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
				RemoteCheatFile file {std::string(name),
				                      std::string(TrimWhitespace(line.substr(separator + 1))),
				                      std::string(match->version), std::string(format)};
				file.url = RemoteCheatFileUrl(file);
				index.files.push_back(std::move(file));
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

CheatIndex ParseModulesCatalog(std::string_view bytes, std::string_view title_id) {
	CheatIndex catalog;
	const auto root = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
	if (root.is_discarded() || !root.is_object() || root.empty()) {
		catalog.error   = "Invalid module catalog.";
		catalog.problem = CheatIndexProblem::Invalid;
		return catalog;
	}
	const auto  id      = NormalizeTitleId(title_id);
	std::size_t invalid = 0;
	for (const auto& [category, kind]: {std::pair {"patches", RemoteCheatKind::Patch},
	                                    std::pair {"cheats", RemoteCheatKind::Cheat}}) {
		const auto group = root.find(category);
		if (group == root.end() || !group->is_object()) {
			continue;
		}
		const auto list = group->find(id);
		if (list == group->end() || !list->is_array()) {
			continue;
		}
		for (const auto& entry: *list) {
			auto url = ResolveModulesUrl(StringOf(entry, "path"));
			if (url.empty()) {
				invalid++;
				continue;
			}
			catalog.files.push_back({StringOf(entry, "title"), StringOf(entry, "gameName"),
			                         StringOf(entry, "version"), "json", kind, std::move(url)});
		}
	}
	if (invalid != 0) {
		catalog.error   = "Skipped " + std::to_string(invalid) + " invalid module catalog entries.";
		catalog.problem = CheatIndexProblem::SkippedEntries;
		catalog.skipped = invalid;
	}
	return catalog;
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
	std::array<RequestId, kCheatSources.size()>         requests {};
	std::array<std::string, kCheatSources.size()>       errors;
	std::array<CheatIndexFailure, kCheatSources.size()> failures;
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
		const auto source    = kCheatSources[index];
		failure              = CheatIndexFailure {std::string(source), {}, {}, 0};
		std::string error;
		if (fetch.failure == HttpFailure::None) {
			auto parsed = source == kModulesCheatSource ? ParseModulesCatalog(body, title_id)
			                                            : ParseCheatIndex(body, source, title_id);
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
				    (result.error.empty() ? "" : "\n") +
				    (kCheatSources[i] == kModulesCheatSource ? std::string("Modules")
				                                             : Upper(kCheatSources[i])) +
				    ": " + errors[i];
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
	state.pending                         = kCheatSources.size();
	const std::weak_ptr<State> weak       = m_state;
	const auto                 generation = state.generation;
	for (std::size_t i = 0; i < kCheatSources.size(); i++) {
		state.requests[i] = StartCheatDownload(
		    state.http, CheatSourceUrl(kCheatSources[i]), kMaxCheatIndexBytes,
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
