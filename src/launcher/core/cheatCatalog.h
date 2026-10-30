#pragma once

// The HEN Cheats Collection file list for the cheats dialog's Remote view (task 16.2; Requirements
// 11.4, 11.17; plan D23), ported from the Qt launcher's CheatRepository::Load and ParseIndex
// (src/launcher/src/cheatRepository.cpp). The collection publishes one index per format,
// <kCheatRepositoryBaseUrl><format>.txt, with lines "<file name>=<title>"; the files themselves
// are at <kCheatRepositoryBaseUrl><format>/<file name> (remoteCheats.h downloads them).
//
// Every download goes through HttpClient (httpClient.h) with the Requirement 11.17 limits: at most
// kMaxCheatIndexBytes for an index (kMaxRemoteCheatFileBytes for a file), a complete response
// within kCheatRequestTimeout, and no stall limit. The Qt launcher used a 15 s transfer
// (inactivity) timeout besides its 30 s limit; the requirement's 30 s completion limit is the only
// one now.

#include "launcher/core/httpClient.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

inline constexpr char kCheatRepositoryBaseUrl[] =
    "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/";
inline constexpr uint64_t                  kMaxCheatIndexBytes = uint64_t {2} << 20u;
inline constexpr std::chrono::milliseconds kCheatRequestTimeout {30000};
inline constexpr char                      kCheatUserAgent[] = "Kyty-Launcher"; // Qt's

// The formats of the collection, in the order their indexes are requested.
inline constexpr std::array<std::string_view, 3> kCheatFormats = {"json", "mc4", "shn"};

// One file of the collection, from an index line.
struct RemoteCheatFile {
	std::string name;    // "<TITLE_ID>_<version>_<rest>.<format>"
	std::string title;   // the text after '=', trimmed
	std::string version; // the <version> part of the name ("01.000")
	std::string format;  // "json", "mc4", or "shn"

	friend bool operator==(const RemoteCheatFile&, const RemoteCheatFile&) = default;
};

// <kCheatRepositoryBaseUrl><format>.txt; "" for a format outside kCheatFormats.
std::string CheatIndexUrl(std::string_view format);

// <kCheatRepositoryBaseUrl><format>/<name>; "" unless format is one of kCheatFormats and name
// matches the collection's pattern ^([A-Z]{4}[0-9]{5})_([0-9]+(\.[0-9]+)+)_[A-Za-z0-9_.-]+\.(ext)$
// with ext equal to format (Qt's RemoteFile::Url).
std::string RemoteCheatFileUrl(const RemoteCheatFile& file);

// Only JSON files can be previewed and imported (Qt's RemoteFile::IsSupported); the Remote view
// lists the others disabled.
bool IsSupportedRemoteCheatFile(const RemoteCheatFile& file);

struct CheatIndex {
	std::vector<RemoteCheatFile> files;
	std::string                  error; // "" when every line was usable
};

// The files of title_id in the index of format, in index order (Qt's ParseIndex). The bytes are
// read as UTF-8 (ill-formed sequences become U+FFFD; a leading byte order mark is dropped) and
// split at LF; each line is trimmed (TrimWhitespace) and empty lines are skipped. A line without
// '=' or whose name (the text before the first '=', untrimmed) does not match the pattern of
// RemoteCheatFileUrl with the index's format counts as invalid; lines for another title ID (the
// name's first part against NormalizeTitleId(title_id)) and repeated names are skipped. error:
// "Invalid cheat index." (no files) for an unknown format, empty bytes, or more than
// kMaxCheatIndexBytes; else "Skipped <n> invalid cheat index entries." when there were invalid
// lines.
CheatIndex ParseCheatIndex(std::string_view bytes, std::string_view format,
                           std::string_view title_id);

// The request of one collection download: GET url, User-Agent kCheatUserAgent, max_bytes,
// total_timeout kCheatRequestTimeout, no stall timeout, and RedirectPolicy::SameOrigin (the Qt
// launcher's SameOriginRedirectPolicy).
HttpRequest CheatRequest(std::string url, uint64_t max_bytes);

// Why a collection download failed, for a result other than HttpFailure::None ("" for None):
// "Cheat download failed: <message>.", "Unexpected response from the cheat repository (HTTP
// <status>).", "Cheat download exceeds the size limit of <n> MiB.", "Cheat download timed out
// after 30 seconds.", "Cheat download stalled: <message>.", or "Cheat download was cancelled.".
std::string DescribeCheatFetchFailure(const HttpResult& result, uint64_t max_bytes);

// Starts one collection download through http (CheatRequest(url, max_bytes)). done runs once,
// from an HttpClient callback, with the body and "" when the request succeeded (a 2xx response
// within the limits), else with "" and DescribeCheatFetchFailure. The body is checked against
// max_bytes as it arrives too, so a client that delivers more ends the request (TooLarge).
// Cancelling the returned request with http.Cancel still runs done (with the Cancelled text), so
// callers that cancel must ignore that call.
RequestId StartCheatDownload(HttpClient& http, std::string url, uint64_t max_bytes,
                             std::function<void(std::string body, std::string error)> done);

struct CheatCatalogResult {
	// The title's files of every index, by version (descending, byte order), then format, then
	// name, as Qt sorted them.
	std::vector<RemoteCheatFile> files;
	// One line "<FORMAT>: <reason>" per index that failed or had invalid lines, in kCheatFormats
	// order, joined by LF; "" when every index loaded cleanly.
	std::string error;
};

// Moves the files whose version equals installed_version to the front, keeping the order inside
// both groups (the Qt dialog listed the installed version's files first).
void PutInstalledVersionFirst(std::vector<RemoteCheatFile>& files,
                              std::string_view              installed_version);

// Loads the three indexes for one title ID (Qt's CheatRepository::Load). on_loaded (optional) runs
// once per Load, after the last of the three requests ended, from an HttpClient callback (never
// inside Load); it must not destroy the catalog. A new Load or Cancel cancels the requests of the
// previous Load, whose result is never delivered; destroying the catalog cancels them too.
class CheatCatalog {
public:
	using LoadedFn = std::function<void(const CheatCatalogResult&)>;

	CheatCatalog(HttpClient& http, LoadedFn on_loaded);
	~CheatCatalog();

	CheatCatalog(const CheatCatalog&)            = delete;
	CheatCatalog& operator=(const CheatCatalog&) = delete;

	void Load(std::string_view title_id);
	void Cancel();

	// True from Load until its result was delivered or it was cancelled.
	[[nodiscard]] bool Loading() const;

private:
	struct State;
	std::shared_ptr<State> m_state;
};

} // namespace kyty::core
