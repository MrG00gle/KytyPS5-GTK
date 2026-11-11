#pragma once

// Downloads one HEN Cheats Collection file for the cheats dialog's Remote preview and import
// (task 16.2; Requirements 11.5, 11.17; Property 33), ported from the Qt launcher's
// CheatRepository::Fetch (src/launcher/src/cheatRepository.cpp) and the dialog's FileLoaded
// handling (src/launcher/src/patchesDialog.cpp). The download has the limits of cheatCatalog.h:
// at most kMaxRemoteCheatFileBytes, a complete response within kCheatRequestTimeout. A file is
// importable only when it downloaded, parsed (ParseCheatDocument), and matches the game
// (ValidateRemoteImport); otherwise error says why and the UI keeps Import disabled. Nothing here
// writes the local cheat file: the import is ImportRemoteCheats (cheats.h).

#include "launcher/core/cheatCatalog.h"
#include "launcher/core/cheats.h"
#include "launcher/core/expected.h"
#include "launcher/core/httpClient.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace kyty::core {

inline constexpr uint64_t kMaxRemoteCheatFileBytes         = kMaxCheatFileBytes; // 8 MiB
inline constexpr char     kUnsupportedCheatFormatMessage[] = "Only JSON cheat files are supported.";

// The game a downloaded file must match: its title ID, its game version (GameEntry::game_version),
// and its executable (CheatProcessName of the effective elf setting).
struct CheatGame {
	std::string title_id;
	std::string version;
	std::string process;
};

struct RemoteCheatResult {
	RemoteCheatFile file;
	// The downloaded file when it parsed, for the preview (also when it is not importable).
	std::optional<CheatDocument> document;
	bool                         importable = false;
	// Why the file cannot be imported: the download failure (DescribeCheatFetchFailure), the parse
	// error (it names the file), or ValidateRemoteImport's reason; "" when importable.
	std::string error;
	// The same with its parts: the download failure (fetch.failure is not None), else the parse
	// error or ValidateRemoteImport's reason (problem); neither when importable.
	CheatFetchError             fetch;
	std::optional<CheatProblem> problem;
};

// One download at a time: a new Fetch or Cancel cancels the previous request, whose result is
// never delivered; destroying the object cancels it too. on_loaded (optional) runs once per Fetch
// that started a request, from an HttpClient callback (never inside Fetch); it must not destroy the
// object.
class RemoteCheats {
public:
	using LoadedFn = std::function<void(const RemoteCheatResult&)>;

	RemoteCheats(HttpClient& http, LoadedFn on_loaded);
	~RemoteCheats();

	RemoteCheats(const RemoteCheats&)            = delete;
	RemoteCheats& operator=(const RemoteCheats&) = delete;

	// Cancels the previous download and downloads RemoteCheatFileUrl(file). A file that is not
	// IsSupportedRemoteCheatFile starts nothing and gives kUnsupportedCheatFormatMessage at once
	// (no on_loaded call).
	Expected<void, std::string> Fetch(const RemoteCheatFile& file, CheatGame game);
	void                        Cancel();

	// True from a Fetch that started a request until its result was delivered or it was cancelled.
	[[nodiscard]] bool Fetching() const;

private:
	struct State;
	std::shared_ptr<State> m_state;
};

} // namespace kyty::core
