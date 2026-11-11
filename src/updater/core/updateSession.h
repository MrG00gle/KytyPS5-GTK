#pragma once

// The update session of official builds (task 17.2; design.md "Updater flow" and "Updater
// interfaces"; Requirements 10.4, 13.1, 13.5, 13.8, 13.12, 13.13), ported from the Qt launcher's
// UpdateChecker (src/launcher/src/updateChecker.cpp). Check() compares the fork's latest release
// with the installed one and offers a newer release; Install() downloads and verifies its package
// into a workspace inside the installation directory, extracts it with /usr/bin/tar, has the
// installed kyty_updater validate it, and starts kyty_updater --install detached, after which the
// launcher quits (Handoff) and the helper installs the release once the launcher has exited.
//
// Every request URL comes from updatePolicy.h and passes IsAllowedUpdateUrl before it is
// requested, so the session talks only to the fork's GitHub releases (Requirement 13.1).

#include "launcher/core/httpClient.h"
#include "launcher/core/scheduler.h"
#include "updater/core/processRunner.h"
#include "updater/core/releaseMetadata.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kyty::updater {

// Whether an update may be installed now (Requirement 10.4). kyty_updater_core links no
// Launcher_Core, so the GTK layer adapts kyty::core::RunState to this interface one to one
// (CanInstallUpdate, InstallStarted, InstallFinished).
class InstallGate {
public:
	virtual ~InstallGate() = default;

	virtual bool CanInstallUpdate() const = 0;
	virtual void InstallStarted()         = 0;
	virtual void InstallFinished()        = 0;
};

// install_dir, helper, and working_dir must be absolute paths: tar and the helper run with the
// workspace as their working directory, so a relative path would name something else there.
// Check() and Install() refuse a configuration with a relative or empty one (Failed with
// "The update configuration is invalid: <field> must be an absolute path (<value>).") before any
// request, gate call, or workspace step.
struct UpdateSessionConfig {
	std::string           installed_tag;     // the release tag of this build (KYTY_RELEASE_TAG)
	std::filesystem::path install_dir;       // the installation directory, updated in place
	std::filesystem::path launcher_relative; // the launcher executable, relative to install_dir
	std::filesystem::path helper;            // the installed kyty_updater, copied before it runs
	int64_t               launcher_pid = 0;  // this process: the helper waits for it to exit
	std::filesystem::path working_dir;       // where the helper starts the launcher again
};

// The design's states plus NoNewer. Idle, Offering, NoNewer, Failed, and Handoff are resting
// states; Check() starts from Idle, Offering, NoNewer, or Failed, and Install() from Offering.
enum class SessionState {
	Idle,
	FetchingLatest,
	FetchingInstalled,
	Offering,
	NoNewer,
	Downloading,
	Extracting,
	Validating,
	Handoff,
	Failed,
};

// Which sentence SessionOutcome::message is (plan V1: the GTK layer shows its own text per kind
// and inserts only the parts in SessionDetail). The sentences are those of this file,
// downloadVerifier.h, and releaseMetadata.h.
enum class SessionMessage {
	None,                 // no message, or one without a kind (see SessionOutcome::detail)
	NoNewerRelease,       // kNoNewerReleaseMessage
	UnknownReleaseOrder,  // kUnknownReleaseOrderMessage
	CloseGame,            // kCloseGameMessage
	GameRunningCancelled, // kGameRunningCancelledMessage
	// "The update configuration is invalid: <text> must be an absolute path (<path>)."
	InvalidConfiguration,
	RefusedUrl,        // "Refused to request <text>: it is not a release URL of <kRepository>."
	InvalidRelease,    // kInvalidReleaseMessage
	UnstableRelease,   // kUnstableReleaseMessage
	InvalidAsset,      // kInvalidAssetMessage
	MissingAsset,      // kMissingAssetMessage
	OtherRelease,      // "GitHub answered with the release <text>."
	CheckTooLarge,     // "The update response is larger than 2 MiB."
	CheckNetwork,      // "The update check failed: <text>" (text: the client's message)
	CheckStatus,       // "The update check failed with HTTP status <number>."
	CheckStalled,      // "The update check received no data for <number> seconds."
	CheckTimedOut,     // "The update check timed out."
	CheckCancelled,    // "The update check was cancelled."
	CreateWorkspace,   // "Cannot create an update folder in <path>: <errno text>"
	CopyHelper,        // "Cannot copy <path> to <other>: <errno text>"
	CreatePayload,     // "Cannot create <path>: <errno text>"
	DownloadNetwork,   // "The download failed: <text>" (text: the client's message)
	DownloadStatus,    // "The download failed with HTTP status <number>."
	DownloadStalled,   // "The download received no data for <number> seconds."
	DownloadTimedOut,  // "The download timed out."
	DownloadCancelled, // kDownloadAbortedMessage
	DownloadTooLarge,  // kDownloadTooLargeMessage
	// "The downloaded file is smaller than the release asset (<received> of <size> bytes)."
	DownloadTooSmall,
	DownloadDigest,   // kDownloadDigestMessage
	DownloadCreate,   // "Cannot create <path>: <errno text>" (the package file)
	DownloadWrite,    // "Cannot write <path>: <errno text>"
	DownloadFinished, // "The download is already finished."
	// "Could not prepare the update: <text> exited with code <number>", then "." or ":\n<stderr>";
	// text is the step ("/usr/bin/tar", "kyty_updater --validate").
	StepExited,
	StepSignaled,        // the same with "was ended by signal <number>"
	StepWaitStatus,      // the same with "ended with wait status <number>"
	StepNotStarted,      // "Could not prepare the update: <process>", then "." or ":\n<stderr>"
	InstallerNotStarted, // "Could not start the update installer: <process>"
};

// The parts of SessionOutcome::message.
struct SessionDetail {
	SessionMessage kind = SessionMessage::None;
	// The message is kInstalledReleaseUnverifiedMessage, a space, and the sentence of kind.
	bool                  installed_unverified = false;
	std::string           text; // see the kinds
	std::filesystem::path path;
	std::filesystem::path other;
	int64_t               number       = 0; // see the kinds
	uint64_t              received     = 0; // DownloadTooSmall
	uint64_t              size         = 0; // DownloadTooSmall
	int                   error_number = 0;
	std::string           standard_error; // Step*: the process's standard error ("" for ".")
	ProcessError          process;        // StepNotStarted, InstallerNotStarted

	friend bool operator==(const SessionDetail&, const SessionDetail&) = default;
};

struct SessionOutcome {
	SessionState state = SessionState::Idle;
	// Failed: the reason (empty after Cancel()); NoNewer: kNoNewerReleaseMessage; Offering: empty,
	// or kCloseGameMessage when Install() was refused; otherwise empty.
	std::string message;
	// message's kind and parts: kind None when message is empty, or for a release error this
	// file does not classify (none of releaseMetadata.h's; the UI then shows message as it is).
	SessionDetail detail;
	// The offered release, from Offering on (also in Failed after an offer).
	std::optional<ReleaseInfo> release;
	// The argument of Check(): the UI reports NoNewer and Failed only for user-activated checks.
	bool user_activated = false;
};

// Release metadata requests: at most 2 MiB, no data for 15 s ends them (Requirement 13.12).
inline constexpr uint64_t                  kMaxReleaseMetadataBytes = uint64_t {2} << 20u;
inline constexpr std::chrono::milliseconds kMetadataStallTimeout {15000};
// The package download: at most the published size, no data for 30 s ends it (Requirement 13.8).
inline constexpr std::chrono::milliseconds kAssetStallTimeout {30000};
inline constexpr std::string_view          kUpdateUserAgent = "KytyPS5-GTK-Updater";

// The workspace <install_dir>/.kyty-update-XXXXXX holds the helper copy, the package, the
// extracted payload, and the helper's log.
inline constexpr std::string_view kWorkspacePrefix   = ".kyty-update-";
inline constexpr std::string_view kHelperFileName    = "kyty_updater";
inline constexpr std::string_view kPackageFileName   = "package-Linux-x86_64.tar.gz";
inline constexpr std::string_view kPayloadFileName   = "payload";
inline constexpr std::string_view kHelperLogFileName = "helper.log";
inline constexpr std::string_view kTarProgram        = "/usr/bin/tar";

inline constexpr std::string_view kNoNewerReleaseMessage = "No newer release is available.";
inline constexpr std::string_view kUnknownReleaseOrderMessage =
    "Could not determine whether the available release is newer.";
inline constexpr std::string_view kInstalledReleaseUnverifiedMessage =
    "Could not verify the currently installed release.";
// The Requirement 10.4 answer while a game runs (kyty::core::RunState::kInstallRefusalMessage).
inline constexpr std::string_view kCloseGameMessage = "Close the running game first.";
// When the gate says no at its second check, right before the handoff.
inline constexpr std::string_view kGameRunningCancelledMessage =
    "The update was cancelled. Close the running game first.";

class UpdateSession {
public:
	using ChangeFn = std::function<void(const SessionOutcome&)>;

	// on_change runs after every state change, after each downloaded chunk (State() Downloading,
	// for DownloadedBytes()), and when Install() is refused; it must not destroy the session.
	UpdateSession(UpdateSessionConfig config, kyty::core::HttpClient& http,
	              kyty::core::Scheduler& scheduler, ProcessRunner& runner, InstallGate& gate,
	              ChangeFn on_change);
	// Cancels a running request or step and removes the workspace; while an installation runs
	// (Downloading to Validating), also calls gate.InstallFinished(). No on_change call.
	~UpdateSession();

	UpdateSession(const UpdateSession&)            = delete;
	UpdateSession& operator=(const UpdateSession&) = delete;

	// Fetches the latest release and, when its tag differs from config.installed_tag, the
	// installed release; ends in Offering (the latest release was published later and has a
	// package), NoNewer (same tag, or not published later), or Failed (the reason; at once, without
	// a request, when the configuration has a relative path). Ignored while a check or an
	// installation runs and after Handoff.
	void Check(bool user_activated);

	// Failed at once, in any state, when the configuration has a relative path (Check() refuses
	// such a configuration too, so the session never offers anything with one). Otherwise in
	// Offering only: refused with kCloseGameMessage (state stays Offering) while
	// gate.CanInstallUpdate() is false; otherwise gate.InstallStarted(), the workspace, the
	// download (Downloading), tar -xzf (Extracting), kyty_updater --validate (Validating), a
	// second gate check, and kyty_updater --install started detached (Handoff: quit now). Any
	// failure removes the workspace, calls gate.InstallFinished(), and ends in Failed. The two
	// process steps run from a Scheduler callback each, after on_change has shown their state;
	// ProcessRunner::Run blocks until the process exits.
	void Install();

	// Stops a check, an offer, or an installation before the handoff: Failed with no message.
	void Cancel();

	[[nodiscard]] SessionState State() const;
	// The bytes of the package received by the current or last download.
	[[nodiscard]] uint64_t DownloadedBytes() const;

private:
	struct Impl;
	std::shared_ptr<Impl> m_impl;
};

// The workspace steps of Install(), for fault injection in tests.
enum class WorkspaceStep { CreateWorkspace, CopyHelper, CreatePayload };

// Tests only, not thread-safe: before each workspace step, Install() calls fault(step); a non-zero
// return value is the errno that the step then fails with, without performing it. An empty
// function removes the hook.
void SetUpdateWorkspaceFaultForTesting(std::function<int(WorkspaceStep)> fault);

} // namespace kyty::updater
