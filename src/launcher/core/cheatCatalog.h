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
//
// Before the collection's indexes, the list also has the patches and cheats that KytyPS5-Modules
// publishes (upstream KytyPS5 commit d2ec91b8, CheatRepository::Load and ParseModules), listed in
// its catalog kModulesCatalogUrl:
//   {"patches": {"<TITLE_ID>": [{"path": "patches/json/<file>.json", "title": "...",
//                                "gameName": "...", "version": "01.000"}, ...]},
//    "cheats": {"<TITLE_ID>": [...]}}
// Each entry's file is cheat JSON at its path, relative to kModulesBaseUrl.

#include "launcher/core/httpClient.h"

#include <array>
#include <chrono>
#include <cstddef>
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

// KytyPS5-Modules (upstream's kModulesUrl): its files are below kModulesBaseUrl, and its catalog
// is index.min.json there. kModulesCheatSource names the catalog in CheatIndexFailure::source.
inline constexpr char kModulesBaseUrl[] = "https://kytyps5.github.io/KytyPS5-Modules/";
inline constexpr char kModulesCatalogUrl[] =
    "https://kytyps5.github.io/KytyPS5-Modules/index.min.json";
inline constexpr std::string_view kModulesCheatSource = "modules";

// The lists CheatCatalog loads, in the order they are requested: the KytyPS5-Modules catalog, then
// the collection's index of each format (upstream's order).
inline constexpr std::array<std::string_view, 4> kCheatSources = {kModulesCheatSource, "json",
                                                                  "mc4", "shn"};

// What a remote file holds (upstream's RemoteFile::kind): every collection file is a Cheat; a
// KytyPS5-Modules file is a Patch or a Cheat, as its catalog lists it.
enum class RemoteCheatKind { Cheat, Patch };

// One file of the remote list: from an index line of the collection or an entry of the
// KytyPS5-Modules catalog.
struct RemoteCheatFile {
	std::string name;    // "<TITLE_ID>_<version>_<rest>.<format>"; a Modules entry's "title"
	std::string title;   // the text after '=', trimmed; a Modules entry's "gameName"
	std::string version; // the <version> part of the name ("01.000"); a Modules entry's "version"
	std::string format;  // "json", "mc4", or "shn" (Modules files are JSON)
	RemoteCheatKind kind = RemoteCheatKind::Cheat;
	// Where the file is (ParseCheatIndex, ParseModulesCatalog); "" lets RemoteCheatFileUrl build
	// the collection's URL from name and format.
	std::string url {};

	friend bool operator==(const RemoteCheatFile&, const RemoteCheatFile&) = default;
};

// <kCheatRepositoryBaseUrl><format>.txt; "" for a format outside kCheatFormats.
std::string CheatIndexUrl(std::string_view format);

// kModulesCatalogUrl for kModulesCheatSource, else CheatIndexUrl(source).
std::string CheatSourceUrl(std::string_view source);

// "" unless format is one of kCheatFormats; then file.url when it is set, else
// <kCheatRepositoryBaseUrl><format>/<name> when name matches the collection's pattern
// ^([A-Z]{4}[0-9]{5})_([0-9]+(\.[0-9]+)+)_[A-Za-z0-9_.-]+\.(ext)$ with ext equal to format (Qt's
// RemoteFile::Url).
std::string RemoteCheatFileUrl(const RemoteCheatFile& file);

// The URL of a KytyPS5-Modules file (upstream's kModulesUrl.resolved(path), accepted when
// kModulesUrl.isParentOf it): path, a URL reference (RFC 3986), resolved against kModulesBaseUrl
// with its dot segments removed, when the result is an https URL of kytyps5.github.io (no user
// information, no port) whose path is below /KytyPS5-Modules/; else "". Percent-encoded unreserved
// characters are decoded first (so "%2E%2E" is a dot segment, as QUrl reads it), characters a URL
// cannot hold are percent-encoded, and a fragment is dropped.
std::string ResolveModulesUrl(std::string_view path);

// A file can be previewed and imported when it has a URL: JSON files as they are, MC4 and SHN files
// converted to cheat JSON (cheatTrainer.h; upstream KytyPS5 commit 91c5820c dropped Qt's
// RemoteFile::IsSupported).
bool IsSupportedRemoteCheatFile(const RemoteCheatFile& file);

// What is wrong with an index or the KytyPS5-Modules catalog, for the GTK layer's own words (plan
// V1).
enum class CheatIndexProblem {
	None,
	Invalid,        // "Invalid cheat index." / "Invalid module catalog."
	SkippedEntries, // "Skipped <skipped> invalid cheat index entries." / "... module catalog ..."
};

struct CheatIndex {
	std::vector<RemoteCheatFile> files;
	std::string                  error; // "" when every line or entry was usable
	CheatIndexProblem            problem = CheatIndexProblem::None;
	std::size_t                  skipped = 0; // SkippedEntries: the invalid lines or entries
};

// The files of title_id in the index of format, in index order (Qt's ParseIndex), each a Cheat
// with its RemoteCheatFileUrl. The bytes are read as UTF-8 (ill-formed sequences become U+FFFD; a
// leading byte order mark is dropped) and split at LF; each line is trimmed (TrimWhitespace) and
// empty lines are skipped. A line without '=' or whose name (the text before the first '=',
// untrimmed) does not match the pattern of RemoteCheatFileUrl with the index's format counts as
// invalid; lines for another title ID (the name's first part against NormalizeTitleId(title_id))
// and repeated names are skipped. error: "Invalid cheat index." (no files) for an unknown format,
// empty bytes, or more than kMaxCheatIndexBytes; else "Skipped <n> invalid cheat index entries."
// when there were invalid lines.
CheatIndex ParseCheatIndex(std::string_view bytes, std::string_view format,
                           std::string_view title_id);

// The files of title_id in a KytyPS5-Modules catalog (upstream's ParseModules): the entries of
// "patches" (kind Patch), then of "cheats" (kind Cheat), under the key NormalizeTitleId(title_id),
// in catalog order. Each file is JSON, its name the entry's "title", its title the entry's
// "gameName", its version the entry's "version" (a member that is not a string reads as ""), and
// its url ResolveModulesUrl of the entry's "path"; an entry whose path gives no URL (also one that
// is not an object) is invalid. A group or a title ID's list that is not an object or an array
// lists nothing. error: "Invalid module catalog." (no files) when the bytes are not a JSON object
// with at least one member; else "Skipped <n> invalid module catalog entries." when there were
// invalid entries.
CheatIndex ParseModulesCatalog(std::string_view bytes, std::string_view title_id);

// The request of one collection download: GET url, User-Agent kCheatUserAgent, max_bytes,
// total_timeout kCheatRequestTimeout, no stall timeout, and RedirectPolicy::SameOrigin (the Qt
// launcher's SameOriginRedirectPolicy).
HttpRequest CheatRequest(std::string url, uint64_t max_bytes);

// Why a collection download failed (plan V1: the GTK layer has its own text per failure).
struct CheatFetchError {
	HttpFailure failure   = HttpFailure::None; // None: the download succeeded
	int         status    = 0;                 // Status: the HTTP status
	uint64_t    limit_mib = 0;                 // TooLarge: max_bytes in MiB
	int64_t     seconds   = 0;                 // TimedOut: kCheatRequestTimeout in seconds
	std::string detail;                        // Network, Stalled: the client's message
	std::string message;                       // DescribeCheatFetchFailure

	friend bool operator==(const CheatFetchError&, const CheatFetchError&) = default;
};

// Why a collection download failed, for a result other than HttpFailure::None ("" for None):
// "Cheat download failed: <message>.", "Unexpected response from the cheat repository (HTTP
// <status>).", "Cheat download exceeds the size limit of <n> MiB.", "Cheat download timed out
// after 30 seconds.", "Cheat download stalled: <message>.", or "Cheat download was cancelled.".
std::string DescribeCheatFetchFailure(const HttpResult& result, uint64_t max_bytes);

// The same failure with its parts; message is DescribeCheatFetchFailure.
CheatFetchError CheatFetchErrorFor(const HttpResult& result, uint64_t max_bytes);

// Starts one collection download through http (CheatRequest(url, max_bytes)). done runs once,
// from an HttpClient callback, with the body and a CheatFetchError of HttpFailure::None when the
// request succeeded (a 2xx response within the limits), else with "" and CheatFetchErrorFor. The
// body is checked against max_bytes as it arrives too, so a client that delivers more ends the
// request (TooLarge). Cancelling the returned request with http.Cancel still runs done (with the
// Cancelled failure), so callers that cancel must ignore that call.
RequestId StartCheatDownload(HttpClient& http, std::string url, uint64_t max_bytes,
                             std::function<void(std::string body, CheatFetchError error)> done);

// A list that failed or had invalid lines or entries: the download failure (fetch.failure is not
// None), else the list's problem.
struct CheatIndexFailure {
	std::string       source; // an element of kCheatSources: "modules", "json", "mc4", or "shn"
	CheatFetchError   fetch;
	CheatIndexProblem problem = CheatIndexProblem::None;
	std::size_t       skipped = 0;

	friend bool operator==(const CheatIndexFailure&, const CheatIndexFailure&) = default;
};

struct CheatCatalogResult {
	// The title's files of every list, by version (descending, byte order), then format, then
	// name, as Qt sorted them.
	std::vector<RemoteCheatFile> files;
	// One line "<SOURCE>: <reason>" per list that failed or had invalid lines or entries, in
	// kCheatSources order, joined by LF; SOURCE is "Modules" for the KytyPS5-Modules catalog and
	// the upper-case format for an index. "" when every list loaded cleanly.
	std::string error;
	// The same lists with their parts, in the same order (empty exactly when error is "").
	std::vector<CheatIndexFailure> failures;
};

// Moves the files whose version equals installed_version to the front, keeping the order inside
// both groups (the Qt dialog listed the installed version's files first).
void PutInstalledVersionFirst(std::vector<RemoteCheatFile>& files,
                              std::string_view              installed_version);

// Loads the four lists of kCheatSources for one title ID (Qt's CheatRepository::Load). on_loaded
// (optional) runs once per Load, after the last of the four requests ended, from an HttpClient
// callback (never inside Load); it must not destroy the catalog. A new Load or Cancel cancels the
// requests of the previous Load, whose result is never delivered; destroying the catalog cancels
// them too.
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
