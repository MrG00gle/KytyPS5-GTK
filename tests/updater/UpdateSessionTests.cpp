// Example tests of kyty::updater::UpdateSession (src/updater/core/updateSession.h, task 17.2;
// Requirements 10.4, 13.1, 13.5, 13.8, 13.12, 13.13) through FakeHttpClient and FakeScheduler (no
// network), a fake ProcessRunner, and a fake InstallGate, with the workspace in a TempDir
// installation: every state; a latest release that fails, stalls past 15 s, exceeds 2 MiB, is
// malformed, a draft, a prerelease, without a package, or with a bad digest or size; an equal
// tag; an installed release that is missing, malformed, or another one; older, equal, and newer
// releases; the refusal while a game runs; a download that stalls at 30 s, is too large, too
// small, corrupt, or cannot be written; tar and validation failures with their standard error;
// the gate closing before the handoff; a failed detached start; Cancel() in every running state
// (also from on_change); each workspace fault; a refused URL; a configuration with a relative or
// empty path; destruction mid-download; a session without on_change; and a client that ignores
// max_bytes. Every request URL must be one of the fork's release URLs with the https-only redirect
// policy, and no workspace may remain after a failure.

#include "launcher/core/httpClient.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/downloadVerifier.h"
#include "updater/core/updatePolicy.h"
#include "updater/core/updateSession.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::core::HttpFailure;
using kyty::core::HttpRequest;
using kyty::core::HttpResult;
using kyty::updater::ProcessError;
using kyty::updater::ProcessResult;
using kyty::updater::SessionMessage;
using kyty::updater::SessionOutcome;
using kyty::updater::SessionState;
using kyty::updater::UpdateSession;
using kyty::updater::UpdateSessionConfig;
using kyty::updater::WorkspaceStep;
using KytyTests::FakeHttpClient;
using KytyTests::FakeResponse;
using KytyTests::FakeScheduler;
using KytyTests::TempDir;
using std::chrono::milliseconds;

constexpr std::string_view kInstalledTag  = "2026.03.01-abc1234";
constexpr std::string_view kLatestTag     = "2026.04.01-def5678";
constexpr std::string_view kInstalledDate = "2026-03-01T10:00:00Z";
constexpr std::string_view kLaterDate     = "2026-04-01T10:00:00Z";
constexpr std::string_view kEarlierDate   = "2026-02-01T10:00:00Z";
constexpr int64_t          kLauncherPid   = 4242;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "UpdateSessionTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

const char* StateName(SessionState state) {
	switch (state) {
		case SessionState::Idle: return "Idle";
		case SessionState::FetchingLatest: return "FetchingLatest";
		case SessionState::FetchingInstalled: return "FetchingInstalled";
		case SessionState::Offering: return "Offering";
		case SessionState::NoNewer: return "NoNewer";
		case SessionState::Downloading: return "Downloading";
		case SessionState::Extracting: return "Extracting";
		case SessionState::Validating: return "Validating";
		case SessionState::Handoff: return "Handoff";
		case SessionState::Failed: return "Failed";
	}
	return "?";
}

const std::string& Package() {
	static const std::string package = UpdaterGen::PatternBytes(5000, 7);
	return package;
}

UpdaterGen::ReleaseSpec Release(std::string_view tag, std::string_view published_at,
                                const std::string& package = Package()) {
	return UpdaterGen::ValidRelease(tag, published_at, UpdaterGen::DigestOf(package),
	                                package.size());
}

FakeResponse Ok(std::string body) {
	FakeResponse response;
	response.body = std::move(body);
	return response;
}

FakeResponse Ok(const UpdaterGen::ReleaseSpec& spec) {
	return Ok(UpdaterGen::ReleaseDocument(spec));
}

FakeResponse NetworkFailure() {
	FakeResponse response;
	response.network_failure = true;
	return response;
}

FakeResponse Status(int status) {
	FakeResponse response;
	response.status = status;
	return response;
}

// A release document of exactly size bytes (padded in its "body" member).
std::string DocumentOfSize(size_t size) {
	auto spec          = Release(kLatestTag, kLaterDate);
	spec.other_members = {{"body", UpdaterGen::JsonString("")}};
	const auto base    = UpdaterGen::ReleaseDocument(spec).size();
	spec.other_members = {{"body", UpdaterGen::JsonString(std::string(size - base, 'x'))}};
	return UpdaterGen::ReleaseDocument(spec);
}

class FakeRunner final: public kyty::updater::ProcessRunner {
public:
	struct Call {
		fs::path                 program;
		std::vector<std::string> args;
		fs::path                 cwd;
		fs::path                 log_file;
		bool                     detached = false;
	};

	ProcessResult Run(const fs::path& program, std::vector<std::string> args,
	                  const fs::path& cwd) override {
		calls.push_back({program, std::move(args), cwd, {}, false});
		if (on_run) {
			on_run(program);
		}
		return program == "/usr/bin/tar" ? tar : validate;
	}

	kyty::core::Expected<void, ProcessError> StartDetached(const fs::path&          program,
	                                                       std::vector<std::string> args,
	                                                       const fs::path&          cwd,
	                                                       const fs::path& log_file) override {
		calls.push_back({program, std::move(args), cwd, log_file, true});
		return detached;
	}

	std::vector<Call>                        calls;
	ProcessResult                            tar {0, "", ""};
	ProcessResult                            validate {0, "", ""};
	kyty::core::Expected<void, ProcessError> detached;
	std::function<void(const fs::path&)>     on_run;
};

class FakeGate final: public kyty::updater::InstallGate {
public:
	bool CanInstallUpdate() const override {
		checks++;
		return can_install;
	}
	void InstallStarted() override { started++; }
	void InstallFinished() override { finished++; }

	bool        can_install = true;
	mutable int checks      = 0;
	int         started     = 0;
	int         finished    = 0;
};

// Resets the workspace fault hook however the test ends.
struct WorkspaceFaultReset {
	~WorkspaceFaultReset() { kyty::updater::SetUpdateWorkspaceFaultForTesting({}); }
};

bool IsMetadataRequest(const HttpRequest& request) {
	return request.url.starts_with(kyty::updater::kReleaseApiPrefix);
}

// Every request goes to one of the fork's release URLs, with the limits of its kind.
void CheckRequests(const std::vector<HttpRequest>& requests) {
	for (const auto& request: requests) {
		Check(kyty::updater::IsAllowedUpdateUrl(request.url) &&
		          (request.url.starts_with(kyty::updater::kReleaseApiPrefix) ||
		           request.url.starts_with(kyty::updater::kReleaseDownloadPrefix)),
		      "an allowed URL: " + request.url);
		Check(request.total_timeout == milliseconds {0}, "no total timeout: " + request.url);
		Check(request.redirects == kyty::core::RedirectPolicy::HttpsOnly,
		      "redirects to any https URL (GitHub sends downloads to another host): " +
		          request.url);
		if (IsMetadataRequest(request)) {
			Check(request.max_bytes == 2u * 1024u * 1024u &&
			          request.stall_timeout == milliseconds {15000},
			      "metadata limits: 2 MiB, 15 s");
			Check(request.headers ==
			          std::vector<std::pair<std::string, std::string>> {
			              {"Accept", "application/vnd.github+json"},
			              {"X-GitHub-Api-Version", "2022-11-28"},
			              {"User-Agent", "KytyPS5-GTK-Updater"}},
			      "metadata headers");
		} else {
			Check(request.stall_timeout == milliseconds {30000} && request.max_bytes > 0,
			      "download limits: the asset size, 30 s");
			Check(request.headers ==
			          std::vector<std::pair<std::string, std::string>> {
			              {"User-Agent", "KytyPS5-GTK-Updater"}},
			      "download headers");
		}
	}
}

struct Harness {
	TempDir                        temp {"kyty_session"};
	fs::path                       install = temp.MakeDirectories("install");
	FakeScheduler                  scheduler;
	FakeHttpClient                 http {scheduler};
	FakeRunner                     runner;
	FakeGate                       gate;
	UpdateSessionConfig            config;
	std::vector<SessionOutcome>    outcomes;
	std::function<void()>          on_change;
	std::unique_ptr<UpdateSession> session;

	explicit Harness(std::string installed_tag = std::string(kInstalledTag)) {
		Check(UpdaterGen::WriteExecutable(install / "kyty_updater", "#!/bin/sh\nexit 0\n"),
		      "the installed helper");
		config.installed_tag     = std::move(installed_tag);
		config.install_dir       = install;
		config.launcher_relative = "kyty_launcher";
		config.helper            = install / "kyty_updater";
		config.launcher_pid      = kLauncherPid;
		config.working_dir       = temp.Path() / "launcher cwd";
		Restart();
	}

	~Harness() {
		session.reset();
		CheckRequests(http.Requests());
	}

	Harness(const Harness&)            = delete;
	Harness& operator=(const Harness&) = delete;

	void Restart() {
		session.reset();
		session = std::make_unique<UpdateSession>(config, http, scheduler, runner, gate,
		                                          [this](const SessionOutcome& outcome) {
			                                          outcomes.push_back(outcome);
			                                          if (on_change) {
				                                          on_change();
			                                          }
		                                          });
	}

	void Latest(FakeResponse response) {
		http.AddResponse(kyty::updater::LatestReleaseUrl(), std::move(response));
	}
	void Installed(FakeResponse response) {
		http.AddResponse(kyty::updater::ReleaseByTagUrl(config.installed_tag), std::move(response));
	}
	void Asset(FakeResponse response, std::string_view tag = kLatestTag) {
		http.AddResponse(kyty::updater::AssetDownloadUrl(tag, kyty::updater::AssetNameForTag(tag)),
		                 std::move(response));
	}

	// A newer latest release and the installed one.
	void ScriptNewer(const std::string& package = Package()) {
		Latest(Ok(Release(kLatestTag, kLaterDate, package)));
		Installed(Ok(Release(kInstalledTag, kInstalledDate)));
	}

	void RunCheck(bool user_activated = true) {
		session->Check(user_activated);
		scheduler.RunUntilIdle();
	}
	void RunInstall() {
		session->Install();
		scheduler.RunUntilIdle();
	}

	const SessionOutcome& Last() const {
		Check(!outcomes.empty(), "an outcome was reported");
		return outcomes.back();
	}

	// The update workspaces in the installation directory.
	std::vector<fs::path> Workspaces() const {
		std::vector<fs::path> found;
		std::error_code       ec;
		for (auto it = fs::directory_iterator(install, ec); !ec && it != fs::directory_iterator();
		     it.increment(ec)) {
			if (it->path().filename().string().starts_with(".kyty-update-")) {
				found.push_back(it->path());
			}
		}
		return found;
	}

	size_t AssetRequests() const {
		size_t count = 0;
		for (const auto& request: http.Requests()) {
			count += IsMetadataRequest(request) ? 0 : 1;
		}
		return count;
	}

	// The session ended in Failed with message, without a workspace, and the gate was released
	// for every installation that started.
	void CheckFailed(const std::string& message, const std::string& name) const {
		Check(session->State() == SessionState::Failed && Last().state == SessionState::Failed,
		      name + ": Failed, not " + StateName(Last().state));
		Check(Last().message == message,
		      name + ": message '" + Last().message + "', expected '" + message + "'");
		// Phase 9 V1: every message has its kind (the launcher's text is checked against the
		// message in launcher_gtk_update_message_text).
		Check(message.empty() == (Last().detail.kind == SessionMessage::None),
		      name + ": the message has a kind exactly when it is not empty");
		Check(Workspaces().empty(), name + ": no workspace remains");
		Check(gate.finished == gate.started, name + ": the gate is released");
	}
};

// ---- Checks

void TestOffer() {
	Harness harness;
	harness.ScriptNewer();
	harness.RunCheck(true);
	const auto& last = harness.Last();
	Check(last.state == SessionState::Offering && last.message.empty() && last.user_activated,
	      "a newer release is offered");
	Check(last.release && last.release->tag == kLatestTag && last.release->asset &&
	          last.release->asset->name == kyty::updater::AssetNameForTag(kLatestTag) &&
	          last.release->asset->size == Package().size(),
	      "the offered release and its package");
	Check(harness.outcomes.size() == 3 &&
	          harness.outcomes[0].state == SessionState::FetchingLatest &&
	          harness.outcomes[1].state == SessionState::FetchingInstalled &&
	          !harness.outcomes[0].release && !harness.outcomes[1].release,
	      "FetchingLatest, FetchingInstalled, Offering");
	const auto& requests = harness.http.Requests();
	Check(requests.size() == 2 && requests[0].url == kyty::updater::LatestReleaseUrl() &&
	          requests[1].url == kyty::updater::ReleaseByTagUrl(kInstalledTag),
	      "the latest release, then the installed one");
	Check(harness.Workspaces().empty() && harness.gate.started == 0, "nothing installed yet");
}

void TestNotNewer() {
	struct Case {
		std::string name;
		std::string latest_tag;
		std::string latest_date;
		size_t      requests;
	};
	for (const auto& [name, tag, date, requests]: std::vector<Case> {
	         {"equal tags", std::string(kInstalledTag), std::string(kLaterDate), 1},
	         {"older", std::string(kLatestTag), std::string(kEarlierDate), 2},
	         {"published at the same time", std::string(kLatestTag), std::string(kInstalledDate),
	          2},
	         {"the same time in another offset", std::string(kLatestTag),
	          "2026-03-01T11:00:00+01:00", 2},
	     }) {
		Harness harness;
		harness.Latest(Ok(Release(tag, date)));
		harness.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
		harness.RunCheck(false);
		const auto& last = harness.Last();
		Check(last.state == SessionState::NoNewer &&
		          last.message == kyty::updater::kNoNewerReleaseMessage && !last.user_activated &&
		          !last.release,
		      name + ": no newer release");
		Check(harness.http.Requests().size() == requests, name + ": the requests");
		Check(harness.session->State() == SessionState::NoNewer, name + ": State()");
	}

	// The same tag without a package is not newer either (Qt compared before the package).
	Harness harness;
	auto    spec = Release(kInstalledTag, kInstalledDate);
	spec.assets.pop_back();
	harness.Latest(Ok(spec));
	harness.RunCheck();
	Check(harness.Last().state == SessionState::NoNewer, "the installed release without a package");
}

void TestLatestFailures() {
	auto draft            = Release(kLatestTag, kLaterDate);
	draft.draft           = "true";
	auto prerelease       = Release(kLatestTag, kLaterDate);
	prerelease.prerelease = "true";
	auto no_date          = Release(kLatestTag, kLaterDate);
	no_date.published_at.reset();
	FakeResponse stalled      = Ok(Release(kLatestTag, kLaterDate));
	stalled.stall_after_bytes = 10;
	struct Case {
		std::string  name;
		FakeResponse response;
		std::string  message;
	};
	for (const auto& [name, response, message]: std::vector<Case> {
	         {"network failure", NetworkFailure(),
	          "The update check failed: simulated network failure"},
	         {"HTTP status", Status(500), "The update check failed with HTTP status 500."},
	         {"stalled", stalled, "The update check received no data for 15 seconds."},
	         {"too large", Ok(DocumentOfSize(2 * 1024 * 1024 + 1)),
	          "The update response is larger than 2 MiB."},
	         {"malformed", Ok("{\"tag_name\": \"v1\""),
	          std::string(kyty::updater::kInvalidReleaseMessage)},
	         {"not an object", Ok("[]"), std::string(kyty::updater::kInvalidReleaseMessage)},
	         {"draft", Ok(draft), std::string(kyty::updater::kUnstableReleaseMessage)},
	         {"prerelease", Ok(prerelease), std::string(kyty::updater::kUnstableReleaseMessage)},
	         {"no publication time", Ok(no_date),
	          std::string(kyty::updater::kUnstableReleaseMessage)},
	     }) {
		Harness harness;
		harness.Latest(response);
		harness.RunCheck();
		harness.CheckFailed(message, name);
		Check(harness.http.Requests().size() == 1, name + ": only the latest release");
		Check(!harness.Last().release, name + ": nothing offered");
	}

	// A stall is measured on the virtual clock.
	Harness harness;
	harness.Latest(stalled);
	harness.RunCheck();
	Check(harness.scheduler.Now() == milliseconds {15000}, "the stall ends the request at 15 s");
}

// Slow responses within the limits, and exactly 2 MiB.
void TestWithinLimits() {
	Harness      harness;
	FakeResponse slow    = Ok(Release(kLatestTag, kLaterDate));
	slow.latency         = milliseconds {14999};
	slow.chunk_sizes     = {100, 100, 100};
	slow.chunk_interval  = milliseconds {14999};
	FakeResponse two_mib = Ok(DocumentOfSize(2 * 1024 * 1024));
	two_mib.chunk_sizes  = {1, 1024 * 1024};
	harness.Latest(slow);
	harness.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
	harness.RunCheck();
	Check(harness.Last().state == SessionState::Offering, "a slow response is not a stall");
	Check(harness.scheduler.Now() >= milliseconds {4 * 14999}, "it took about a minute");

	Harness exact;
	exact.Latest(two_mib);
	exact.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
	exact.RunCheck();
	Check(exact.Last().state == SessionState::Offering, "exactly 2 MiB is accepted");
}

void TestPackageProblems() {
	auto missing = Release(kLatestTag, kLaterDate);
	missing.assets.pop_back();
	auto bad_digest             = Release(kLatestTag, kLaterDate);
	bad_digest.assets[1].digest = UpdaterGen::JsonString("sha256:1234");
	auto zero_size              = Release(kLatestTag, kLaterDate);
	zero_size.assets[1].size    = "0";
	auto wrong_name             = Release(kLatestTag, kLaterDate);
	wrong_name.assets[1].name   = UpdaterGen::JsonString("other-Linux-x86_64.tar.gz");
	struct Case {
		std::string             name;
		UpdaterGen::ReleaseSpec spec;
		std::string_view        message;
	};
	for (const auto& [name, spec, message]: std::vector<Case> {
	         {"no package", missing, kyty::updater::kMissingAssetMessage},
	         {"a package of another name", wrong_name, kyty::updater::kMissingAssetMessage},
	         {"a bad digest", bad_digest, kyty::updater::kInvalidAssetMessage},
	         {"size 0", zero_size, kyty::updater::kInvalidAssetMessage},
	     }) {
		Harness harness;
		harness.Latest(Ok(spec));
		harness.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
		harness.RunCheck();
		harness.CheckFailed(std::string(message), name);
		Check(harness.http.Requests().size() == 2 && harness.AssetRequests() == 0,
		      name + ": no download");
	}
}

void TestInstalledFailures() {
	auto other_tag = Release("2026.01.01-0000000", kInstalledDate);
	auto no_date   = Release(kInstalledTag, kInstalledDate);
	no_date.published_at.reset();
	FakeResponse stalled      = Ok(Release(kInstalledTag, kInstalledDate));
	stalled.stall_after_bytes = 0;
	const std::string prefix = std::string(kyty::updater::kInstalledReleaseUnverifiedMessage) + " ";
	struct Case {
		std::string  name;
		FakeResponse response;
		std::string  reason;
	};
	for (const auto& [name, response, reason]: std::vector<Case> {
	         {"missing", Status(404), "The update check failed with HTTP status 404."},
	         {"network failure", NetworkFailure(),
	          "The update check failed: simulated network failure"},
	         {"stalled", stalled, "The update check received no data for 15 seconds."},
	         {"too large", Ok(std::string(2 * 1024 * 1024 + 1, ' ')),
	          "The update response is larger than 2 MiB."},
	         {"malformed", Ok("not JSON"), std::string(kyty::updater::kInvalidReleaseMessage)},
	         {"no publication time", Ok(no_date),
	          std::string(kyty::updater::kUnstableReleaseMessage)},
	         {"another release", Ok(other_tag),
	          "GitHub answered with the release 2026.01.01-0000000."},
	     }) {
		Harness harness;
		harness.Latest(Ok(Release(kLatestTag, kLaterDate)));
		harness.Installed(response);
		harness.RunCheck();
		harness.CheckFailed(prefix + reason, name);
		Check(harness.http.Requests().size() == 2 && harness.AssetRequests() == 0,
		      name + ": no download");
	}

	// An installed tag that is not a URL path segment is refused before any request.
	Harness refused("..");
	refused.Latest(Ok(Release(kLatestTag, kLaterDate)));
	refused.RunCheck();
	refused.CheckFailed(prefix + "Refused to request " + kyty::updater::ReleaseByTagUrl("..") +
	                        ": it is not a release URL of MrG00gle/KytyPS5-GTK.",
	                    "a refused installed tag");
	Check(refused.http.Requests().size() == 1, "the refused URL is never requested");

	// A draft installed release still has a publication time to compare (decision D33/U7).
	Harness draft;
	auto    installed = Release(kInstalledTag, kInstalledDate);
	installed.draft   = "true";
	draft.Latest(Ok(Release(kLatestTag, kLaterDate)));
	draft.Installed(Ok(installed));
	draft.RunCheck();
	Check(draft.Last().state == SessionState::Offering, "a draft installed release compares");
}

// ---- Install

void TestInstall() {
	Harness harness;
	harness.ScriptNewer();
	FakeResponse asset = Ok(Package());
	asset.chunk_sizes  = {1000, 0, 1500};
	harness.Asset(asset);
	harness.RunCheck();
	Check(harness.Last().state == SessionState::Offering, "offered");

	std::vector<SessionState> states_at_run;
	harness.runner.on_run = [&](const fs::path&) { states_at_run.push_back(harness.Last().state); };
	const auto first      = harness.outcomes.size();
	harness.session->Install();
	Check(harness.Last().state == SessionState::Downloading && harness.gate.started == 1 &&
	          harness.gate.checks == 1,
	      "Install() starts the download after one gate check");
	const auto workspaces = harness.Workspaces();
	Check(workspaces.size() == 1, "one workspace");
	const auto workspace = workspaces[0];
	Check(workspace.filename().string().size() == std::string(".kyty-update-XXXXXX").size(),
	      "the workspace name: " + workspace.string());
	const auto helper = workspace / "kyty_updater";
	Check(UpdaterGen::ReadFile(helper) == std::string("#!/bin/sh\nexit 0\n"), "the helper copy");
	std::error_code ec;
	Check((fs::status(helper, ec).permissions() & fs::perms::owner_exec) != fs::perms::none,
	      "the helper copy is executable");
	Check(fs::is_directory(workspace / "payload", ec), "the payload directory");

	harness.scheduler.RunUntilIdle();
	std::vector<SessionState> states;
	for (size_t i = first; i < harness.outcomes.size(); i++) {
		states.push_back(harness.outcomes[i].state);
		Check(harness.outcomes[i].release && harness.outcomes[i].release->tag == kLatestTag,
		      "the release is in every installation outcome");
	}
	Check(states == std::vector<SessionState> {SessionState::Downloading, SessionState::Downloading,
	                                           SessionState::Downloading, SessionState::Downloading,
	                                           SessionState::Extracting, SessionState::Validating,
	                                           SessionState::Handoff},
	      "Downloading (start and three chunks), Extracting, Validating, Handoff");
	Check(states_at_run ==
	          std::vector<SessionState> {SessionState::Extracting, SessionState::Validating},
	      "each process step runs after its state was reported");
	Check(harness.session->State() == SessionState::Handoff &&
	          harness.session->DownloadedBytes() == Package().size(),
	      "Handoff after the whole package");
	Check(harness.scheduler.RequestedDelays() ==
	          std::vector<milliseconds> {milliseconds {0}, milliseconds {0}},
	      "the two process steps are deferred");

	const auto  payload = (workspace / "payload").string();
	const auto  package = (workspace / "package-Linux-x86_64.tar.gz").string();
	const auto& calls   = harness.runner.calls;
	Check(calls.size() == 3, "tar, validate, install");
	Check(calls[0].program == "/usr/bin/tar" && !calls[0].detached &&
	          calls[0].args == std::vector<std::string> {"-xzf", package, "-C", payload,
	                                                     "--no-same-owner",
	                                                     "--no-same-permissions"} &&
	          calls[0].cwd == workspace,
	      "tar -xzf into the payload");
	Check(calls[1].program == helper && !calls[1].detached &&
	          calls[1].args == std::vector<std::string> {"--validate", payload,
	                                                     harness.install.string(),
	                                                     "kyty_launcher"} &&
	          calls[1].cwd == workspace,
	      "kyty_updater --validate from the workspace");
	Check(calls[2].program == helper && calls[2].detached &&
	          calls[2].args == std::vector<std::string> {"--install", payload,
	                                                     harness.install.string(), "kyty_launcher",
	                                                     "4242",
	                                                     harness.config.working_dir.string()} &&
	          calls[2].cwd == workspace && calls[2].log_file == workspace / "helper.log",
	      "kyty_updater --install started detached with its log");
	Check(UpdaterGen::ReadFile(package) == Package(), "the verified package stays for the helper");
	Check(harness.gate.checks == 2 && harness.gate.started == 1 && harness.gate.finished == 0,
	      "the gate is checked again before the handoff and stays in the installation");
	const auto& requests = harness.http.Requests();
	Check(requests.size() == 3 &&
	          requests[2].url == kyty::updater::AssetDownloadUrl(
	                                 kLatestTag, kyty::updater::AssetNameForTag(kLatestTag)) &&
	          requests[2].max_bytes == Package().size(),
	      "the package download");

	// After the handoff nothing more happens, and the helper keeps the workspace.
	const auto reported = harness.outcomes.size();
	harness.session->Check(true);
	harness.session->Install();
	harness.session->Cancel();
	Check(harness.outcomes.size() == reported, "Check, Install, Cancel do nothing after Handoff");
	harness.session.reset();
	Check(harness.Workspaces().size() == 1 && harness.gate.finished == 0,
	      "the workspace and the installation belong to the helper");
}

// Requirement 10.4: refused while a game runs, with the state kept.
void TestRefusedWhileRunning() {
	Harness harness;
	harness.ScriptNewer();
	harness.Asset(Ok(Package()));
	harness.RunCheck();
	harness.gate.can_install = false;
	harness.session->Install();
	const auto& last = harness.Last();
	Check(last.state == SessionState::Offering && last.message == "Close the running game first." &&
	          last.release,
	      "refused with the Requirement 10.4 message");
	Check(harness.session->State() == SessionState::Offering && harness.gate.started == 0 &&
	          harness.Workspaces().empty() && harness.AssetRequests() == 0,
	      "nothing started");
	harness.gate.can_install = true;
	harness.RunInstall();
	Check(harness.session->State() == SessionState::Handoff, "installed once the game closed");
}

void TestDownloadFailures() {
	std::string corrupt = Package();
	corrupt[1234] ^= 0x55;
	FakeResponse stalled      = Ok(Package());
	stalled.stall_after_bytes = 1000;
	FakeResponse longer       = Ok(Package() + "extra");
	FakeResponse shorter      = Ok(Package().substr(0, 4000));
	struct Case {
		std::string  name;
		FakeResponse response;
		std::string  message;
	};
	for (const auto& [name, response, message]: std::vector<Case> {
	         {"stalled", stalled, "The download received no data for 30 seconds."},
	         {"too large", longer, std::string(kyty::updater::kDownloadTooLargeMessage)},
	         {"too small", shorter,
	          "The downloaded file is smaller than the release asset (4000 of 5000 bytes)."},
	         {"corrupt", Ok(corrupt), std::string(kyty::updater::kDownloadDigestMessage)},
	         {"network failure", NetworkFailure(),
	          "The download failed: simulated network failure"},
	         {"HTTP status", Status(404), "The download failed with HTTP status 404."},
	     }) {
		Harness harness;
		harness.ScriptNewer();
		harness.Asset(response);
		harness.RunCheck();
		harness.RunInstall();
		harness.CheckFailed(message, name);
		Check(harness.gate.started == 1 && harness.gate.finished == 1, name + ": gate released");
		Check(harness.runner.calls.empty(), name + ": nothing extracted");
		Check(harness.Last().release.has_value(), name + ": the offered release stays known");
	}

	Harness harness;
	harness.ScriptNewer();
	harness.Asset(stalled);
	harness.RunCheck();
	const auto start = harness.scheduler.Now();
	harness.RunInstall();
	Check(harness.scheduler.Now() - start == milliseconds {30000}, "the download stalls at 30 s");

	// A slow download within the stall limit succeeds.
	Harness      slow;
	FakeResponse chunks   = Ok(Package());
	chunks.chunk_sizes    = {1000, 1000, 1000, 1000};
	chunks.chunk_interval = milliseconds {29999};
	slow.ScriptNewer();
	slow.Asset(chunks);
	slow.RunCheck();
	slow.RunInstall();
	Check(slow.session->State() == SessionState::Handoff, "29.999 s between chunks is no stall");

	// A write error of the package file.
	Harness written;
	written.ScriptNewer();
	written.Asset(Ok(Package()));
	written.RunCheck();
	kyty::updater::SetDownloadWriteFaultForTesting([] { return ENOSPC; });
	written.RunInstall();
	kyty::updater::SetDownloadWriteFaultForTesting({});
	const auto& message = written.Last().message;
	Check(message.starts_with("Cannot write " + written.install.string() + "/.kyty-update-") &&
	          message.ends_with("/package-Linux-x86_64.tar.gz: " + ErrorText(ENOSPC)),
	      "the write error: " + message);
	written.CheckFailed(message, "write error");
}

void TestProcessFailures() {
	struct Case {
		std::string                                 name;
		std::function<void(FakeRunner&, FakeGate&)> setup;
		std::string                                 message;
		size_t                                      calls;
	};
	for (const auto& [name, setup, message, calls]: std::vector<Case> {
	         {"tar exit code with standard error",
	          [](FakeRunner& runner, FakeGate&) {
		          runner.tar = {2 << 8, "", "tar: This does not look like a tar archive\n"};
	          },
	          "Could not prepare the update: /usr/bin/tar exited with code 2:\n"
	          "tar: This does not look like a tar archive\n",
	          1},
	         {"tar exit code", [](FakeRunner& runner, FakeGate&) { runner.tar = {2 << 8, "", ""}; },
	          "Could not prepare the update: /usr/bin/tar exited with code 2.", 1},
	         {"tar signal", [](FakeRunner& runner, FakeGate&) { runner.tar = {9, "", ""}; },
	          "Could not prepare the update: /usr/bin/tar was ended by signal 9.", 1},
	         {"tar stopped", [](FakeRunner& runner, FakeGate&) { runner.tar = {0x137f, "", ""}; },
	          "Could not prepare the update: /usr/bin/tar ended with wait status 4991.", 1},
	         {"tar missing",
	          [](FakeRunner& runner, FakeGate&) {
		          runner.tar = {-1, "Cannot start /usr/bin/tar: " + ErrorText(ENOENT), ""};
	          },
	          "Could not prepare the update: Cannot start /usr/bin/tar: " + ErrorText(ENOENT) + ".",
	          1},
	         {"validation",
	          [](FakeRunner& runner, FakeGate&) {
		          runner.validate = {1 << 8, "",
		                             "Release is missing an executable: kyty_updater\n"};
	          },
	          "Could not prepare the update: kyty_updater --validate exited with code 1:\n"
	          "Release is missing an executable: kyty_updater\n",
	          2},
	         {"a game started before the handoff",
	          [](FakeRunner& runner, FakeGate& gate) {
		          runner.on_run = [&gate](const fs::path& program) {
			          if (program != "/usr/bin/tar") {
				          gate.can_install = false;
			          }
		          };
	          },
	          "The update was cancelled. Close the running game first.", 2},
	         {"detached start",
	          [](FakeRunner& runner, FakeGate&) {
		          ProcessError error;
		          error.kind         = kyty::updater::ProcessErrorKind::Spawn;
		          error.program      = "kyty_updater";
		          error.error_number = EACCES;
		          error.message      = "Cannot start kyty_updater: " + ErrorText(EACCES);
		          runner.detached    = kyty::core::MakeUnexpected(std::move(error));
	          },
	          "Could not start the update installer: Cannot start kyty_updater: " +
	              ErrorText(EACCES),
	          3},
	     }) {
		Harness harness;
		harness.ScriptNewer();
		harness.Asset(Ok(Package()));
		setup(harness.runner, harness.gate);
		harness.RunCheck();
		harness.RunInstall();
		harness.CheckFailed(message, name);
		Check(harness.runner.calls.size() == calls, name + ": the process calls");
	}

	// The parts of the step messages.
	const auto detail_of = [](const std::function<void(FakeRunner&)>& setup) {
		Harness harness;
		harness.ScriptNewer();
		harness.Asset(Ok(Package()));
		setup(harness.runner);
		harness.RunCheck();
		harness.RunInstall();
		return harness.Last().detail;
	};
	auto detail = detail_of([](FakeRunner& runner) {
		runner.tar = {2 << 8, "", "tar: This does not look like a tar archive\n"};
	});
	Check(detail.kind == SessionMessage::StepExited && detail.text == "/usr/bin/tar" &&
	          detail.number == 2 &&
	          detail.standard_error == "tar: This does not look like a tar archive\n",
	      "tar's exit: the step, the code, and the standard error");
	detail = detail_of([](FakeRunner& runner) { runner.tar = {9, "", ""}; });
	Check(detail.kind == SessionMessage::StepSignaled && detail.number == 9 &&
	          detail.standard_error.empty(),
	      "tar's signal");
	detail = detail_of([](FakeRunner& runner) { runner.tar = {0x137f, "", ""}; });
	Check(detail.kind == SessionMessage::StepWaitStatus && detail.number == 0x137f,
	      "tar's wait status");
	detail = detail_of([](FakeRunner& runner) {
		runner.tar.wait_status          = -1;
		runner.tar.error                = "Cannot start /usr/bin/tar: " + ErrorText(ENOENT);
		runner.tar.failure.kind         = kyty::updater::ProcessErrorKind::Spawn;
		runner.tar.failure.program      = "/usr/bin/tar";
		runner.tar.failure.error_number = ENOENT;
		runner.tar.failure.message      = runner.tar.error;
	});
	Check(detail.kind == SessionMessage::StepNotStarted &&
	          detail.process.kind == kyty::updater::ProcessErrorKind::Spawn &&
	          detail.process.error_number == ENOENT && detail.process.program == "/usr/bin/tar",
	      "tar not started: the process error");
	detail = detail_of([](FakeRunner& runner) {
		ProcessError error;
		error.kind         = kyty::updater::ProcessErrorKind::OpenLog;
		error.program      = "/x/kyty_updater";
		error.error_number = ENOSPC;
		error.path         = "/x/helper.log";
		error.message   = "Cannot start /x/kyty_updater: cannot open the log file /x/helper.log: " +
		                  ErrorText(ENOSPC);
		runner.detached = kyty::core::MakeUnexpected(error);
	});
	Check(detail.kind == SessionMessage::InstallerNotStarted &&
	          detail.process.kind == kyty::updater::ProcessErrorKind::OpenLog &&
	          detail.process.path == "/x/helper.log",
	      "the installer not started: the process error");
}

void TestWorkspaceFaults() {
	const WorkspaceFaultReset reset;
	for (const auto step: {WorkspaceStep::CreateWorkspace, WorkspaceStep::CopyHelper,
	                       WorkspaceStep::CreatePayload}) {
		Harness harness;
		harness.ScriptNewer();
		harness.Asset(Ok(Package()));
		harness.RunCheck();
		fs::path workspace;
		kyty::updater::SetUpdateWorkspaceFaultForTesting([&](WorkspaceStep current) {
			if (const auto found = harness.Workspaces(); !found.empty()) {
				workspace = found[0];
			}
			return current == step ? EACCES : 0;
		});
		harness.RunInstall();
		kyty::updater::SetUpdateWorkspaceFaultForTesting({});
		std::string expected;
		switch (step) {
			case WorkspaceStep::CreateWorkspace:
				expected = "Cannot create an update folder in " + harness.install.string() + ": " +
				           ErrorText(EACCES);
				break;
			case WorkspaceStep::CopyHelper:
				expected = "Cannot copy " + harness.config.helper.string() + " to " +
				           (workspace / "kyty_updater").string() + ": " + ErrorText(EACCES);
				break;
			case WorkspaceStep::CreatePayload:
				expected =
				    "Cannot create " + (workspace / "payload").string() + ": " + ErrorText(EACCES);
				break;
		}
		harness.CheckFailed(expected, "workspace step " + std::to_string(static_cast<int>(step)));
		Check(harness.AssetRequests() == 0 && harness.gate.started == 1,
		      "no download after a workspace failure");
		const auto& detail = harness.Last().detail;
		Check(detail.error_number == EACCES, "the workspace step's errno");
		switch (step) {
			case WorkspaceStep::CreateWorkspace:
				Check(detail.kind == SessionMessage::CreateWorkspace &&
				          detail.path == harness.install,
				      "CreateWorkspace: the installation directory");
				break;
			case WorkspaceStep::CopyHelper:
				Check(detail.kind == SessionMessage::CopyHelper &&
				          detail.path == harness.config.helper &&
				          detail.other == workspace / "kyty_updater",
				      "CopyHelper: the helper and its copy");
				break;
			case WorkspaceStep::CreatePayload:
				Check(detail.kind == SessionMessage::CreatePayload &&
				          detail.path == workspace / "payload",
				      "CreatePayload: the payload directory");
				break;
		}
	}

	// Real failures: a missing helper and a missing installation directory.
	Harness no_helper;
	no_helper.config.helper = no_helper.install / "missing";
	no_helper.Restart();
	no_helper.ScriptNewer();
	no_helper.RunCheck();
	no_helper.RunInstall();
	Check(no_helper.Last().message.starts_with("Cannot copy " +
	                                           (no_helper.install / "missing").string() + " to ") &&
	          no_helper.Last().message.ends_with(": " + ErrorText(ENOENT)),
	      "a missing helper: " + no_helper.Last().message);
	no_helper.CheckFailed(no_helper.Last().message, "a missing helper");

	Harness no_install;
	no_install.config.install_dir = no_install.temp.Path() / "missing";
	no_install.Restart();
	no_install.ScriptNewer();
	no_install.RunCheck();
	no_install.RunInstall();
	no_install.CheckFailed("Cannot create an update folder in " +
	                           (no_install.temp.Path() / "missing").string() + ": " +
	                           ErrorText(ENOENT),
	                       "a missing installation directory");
}

// A release tag that is not a URL path segment cannot be downloaded.
void TestRefusedAssetUrl() {
	Harness harness;
	harness.Latest(Ok(Release("..", kLaterDate)));
	harness.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
	harness.RunCheck();
	Check(harness.Last().state == SessionState::Offering, "the release is offered");
	harness.RunInstall();
	harness.CheckFailed(
	    "Refused to request " +
	        kyty::updater::AssetDownloadUrl("..", kyty::updater::AssetNameForTag("..")) +
	        ": it is not a release URL of MrG00gle/KytyPS5-GTK.",
	    "a refused download");
	Check(harness.AssetRequests() == 0, "the refused URL is never requested");
}

// ---- Cancel, busy states, destruction

void TestCancel() {
	// While fetching the latest and the installed release.
	for (const auto state: {SessionState::FetchingLatest, SessionState::FetchingInstalled}) {
		Harness      harness;
		FakeResponse slow = Ok(Release(kLatestTag, kLaterDate));
		slow.latency      = milliseconds {1000};
		harness.Latest(slow);
		FakeResponse installed = Ok(Release(kInstalledTag, kInstalledDate));
		installed.latency      = milliseconds {1000};
		harness.Installed(installed);
		harness.session->Check(true);
		if (state == SessionState::FetchingInstalled) {
			harness.scheduler.AdvanceBy(milliseconds {1500});
		}
		Check(harness.session->State() == state, std::string("in ") + StateName(state));
		harness.session->Cancel();
		harness.CheckFailed("", std::string("cancelled in ") + StateName(state));
		const auto reported = harness.outcomes.size();
		harness.scheduler.RunUntilIdle();
		Check(harness.outcomes.size() == reported && harness.http.ActiveCount() == 0,
		      "the cancelled request ends and reports nothing");
	}

	// An offer.
	{
		Harness harness;
		harness.ScriptNewer();
		harness.RunCheck();
		harness.session->Cancel();
		harness.CheckFailed("", "the offer declined");
		Check(harness.Last().release.has_value(), "the declined release is still reported");
	}

	// During the download.
	{
		Harness      harness;
		FakeResponse slow   = Ok(Package());
		slow.chunk_sizes    = {1000, 1000};
		slow.chunk_interval = milliseconds {1000};
		harness.ScriptNewer();
		harness.Asset(slow);
		harness.RunCheck();
		harness.session->Install();
		harness.scheduler.AdvanceBy(milliseconds {1500});
		Check(harness.session->State() == SessionState::Downloading &&
		          harness.session->DownloadedBytes() == 2000,
		      "two chunks downloaded");
		harness.session->Cancel();
		harness.CheckFailed("", "cancelled during the download");
		const auto reported = harness.outcomes.size();
		harness.scheduler.RunUntilIdle();
		Check(harness.outcomes.size() == reported && harness.runner.calls.empty(),
		      "the cancelled download reports nothing more");
	}

	// From on_change: in a download chunk, in Extracting (before tar), and in Validating.
	for (const auto state:
	     {SessionState::Downloading, SessionState::Extracting, SessionState::Validating}) {
		Harness harness;
		harness.ScriptNewer();
		FakeResponse asset = Ok(Package());
		asset.chunk_sizes  = {100};
		harness.Asset(asset);
		harness.RunCheck();
		int chunk_outcomes = 0;
		harness.on_change  = [&] {
			const auto current = harness.Last().state;
			if (current == SessionState::Downloading) {
				chunk_outcomes++;
			}
			if (current == state && (state != SessionState::Downloading || chunk_outcomes == 2)) {
				harness.session->Cancel();
			}
		};
		harness.RunInstall();
		harness.CheckFailed("", std::string("cancelled from on_change in ") + StateName(state));
		const size_t expected_calls = state == SessionState::Validating ? 1 : 0;
		Check(harness.runner.calls.size() == expected_calls,
		      std::string("no later step after a cancel in ") + StateName(state));
	}

	// Nothing to cancel.
	Harness harness;
	harness.session->Cancel();
	Check(harness.outcomes.empty() && harness.session->State() == SessionState::Idle,
	      "Cancel() in Idle");
	harness.Latest(Ok(Release(kInstalledTag, kInstalledDate)));
	harness.RunCheck();
	const auto reported = harness.outcomes.size();
	harness.session->Cancel();
	Check(harness.outcomes.size() == reported && harness.session->State() == SessionState::NoNewer,
	      "Cancel() in NoNewer");
}

void TestBusyAndRestart() {
	Harness harness;
	harness.ScriptNewer();
	harness.ScriptNewer();
	harness.session->Install();
	Check(harness.outcomes.empty(), "Install() in Idle does nothing");
	harness.session->Check(true);
	harness.session->Check(false);
	harness.session->Install();
	Check(harness.http.Requests().size() == 1 && harness.outcomes.size() == 1,
	      "a second Check() and Install() while fetching do nothing");
	harness.scheduler.RunUntilIdle();
	Check(harness.Last().state == SessionState::Offering && harness.Last().user_activated,
	      "the first check's flag");

	// Check() from Offering starts again.
	harness.session->Check(false);
	Check(harness.Last().state == SessionState::FetchingLatest && !harness.Last().release &&
	          !harness.Last().user_activated,
	      "a new check forgets the offer");
	harness.scheduler.RunUntilIdle();
	Check(harness.Last().state == SessionState::Offering, "and offers again");

	// Check() from Failed starts again.
	harness.Latest(NetworkFailure());
	harness.Latest(Ok(Release(kInstalledTag, kInstalledDate)));
	harness.RunCheck();
	Check(harness.Last().state == SessionState::Failed, "failed");
	harness.RunCheck();
	Check(harness.Last().state == SessionState::NoNewer, "a check after a failure");
	Check(harness.session->DownloadedBytes() == 0, "nothing downloaded");
}

// install_dir, helper, and working_dir must be absolute (tar and the helper run with the workspace
// as their working directory): a relative or empty one ends Check() in Failed at once, before any
// request, gate call, or workspace step, and the first such field is named.
void TestRelativeConfiguration() {
	const std::string prefix = "The update configuration is invalid: ";
	struct Case {
		std::string                               name;
		std::function<void(UpdateSessionConfig&)> edit;
		std::string                               message;
	};
	const std::vector<Case> cases = {
	    {"relative install_dir", [](UpdateSessionConfig& c) { c.install_dir = "install"; },
	     prefix + "install_dir must be an absolute path (install)."},
	    {"empty install_dir", [](UpdateSessionConfig& c) { c.install_dir.clear(); },
	     prefix + "install_dir must be an absolute path ()."},
	    {"relative helper", [](UpdateSessionConfig& c) { c.helper = "./kyty_updater"; },
	     prefix + "helper must be an absolute path (./kyty_updater)."},
	    {"empty helper", [](UpdateSessionConfig& c) { c.helper.clear(); },
	     prefix + "helper must be an absolute path ()."},
	    {"relative working_dir", [](UpdateSessionConfig& c) { c.working_dir = "launcher cwd"; },
	     prefix + "working_dir must be an absolute path (launcher cwd)."},
	    {"empty working_dir", [](UpdateSessionConfig& c) { c.working_dir.clear(); },
	     prefix + "working_dir must be an absolute path ()."},
	    {"relative helper and working_dir",
	     [](UpdateSessionConfig& c) {
		     c.helper      = "bin/kyty_updater";
		     c.working_dir = ".";
	     },
	     prefix + "helper must be an absolute path (bin/kyty_updater)."},
	};
	for (const auto& c: cases) {
		Harness harness;
		c.edit(harness.config);
		harness.Restart();
		harness.RunCheck(true);
		Check(harness.outcomes.size() == 1, c.name + ": one outcome, Failed at once");
		harness.CheckFailed(c.message, c.name);
		Check(harness.Last().user_activated && !harness.Last().release,
		      c.name + ": the check's flag, no release");
		harness.RunCheck(false);
		Check(harness.outcomes.size() == 2 && !harness.Last().user_activated,
		      c.name + ": every check is refused");
		harness.CheckFailed(c.message, c.name + ", again");

		// Install() refuses the configuration as well, in any state (here Failed, and Idle).
		harness.RunInstall();
		Check(harness.outcomes.size() == 3, c.name + ": Install() reports the refusal");
		harness.CheckFailed(c.message, c.name + ", Install()");
		Harness idle;
		c.edit(idle.config);
		idle.Restart();
		idle.RunInstall();
		Check(idle.outcomes.size() == 1, c.name + ": Install() in Idle ends in Failed at once");
		idle.CheckFailed(c.message, c.name + ", Install() in Idle");

		for (const Harness* h: {&harness, &idle}) {
			Check(h->http.Requests().empty() && h->runner.calls.empty(),
			      c.name + ": no request and no process");
			Check(h->gate.checks == 0 && h->gate.started == 0 && h->gate.finished == 0,
			      c.name + ": the gate is untouched");
			Check(h->Workspaces().empty(), c.name + ": no workspace");
		}
	}
}

// on_change is optional.
void TestWithoutObserver() {
	Harness harness;
	harness.session = std::make_unique<UpdateSession>(
	    harness.config, harness.http, harness.scheduler, harness.runner, harness.gate, nullptr);
	harness.Latest(Ok(Release(kInstalledTag, kInstalledDate)));
	harness.RunCheck();
	Check(harness.session->State() == SessionState::NoNewer && harness.outcomes.empty(),
	      "a session without on_change still decides");
}

void TestDestruction() {
	Harness      harness;
	FakeResponse slow   = Ok(Package());
	slow.chunk_sizes    = {1000};
	slow.chunk_interval = milliseconds {5000};
	harness.ScriptNewer();
	harness.Asset(slow);
	harness.RunCheck();
	harness.session->Install();
	harness.scheduler.AdvanceBy(milliseconds {100});
	Check(harness.Workspaces().size() == 1 && harness.session->DownloadedBytes() == 1000,
	      "a download runs");
	const auto reported = harness.outcomes.size();
	harness.session.reset();
	Check(harness.Workspaces().empty() && harness.gate.finished == 1,
	      "destruction removes the workspace and releases the gate");
	harness.scheduler.RunUntilIdle();
	Check(harness.outcomes.size() == reported && harness.http.ActiveCount() == 0,
	      "the cancelled request finds the session gone");
}

// A client that ignores max_bytes and reports the rarer failures: the session keeps its own
// limit and words every failure.
class ManualHttpClient final: public kyty::core::HttpClient {
public:
	struct Pending {
		HttpRequest request;
		ChunkFn     on_chunk;
		DoneFn      on_done;
	};

	kyty::core::RequestId Get(const HttpRequest& request, ChunkFn on_chunk,
	                          DoneFn on_done) override {
		pending.push_back({request, std::move(on_chunk), std::move(on_done)});
		return pending.size();
	}
	void Cancel(kyty::core::RequestId id) override { cancelled.push_back(id); }

	std::vector<Pending>               pending;
	std::vector<kyty::core::RequestId> cancelled;
};

void TestClientFailures() {
	const TempDir temp("kyty_session");
	const auto    install = temp.MakeDirectories("install");
	Check(UpdaterGen::WriteExecutable(install / "kyty_updater", "#!/bin/sh\n"), "the helper");
	UpdateSessionConfig config;
	config.installed_tag     = kInstalledTag;
	config.install_dir       = install;
	config.launcher_relative = "kyty_launcher";
	config.helper            = install / "kyty_updater";
	config.working_dir       = temp.Path();
	FakeScheduler               scheduler;
	ManualHttpClient            http;
	FakeRunner                  runner;
	FakeGate                    gate;
	std::vector<SessionOutcome> outcomes;
	UpdateSession session(config, http, scheduler, runner, gate,
	                      [&](const SessionOutcome& outcome) { outcomes.push_back(outcome); });
	const auto    bytes = [](std::string_view text) { return UpdaterGen::AsBytes(text); };

	// More than 2 MiB in one chunk: the session stops the body itself.
	session.Check(true);
	const std::string big(2 * 1024 * 1024 + 1, ' ');
	Check(http.pending.back().on_chunk(bytes(big.substr(0, 10))), "a small chunk is taken");
	Check(!http.pending.back().on_chunk(bytes(big)), "the chunk past 2 MiB stops the request");
	http.pending.back().on_done({HttpFailure::Cancelled, 200, "aborted"});
	Check(outcomes.back().state == SessionState::Failed &&
	          outcomes.back().message == "The update response is larger than 2 MiB.",
	      "the session's own limit: " + outcomes.back().message);

	for (const auto& [failure, message]: std::vector<std::pair<HttpFailure, std::string>> {
	         {HttpFailure::TimedOut, "The update check timed out."},
	         {HttpFailure::Cancelled, "The update check was cancelled."},
	     }) {
		session.Check(true);
		http.pending.back().on_done({failure, 0, "x"});
		Check(outcomes.back().state == SessionState::Failed && outcomes.back().message == message,
		      "metadata failure: " + outcomes.back().message);
	}

	for (const auto& [failure, message]: std::vector<std::pair<HttpFailure, std::string>> {
	         {HttpFailure::TimedOut, "The download timed out."},
	         {HttpFailure::Cancelled, std::string(kyty::updater::kDownloadAbortedMessage)},
	     }) {
		session.Check(true);
		const auto latest    = UpdaterGen::ReleaseDocument(Release(kLatestTag, kLaterDate));
		const auto installed = UpdaterGen::ReleaseDocument(Release(kInstalledTag, kInstalledDate));
		Check(http.pending.back().on_chunk(bytes(latest)), "the latest release");
		http.pending.back().on_done({});
		Check(http.pending.back().on_chunk(bytes(installed)), "the installed release");
		http.pending.back().on_done({});
		Check(outcomes.back().state == SessionState::Offering, "offered");
		session.Install();
		Check(
		    session.State() == SessionState::Downloading &&
		        http.pending.back().request.url.starts_with(kyty::updater::kReleaseDownloadPrefix),
		    "downloading");
		http.pending.back().on_done({failure, 0, "x"});
		Check(outcomes.back().state == SessionState::Failed && outcomes.back().message == message,
		      "download failure: " + outcomes.back().message);
	}
	Check(gate.started == 2 && gate.finished == 2, "every installation released the gate");
	CheckRequests([&] {
		std::vector<HttpRequest> requests;
		for (const auto& pending: http.pending) {
			requests.push_back(pending.request);
		}
		return requests;
	}());
}

} // namespace

int main() {
	TestOffer();
	TestNotNewer();
	TestLatestFailures();
	TestWithinLimits();
	TestPackageProblems();
	TestInstalledFailures();
	TestInstall();
	TestRefusedWhileRunning();
	TestDownloadFailures();
	TestProcessFailures();
	TestWorkspaceFaults();
	TestRefusedAssetUrl();
	TestCancel();
	TestBusyAndRestart();
	TestRelativeConfiguration();
	TestWithoutObserver();
	TestDestruction();
	TestClientFailures();
	std::printf("UpdateSessionTests: all tests passed\n");
	return 0;
}
