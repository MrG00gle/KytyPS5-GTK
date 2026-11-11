#include "updater/core/updateSession.h"

#include "updater/core/downloadVerifier.h"
#include "updater/core/updatePolicy.h"

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <utility>
#include <vector>

namespace kyty::updater {

namespace {

namespace fs = std::filesystem;
using kyty::core::HttpFailure;
using kyty::core::HttpRequest;
using kyty::core::HttpResult;

constexpr std::string_view kMetadataTooLargeMessage = "The update response is larger than 2 MiB.";

std::function<int(WorkspaceStep)>& WorkspaceFaultHook() {
	static std::function<int(WorkspaceStep)> hook;
	return hook;
}

int InjectedFault(WorkspaceStep step) {
	const auto& hook = WorkspaceFaultHook();
	return hook ? hook(step) : 0;
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

int64_t Seconds(std::chrono::milliseconds duration) {
	return duration.count() / 1000;
}

// A session message: the English text and its parts (SessionOutcome::message and detail).
struct Reason {
	std::string   message;
	SessionDetail detail;
};

Reason Said(SessionMessage kind, std::string message) {
	Reason reason;
	reason.message     = std::move(message);
	reason.detail.kind = kind;
	return reason;
}

Reason Said(SessionMessage kind, std::string_view message) {
	return Said(kind, std::string(message));
}

Reason Said(SessionMessage kind, const char* message) {
	return Said(kind, std::string(message));
}

Reason RefusedUrl(const std::string& url) {
	auto reason        = Said(SessionMessage::RefusedUrl, "Refused to request " + url +
	                                                          ": it is not a release URL of " +
	                                                          std::string(kRepository) + ".");
	reason.detail.text = url;
	return reason;
}

// "The update check ..." or "The download ...", with a number.
Reason Numbered(SessionMessage kind, std::string message, int64_t number) {
	auto reason          = Said(kind, std::move(message));
	reason.detail.number = number;
	return reason;
}

// The reason of a failed release metadata request (Requirement 13.12).
Reason DescribeMetadataFailure(const HttpResult& result) {
	switch (result.failure) {
		case HttpFailure::Network: {
			auto reason =
			    Said(SessionMessage::CheckNetwork, "The update check failed: " + result.message);
			reason.detail.text = result.message;
			return reason;
		}
		case HttpFailure::Status:
			return Numbered(SessionMessage::CheckStatus,
			                "The update check failed with HTTP status " +
			                    std::to_string(result.status) + ".",
			                result.status);
		case HttpFailure::Stalled:
			return Numbered(SessionMessage::CheckStalled,
			                "The update check received no data for " +
			                    std::to_string(Seconds(kMetadataStallTimeout)) + " seconds.",
			                Seconds(kMetadataStallTimeout));
		case HttpFailure::TimedOut:
			return Said(SessionMessage::CheckTimedOut, "The update check timed out.");
		case HttpFailure::Cancelled:
			return Said(SessionMessage::CheckCancelled, "The update check was cancelled.");
		case HttpFailure::None:
		case HttpFailure::TooLarge: break;
	}
	return Said(SessionMessage::CheckTooLarge, kMetadataTooLargeMessage);
}

// The reason of a failed package download (Requirement 13.8).
Reason DescribeDownloadFailure(const HttpResult& result) {
	switch (result.failure) {
		case HttpFailure::Network: {
			auto reason =
			    Said(SessionMessage::DownloadNetwork, "The download failed: " + result.message);
			reason.detail.text = result.message;
			return reason;
		}
		case HttpFailure::Status:
			return Numbered(SessionMessage::DownloadStatus,
			                "The download failed with HTTP status " +
			                    std::to_string(result.status) + ".",
			                result.status);
		case HttpFailure::Stalled:
			return Numbered(SessionMessage::DownloadStalled,
			                "The download received no data for " +
			                    std::to_string(Seconds(kAssetStallTimeout)) + " seconds.",
			                Seconds(kAssetStallTimeout));
		case HttpFailure::TimedOut:
			return Said(SessionMessage::DownloadTimedOut, "The download timed out.");
		case HttpFailure::Cancelled:
			return Said(SessionMessage::DownloadCancelled, kDownloadAbortedMessage);
		case HttpFailure::None:
		case HttpFailure::TooLarge: break;
	}
	return Said(SessionMessage::DownloadTooLarge, kDownloadTooLargeMessage);
}

// The reason of a failed Append() or Finish() of the verifier: its message and its parts.
Reason DescribeVerifierFailure(std::string message, const DownloadFailure& failure,
                               const fs::path& path) {
	SessionMessage kind = SessionMessage::DownloadCancelled;
	switch (failure.kind) {
		case DownloadFailureKind::None:
		case DownloadFailureKind::Aborted: break;
		case DownloadFailureKind::Create: kind = SessionMessage::DownloadCreate; break;
		case DownloadFailureKind::Write: kind = SessionMessage::DownloadWrite; break;
		case DownloadFailureKind::TooLarge: kind = SessionMessage::DownloadTooLarge; break;
		case DownloadFailureKind::TooSmall: kind = SessionMessage::DownloadTooSmall; break;
		case DownloadFailureKind::Digest: kind = SessionMessage::DownloadDigest; break;
		case DownloadFailureKind::Finished: kind = SessionMessage::DownloadFinished; break;
	}
	auto reason                = Said(kind, std::move(message));
	reason.detail.path         = path;
	reason.detail.error_number = failure.error_number;
	reason.detail.received     = failure.received;
	reason.detail.size         = failure.size;
	return reason;
}

// The reason of a release without a usable package or not to compare (releaseMetadata.h).
Reason DescribeReleaseError(const std::string& error) {
	const std::pair<std::string_view, SessionMessage> known[] = {
	    {kInvalidReleaseMessage, SessionMessage::InvalidRelease},
	    {kUnstableReleaseMessage, SessionMessage::UnstableRelease},
	    {kInvalidAssetMessage, SessionMessage::InvalidAsset},
	    {kMissingAssetMessage, SessionMessage::MissingAsset}};
	for (const auto& [text, kind]: known) {
		if (error == text) {
			return Said(kind, error);
		}
	}
	return Said(SessionMessage::None, error);
}

// nullopt when the process ran and exited 0; otherwise why the step failed, with the process's
// standard error.
std::optional<Reason> DescribeStepFailure(std::string_view step, const ProcessResult& result) {
	Reason      reason;
	std::string what;
	if (!result.error.empty()) {
		what                          = result.error;
		reason.detail.kind            = SessionMessage::StepNotStarted;
		reason.detail.process         = result.failure;
		reason.detail.process.message = result.error;
	} else if (WIFEXITED(result.wait_status)) {
		if (WEXITSTATUS(result.wait_status) == 0) {
			return std::nullopt;
		}
		what = std::string(step) + " exited with code " +
		       std::to_string(WEXITSTATUS(result.wait_status));
		reason.detail.kind   = SessionMessage::StepExited;
		reason.detail.number = WEXITSTATUS(result.wait_status);
	} else if (WIFSIGNALED(result.wait_status)) {
		what = std::string(step) + " was ended by signal " +
		       std::to_string(WTERMSIG(result.wait_status));
		reason.detail.kind   = SessionMessage::StepSignaled;
		reason.detail.number = WTERMSIG(result.wait_status);
	} else {
		what = std::string(step) + " ended with wait status " + std::to_string(result.wait_status);
		reason.detail.kind   = SessionMessage::StepWaitStatus;
		reason.detail.number = result.wait_status;
	}
	if (reason.detail.kind != SessionMessage::StepNotStarted) {
		reason.detail.text = std::string(step);
	}
	reason.detail.standard_error = result.standard_error;
	reason.message               = "Could not prepare the update: " + what;
	if (result.standard_error.empty()) {
		reason.message += ".";
	} else {
		reason.message += ":\n" + result.standard_error;
	}
	return reason;
}

// Why config cannot be used: the first of install_dir, helper, and working_dir that is not an
// absolute path (an empty path is not one either); nullopt when all three are.
std::optional<Reason> ConfigurationError(const UpdateSessionConfig& config) {
	const std::pair<std::string_view, const fs::path*> fields[] = {
	    {"install_dir", &config.install_dir},
	    {"helper", &config.helper},
	    {"working_dir", &config.working_dir}};
	for (const auto& [name, path]: fields) {
		if (!path->is_absolute()) {
			auto reason        = Said(SessionMessage::InvalidConfiguration,
			                          "The update configuration is invalid: " + std::string(name) +
			                              " must be an absolute path (" + path->string() + ").");
			reason.detail.text = std::string(name);
			reason.detail.path = *path;
			return reason;
		}
	}
	return std::nullopt;
}

// A check or an installation step runs, or the helper took over.
bool IsRunning(SessionState state) {
	switch (state) {
		case SessionState::FetchingLatest:
		case SessionState::FetchingInstalled:
		case SessionState::Downloading:
		case SessionState::Extracting:
		case SessionState::Validating:
		case SessionState::Handoff: return true;
		case SessionState::Idle:
		case SessionState::Offering:
		case SessionState::NoNewer:
		case SessionState::Failed: break;
	}
	return false;
}

} // namespace

struct UpdateSession::Impl: std::enable_shared_from_this<Impl> {
	using ReleaseDone = void (Impl::*)(const ReleaseInfo* release, Reason error);
	using Step        = void (Impl::*)();

	Impl(UpdateSessionConfig session_config, kyty::core::HttpClient& http_client,
	     kyty::core::Scheduler& session_scheduler, ProcessRunner& process_runner,
	     InstallGate& install_gate, ChangeFn change)
	    : config(std::move(session_config)), http(http_client), scheduler(session_scheduler),
	      runner(process_runner), gate(install_gate), on_change(std::move(change)) {}

	UpdateSessionConfig     config;
	kyty::core::HttpClient& http;
	kyty::core::Scheduler&  scheduler;
	ProcessRunner&          runner;
	InstallGate&            gate;
	ChangeFn                on_change;

	SessionState  state = SessionState::Idle;
	std::string   message;
	SessionDetail detail;
	bool          user_activated = false;
	// Callbacks of requests and timers started before the last Stop() find a newer generation and
	// do nothing.
	uint64_t                          generation = 0;
	kyty::core::RequestId             request    = 0;
	kyty::core::TimerId               timer      = 0;
	std::string                       body;
	bool                              body_too_large = false;
	std::optional<ReleaseInfo>        latest;
	bool                              offered    = false;
	bool                              installing = false; // between InstallStarted and the end
	fs::path                          workspace;
	std::unique_ptr<DownloadVerifier> verifier;
	std::optional<Reason>             download_error;
	uint64_t                          downloaded = 0;

	void Notify() {
		if (!on_change) {
			return;
		}
		SessionOutcome outcome;
		outcome.state   = state;
		outcome.message = message;
		outcome.detail  = detail;
		if (offered) {
			outcome.release = latest;
		}
		outcome.user_activated = user_activated;
		on_change(outcome);
	}

	// A resting state, reported at once.
	void Enter(SessionState next, Reason reason = {}) {
		state   = next;
		message = std::move(reason.message);
		detail  = std::move(reason.detail);
		Notify();
	}

	// A running state: start runs its first action, then next is reported unless start already
	// ended the session (a synchronous failure).
	template <typename Start>
	void Begin(SessionState next, Start start) {
		state = next;
		message.clear();
		detail                        = {};
		const auto started_generation = generation;
		start();
		if (generation == started_generation && state == next) {
			Notify();
		}
	}

	// Stops the running request or step, deletes an unfinished download, removes the workspace,
	// and ends the installation at the gate.
	void Stop() {
		generation++;
		if (request != 0) {
			http.Cancel(request);
			request = 0;
		}
		if (timer != 0) {
			scheduler.Cancel(timer);
			timer = 0;
		}
		verifier.reset();
		if (!workspace.empty()) {
			std::error_code ignored;
			fs::remove_all(workspace, ignored);
			workspace.clear();
		}
		if (installing) {
			installing = false;
			gate.InstallFinished();
		}
	}

	void Fail(Reason reason) {
		Stop();
		Enter(SessionState::Failed, std::move(reason));
	}

	// ---- Check

	void Check(bool activated) {
		if (IsRunning(state)) {
			return;
		}
		Stop();
		user_activated = activated;
		latest.reset();
		offered    = false;
		downloaded = 0;
		if (auto error = ConfigurationError(config)) {
			Fail(std::move(*error));
			return;
		}
		Begin(SessionState::FetchingLatest,
		      [this] { FetchRelease(LatestReleaseUrl(), &Impl::LatestFetched); });
	}

	void FetchRelease(const std::string& url, ReleaseDone done) {
		if (!IsAllowedUpdateUrl(url)) {
			(this->*done)(nullptr, RefusedUrl(url));
			return;
		}
		body.clear();
		body_too_large = false;
		HttpRequest metadata;
		metadata.url                   = url;
		metadata.headers               = {{"Accept", "application/vnd.github+json"},
		                                  {"X-GitHub-Api-Version", "2022-11-28"},
		                                  {"User-Agent", std::string(kUpdateUserAgent)}};
		metadata.max_bytes             = kMaxReleaseMetadataBytes;
		metadata.stall_timeout         = kMetadataStallTimeout;
		const std::weak_ptr<Impl> weak = weak_from_this();
		const auto                started_generation = generation;
		request                                      = http.Get(
		    metadata,
		    [weak, started_generation](std::span<const std::byte> chunk) {
			    const auto self = weak.lock();
			    return self && self->generation == started_generation && self->MetadataChunk(chunk);
		    },
		    [weak, started_generation, done](const HttpResult& result) {
			    const auto self = weak.lock();
			    if (self && self->generation == started_generation) {
				    self->request = 0;
				    self->MetadataDone(result, done);
			    }
		    });
	}

	// The client enforces max_bytes too; this keeps a client that does not within the limit.
	bool MetadataChunk(std::span<const std::byte> chunk) {
		if (chunk.size() > kMaxReleaseMetadataBytes - body.size()) {
			body_too_large = true;
			return false;
		}
		body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
		return true;
	}

	void MetadataDone(const HttpResult& result, ReleaseDone done) {
		if (body_too_large) {
			(this->*done)(nullptr, Said(SessionMessage::CheckTooLarge, kMetadataTooLargeMessage));
			return;
		}
		if (result.failure != HttpFailure::None) {
			(this->*done)(nullptr, DescribeMetadataFailure(result));
			return;
		}
		const auto release = ParseRelease(body);
		body.clear();
		(this->*done)(&release, {});
	}

	void LatestFetched(const ReleaseInfo* release, Reason error) {
		if (release == nullptr) {
			Fail(std::move(error));
			return;
		}
		// Not a published stable release (or not a release at all): nothing to compare.
		if (release->error == kInvalidReleaseMessage || release->error == kUnstableReleaseMessage) {
			Fail(DescribeReleaseError(release->error));
			return;
		}
		latest = *release;
		if (latest->tag == config.installed_tag) {
			Decide(*latest);
			return;
		}
		// Tags cannot order releases (Requirement 13.5): compare the publication times.
		Begin(SessionState::FetchingInstalled, [this] {
			FetchRelease(ReleaseByTagUrl(config.installed_tag), &Impl::InstalledFetched);
		});
	}

	void InstalledFetched(const ReleaseInfo* release, Reason error) {
		Reason reason = std::move(error);
		if (release != nullptr) {
			if (release->tag.empty() || !release->published_at) {
				reason = DescribeReleaseError(release->error);
			} else if (release->tag != config.installed_tag) {
				reason             = Said(SessionMessage::OtherRelease,
				                          "GitHub answered with the release " + release->tag + ".");
				reason.detail.text = release->tag;
			} else {
				Decide(*release);
				return;
			}
		}
		reason.message = std::string(kInstalledReleaseUnverifiedMessage) + " " + reason.message;
		reason.detail.installed_unverified = true;
		Fail(std::move(reason));
	}

	void Decide(const ReleaseInfo& installed) {
		const auto relation = Compare(*latest, installed);
		if (relation == Relation::NotNewer) {
			Enter(SessionState::NoNewer,
			      Said(SessionMessage::NoNewerRelease, kNoNewerReleaseMessage));
			return;
		}
		if (relation == Relation::Unknown) {
			Fail(Said(SessionMessage::UnknownReleaseOrder, kUnknownReleaseOrderMessage));
			return;
		}
		// A newer release without a usable package (Requirement 13.6).
		if (!latest->asset) {
			Fail(DescribeReleaseError(latest->error));
			return;
		}
		offered = true;
		Enter(SessionState::Offering);
	}

	// ---- Install

	void Install() {
		// Refused in any state: Check() never runs or offers with such a configuration, so the
		// session is in Idle or Failed here and nothing needs stopping.
		if (auto error = ConfigurationError(config)) {
			Fail(std::move(*error));
			return;
		}
		if (state != SessionState::Offering) {
			return;
		}
		if (!gate.CanInstallUpdate()) {
			message     = kCloseGameMessage;
			detail      = {};
			detail.kind = SessionMessage::CloseGame;
			Notify();
			return;
		}
		gate.InstallStarted();
		installing = true;
		downloaded = 0;
		if (auto error = CreateWorkspace()) {
			Fail(std::move(*error));
			return;
		}
		Begin(SessionState::Downloading, [this] { StartDownload(); });
	}

	// <install_dir>/.kyty-update-XXXXXX with the helper copy and an empty payload directory;
	// nullopt or why it failed.
	std::optional<Reason> CreateWorkspace() {
		const auto failed = [](SessionMessage kind, std::string message, const fs::path& path,
		                       const fs::path& other, int error_number) {
			auto reason                = Said(kind, std::move(message));
			reason.detail.path         = path;
			reason.detail.other        = other;
			reason.detail.error_number = error_number;
			return reason;
		};
		std::string pattern =
		    (config.install_dir / (std::string(kWorkspacePrefix) + "XXXXXX")).string();
		int error_number = InjectedFault(WorkspaceStep::CreateWorkspace);
		if (error_number == 0 && ::mkdtemp(pattern.data()) == nullptr) {
			error_number = errno;
		}
		if (error_number != 0) {
			return failed(SessionMessage::CreateWorkspace,
			              "Cannot create an update folder in " + config.install_dir.string() +
			                  ": " + ErrorText(error_number),
			              config.install_dir, {}, error_number);
		}
		workspace = pattern;

		const auto  helper = workspace / kHelperFileName;
		std::string reason;
		int         copy_errno = 0;
		if (const int fault = InjectedFault(WorkspaceStep::CopyHelper); fault != 0) {
			reason     = ErrorText(fault);
			copy_errno = fault;
		} else {
			// copy_file copies the permission bits too, so the copy stays executable.
			std::error_code ec;
			fs::copy_file(config.helper, helper, ec);
			if (ec) {
				reason     = ec.message();
				copy_errno = ec.value();
			}
		}
		if (!reason.empty()) {
			return failed(SessionMessage::CopyHelper,
			              "Cannot copy " + config.helper.string() + " to " + helper.string() +
			                  ": " + reason,
			              config.helper, helper, copy_errno);
		}

		const auto payload = workspace / kPayloadFileName;
		error_number       = InjectedFault(WorkspaceStep::CreatePayload);
		if (error_number == 0 && ::mkdir(payload.c_str(), 0777) != 0) {
			error_number = errno;
		}
		if (error_number != 0) {
			return failed(SessionMessage::CreatePayload,
			              "Cannot create " + payload.string() + ": " + ErrorText(error_number),
			              payload, {}, error_number);
		}
		return std::nullopt;
	}

	void StartDownload() {
		const auto& asset = *latest->asset;
		const auto  url   = AssetDownloadUrl(latest->tag, asset.name);
		if (!IsAllowedUpdateUrl(url)) {
			Fail(RefusedUrl(url));
			return;
		}
		verifier = std::make_unique<DownloadVerifier>(asset, workspace / kPackageFileName);
		download_error.reset();
		HttpRequest download;
		download.url                   = url;
		download.headers               = {{"User-Agent", std::string(kUpdateUserAgent)}};
		download.max_bytes             = asset.size;
		download.stall_timeout         = kAssetStallTimeout;
		const std::weak_ptr<Impl> weak = weak_from_this();
		const auto                started_generation = generation;
		request                                      = http.Get(
		    download,
		    [weak, started_generation](std::span<const std::byte> chunk) {
			    const auto self = weak.lock();
			    return self && self->generation == started_generation && self->DownloadChunk(chunk);
		    },
		    [weak, started_generation](const HttpResult& result) {
			    const auto self = weak.lock();
			    if (self && self->generation == started_generation) {
				    self->request = 0;
				    self->DownloadDone(result);
			    }
		    });
	}

	bool DownloadChunk(std::span<const std::byte> chunk) {
		if (const auto appended = verifier->Append(chunk); !appended) {
			download_error = DescribeVerifierFailure(appended.error(), verifier->LastFailure(),
			                                         verifier->Path());
			return false;
		}
		downloaded                     = verifier->Received();
		const auto notified_generation = generation;
		Notify();
		// on_change may have cancelled the session.
		return generation == notified_generation;
	}

	void DownloadDone(const HttpResult& result) {
		if (download_error) {
			Fail(std::move(*download_error));
			return;
		}
		if (result.failure != HttpFailure::None) {
			Fail(DescribeDownloadFailure(result));
			return;
		}
		if (const auto finished = verifier->Finish(); !finished) {
			Fail(DescribeVerifierFailure(finished.error(), verifier->LastFailure(),
			                             verifier->Path()));
			return;
		}
		verifier.reset(); // keeps the verified package
		Begin(SessionState::Extracting, [this] { Defer(&Impl::Extract); });
	}

	// Runs step from a Scheduler callback, so the UI shows the state before a blocking process.
	void Defer(Step step) {
		const std::weak_ptr<Impl> weak               = weak_from_this();
		const auto                started_generation = generation;
		timer = scheduler.After(std::chrono::milliseconds {0}, [weak, started_generation, step] {
			const auto self = weak.lock();
			if (self && self->generation == started_generation) {
				self->timer = 0;
				((*self).*step)();
			}
		});
	}

	void Extract() {
		const auto payload = workspace / kPayloadFileName;
		const auto result =
		    runner.Run(fs::path(kTarProgram),
		               {"-xzf", (workspace / kPackageFileName).string(), "-C", payload.string(),
		                "--no-same-owner", "--no-same-permissions"},
		               workspace);
		if (auto failure = DescribeStepFailure(kTarProgram, result)) {
			Fail(std::move(*failure));
			return;
		}
		Begin(SessionState::Validating, [this] { Defer(&Impl::ValidateAndHandOff); });
	}

	void ValidateAndHandOff() {
		const auto helper  = workspace / kHelperFileName;
		const auto payload = workspace / kPayloadFileName;
		const auto result = runner.Run(helper,
		                               {"--validate", payload.string(), config.install_dir.string(),
		                                config.launcher_relative.string()},
		                               workspace);
		if (auto failure = DescribeStepFailure("kyty_updater --validate", result)) {
			Fail(std::move(*failure));
			return;
		}
		// A game may have started while the release was prepared (as Qt's Restart() checked).
		if (!gate.CanInstallUpdate()) {
			Fail(Said(SessionMessage::GameRunningCancelled, kGameRunningCancelledMessage));
			return;
		}
		const auto started =
		    runner.StartDetached(helper,
		                         {"--install", payload.string(), config.install_dir.string(),
		                          config.launcher_relative.string(),
		                          std::to_string(config.launcher_pid), config.working_dir.string()},
		                         workspace, workspace / kHelperLogFileName);
		if (!started) {
			auto reason = Said(SessionMessage::InstallerNotStarted,
			                   "Could not start the update installer: " + started.error().message);
			reason.detail.process = started.error();
			Fail(std::move(reason));
			return;
		}
		// The helper owns the workspace now, and the installation stays in progress at the gate
		// until the launcher quits.
		workspace.clear();
		installing = false;
		Enter(SessionState::Handoff);
	}

	void Cancel() {
		if (state == SessionState::Offering ||
		    (IsRunning(state) && state != SessionState::Handoff)) {
			Fail({});
		}
	}
};

UpdateSession::UpdateSession(UpdateSessionConfig config, kyty::core::HttpClient& http,
                             kyty::core::Scheduler& scheduler, ProcessRunner& runner,
                             InstallGate& gate, ChangeFn on_change)
    : m_impl(std::make_shared<Impl>(std::move(config), http, scheduler, runner, gate,
                                    std::move(on_change))) {}

UpdateSession::~UpdateSession() {
	m_impl->Stop();
}

void UpdateSession::Check(bool user_activated) {
	m_impl->Check(user_activated);
}

void UpdateSession::Install() {
	m_impl->Install();
}

void UpdateSession::Cancel() {
	m_impl->Cancel();
}

SessionState UpdateSession::State() const {
	return m_impl->state;
}

uint64_t UpdateSession::DownloadedBytes() const {
	return m_impl->downloaded;
}

void SetUpdateWorkspaceFaultForTesting(std::function<int(WorkspaceStep)> fault) {
	WorkspaceFaultHook() = std::move(fault);
}

} // namespace kyty::updater
