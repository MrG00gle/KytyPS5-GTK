#pragma once

// Compatibility_Status data (Requirements 8.1-8.6; design.md, "Compatibility database and cache"):
// the parser and lookup, the model the game list displays, the loader that downloads the
// Compatibility_Database with retries and falls back to the cache, and the --local maintainer file.
// Ported from the Qt launcher's CompatibilityDatabase (src/launcher/src/compatibilityDatabase.cpp).
//
// Content format (the remote file, the cache, and the --local file):
//   { "<TITLE_ID>": { "status": "InGame", "comment": "..." }, ... }
// Member keys are title IDs, matched after NormalizeTitleId() (titleId.h: TrimWhitespace, then
// ASCII letters upper-cased). The status aliases are those of the Qt launcher, compared after
// TrimWhitespace and with letter case: "InGame" and "In game", "MainMenu" and "Main menu", "Logo",
// "DoesntBoot" and "Doesn't boot"; every other value, a non-string status, a missing status, and a
// member that is not an object give Unknown.

#include "launcher/core/atomicFile.h"
#include "launcher/core/expected.h"
#include "launcher/core/httpClient.h"
#include "launcher/core/scheduler.h"
#include "launcher/core/titleId.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kyty::core {

enum class CompatibilityStatus { Unknown, InGame, MainMenu, Logo, DoesntBoot };

struct CompatibilityEntry {
	CompatibilityStatus status = CompatibilityStatus::Unknown;
	std::string         comment;

	friend bool operator==(const CompatibilityEntry&, const CompatibilityEntry&) = default;
};

struct CompatibilityDatabase {
	// Keyed by NormalizeTitleId(member key); an empty key is never stored.
	std::map<std::string, CompatibilityEntry> entries;

	friend bool operator==(const CompatibilityDatabase&, const CompatibilityDatabase&) = default;
};

// Where the Compatibility_Database comes from, and the download limits. The Qt launcher had no
// size limit; 16 MiB bounds the memory a download or a cache read can take.
inline constexpr char kCompatibilityUrl[] = "https://kytyps5.github.io/data/compatibility.json";
inline constexpr char kCompatibilityCacheFileName[] = "compatibility_cache.json"; // Data_Directory
inline constexpr char kLocalCompatibilityFileName[] = "compatibility_db.json";    // --local, ./
inline constexpr uint64_t                  kMaxCompatibilityBytes = uint64_t {16} << 20u;
inline constexpr std::chrono::milliseconds kCompatibilityTimeout {15000};

// The status an alias names, after TrimWhitespace; Unknown for anything else.
CompatibilityStatus ParseCompatibilityStatus(std::string_view text);

// The key the Qt launcher wrote for a status: "InGame", "MainMenu", "Logo", "DoesntBoot", or
// "Unknown".
std::string_view CompatibilityStatusKey(CompatibilityStatus status);

// Parses Compatibility_Database content. Fails, with a message that describes the parse error,
// when the content is not valid JSON or its top-level value is not an object (Requirement 8.6).
// Members whose key is empty after trimming are skipped. When two member keys normalize to the
// same title ID, the member whose raw key comes later in byte order wins (the Qt launcher's order).
Expected<CompatibilityDatabase, std::string> ParseCompatibility(std::string_view json);

// The entry for NormalizeTitleId(title_id); a default (Unknown, no comment) entry when the
// normalized ID is empty or has no entry (Requirement 8.5).
CompatibilityEntry Lookup(const CompatibilityDatabase& database, std::string_view title_id);

// The content in the Qt launcher's format: one member per entry in key order, each with "comment"
// and "status" (CompatibilityStatusKey), indented by 4 spaces like QJsonDocument::Indented, ending
// with a line break. Ill-formed UTF-8 in a comment is written as U+FFFD.
std::string SerializeCompatibility(const CompatibilityDatabase& database);

// What a CompatibilityLoader run produced.
struct CompatibilityLoadResult {
	enum class Source {
		Download, // an attempt downloaded content that parsed; the cache now holds it
		Cache,    // every attempt failed; the cache parsed
		None      // every attempt failed and no usable cache exists
	};
	Source                               source = Source::None;
	std::optional<CompatibilityDatabase> database; // set for Download and Cache
	int                                  attempts = 0;
	std::string last_failure; // why the last attempt failed; empty after a download
};

// The Compatibility_Status values the game list displays. Entries detected later look their
// status up here too, so no rescan is needed (Requirement 8.5). Starts empty: every status is
// Unknown.
class CompatibilityModel {
public:
	// Replaces the displayed statuses with database.
	void Apply(CompatibilityDatabase database);

	// Applies the database of a load, when it produced one, and returns true; a load without a
	// database (every download and the cache failed) keeps the statuses loaded before
	// (Requirement 8.6) and returns false.
	bool ApplyLoadResult(const CompatibilityLoadResult& result);

	[[nodiscard]] CompatibilityEntry StatusFor(std::string_view title_id) const {
		return Lookup(m_database, title_id);
	}

	[[nodiscard]] const CompatibilityDatabase& Database() const { return m_database; }

	// --local maintainer edits (LFI-21): set the status or comment of NormalizeTitleId(title_id),
	// creating the entry. False, and nothing changes, when the normalized ID is empty. The caller
	// saves with SaveLocalCompatibility.
	bool SetStatus(std::string_view title_id, CompatibilityStatus status);
	bool SetComment(std::string_view title_id, std::string comment);

private:
	CompatibilityDatabase m_database;
};

// Downloads the Compatibility_Database once per launcher start (Requirements 8.1-8.4, 8.6):
// - up to kMaxDownloadAttempts GET requests of kCompatibilityUrl, each with a total timeout of
//   kCompatibilityTimeout and at most kMaxCompatibilityBytes, waiting RetryDelayAfter(n)
//   (750, 1500, 2250 ms) on the Scheduler after failed attempt n. An attempt fails on a network
//   error, a status outside 200-299, a timeout, a stall, a body over the limit, or content that
//   ParseCompatibility rejects;
// - the first attempt whose content parses ends the run: its bytes replace
//   <data_dir>/compatibility_cache.json through WriteFileAtomically (a write failure is only a
//   warning) and its database is delivered (Source::Download);
// - after the last failed attempt the cache is read (ReadFileLimited, kMaxCompatibilityBytes) and
//   its database delivered when it parses (Source::Cache); else the result has no database
//   (Source::None) and a warning names the last download failure.
// on_warning receives one message per failed attempt, per unusable cache, per cache write
// failure, and for the fallback outcomes; on_finished runs exactly once, after the last
// on_warning. Both run from HttpClient or Scheduler callbacks on the main thread, never inside
// Start(), and must not destroy the loader. Destroying the loader cancels the pending request or
// timer; no callback runs afterwards.
class CompatibilityLoader {
public:
	struct Callbacks {
		std::function<void(const std::string&)>      on_warning;
		std::function<void(CompatibilityLoadResult)> on_finished;
	};

	CompatibilityLoader(HttpClient& http, Scheduler& scheduler, std::filesystem::path data_dir,
	                    Callbacks callbacks);
	~CompatibilityLoader();

	CompatibilityLoader(const CompatibilityLoader&)            = delete;
	CompatibilityLoader& operator=(const CompatibilityLoader&) = delete;

	// Sends the first request. Only the first call does anything.
	void Start();

	// True once on_finished has run.
	[[nodiscard]] bool Finished() const;

	[[nodiscard]] std::filesystem::path CachePath() const;

private:
	struct State;
	std::shared_ptr<State> m_state;
};

// The --local maintainer mode (design.md, "Gaps against the approved requirements" 2, LFI-21): the
// launcher reads and writes ./compatibility_db.json (kLocalCompatibilityFileName) in the Qt format
// and downloads nothing. A missing file gives an empty database (as the Qt launcher); an
// unreadable file, one over kMaxCompatibilityBytes, or content that does not parse is an error
// naming the path and the reason.
Expected<CompatibilityDatabase, std::string>
LoadLocalCompatibility(const std::filesystem::path& path);

// Writes SerializeCompatibility(database) to path through WriteFileAtomically.
Expected<void, IoError> SaveLocalCompatibility(const std::filesystem::path& path,
                                               const CompatibilityDatabase& database);

} // namespace kyty::core
