// The update UI's texts for the updater's messages (src/launcher/gtk/coreMessageText.h
// SessionMessageText and updateUi.h UpdateMessageText; phase 9 plan V1, D5, D8; phase 7 plan D28,
// W5): a real kyty::updater::UpdateSession, on FakeHttpClient and FakeScheduler (no network) with
// FakeProcessRunner and a fake install gate, is driven into every failure and answer the updater
// words. For each message the session reports, the test asserts that its text has a msgid (and
// plural msgid) that is one of the literals of coreMessageText.cpp's N_(), _(), NGettext(), and
// NGettextNoop() calls, read from the source file, that UpdateMessageText shows that text with its
// parts, and that in the C locale (no catalog: gettext returns the msgid) this is the updater's
// own sentence again. A rewording of a sentence in src/updater, or of the launcher's text for it,
// therefore fails here, and so does a session message without a kind. Every SessionMessage kind
// is reached once at least; the three that a session cannot reach are checked on outcomes made
// here. No display, no gtk_init.

#include "launcher/core/httpClient.h"
#include "launcher/core/text.h"
#include "launcher/gtk/coreMessageText.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/updateUi.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/gtk/fakeServices.h"
#include "support/gtk/sourceLiterals.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/downloadVerifier.h"
#include "updater/core/releaseMetadata.h"
#include "updater/core/updatePolicy.h"
#include "updater/core/updateSession.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <glib.h>
#include <map>
#include <memory>
#include <set>
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
using kyty::updater::SessionMessage;
using kyty::updater::SessionOutcome;
using kyty::updater::SessionState;
using kyty::updater::UpdateSession;
using kyty::updater::WorkspaceStep;
using KytyTests::FakeHttpClient;
using KytyTests::FakeResponse;
using KytyTests::FakeScheduler;

constexpr std::string_view kInstalledTag  = "2026.03.01-abc1234";
constexpr std::string_view kLatestTag     = "2026.04.01-def5678";
constexpr std::string_view kInstalledDate = "2026-03-01T10:00:00Z";
constexpr std::string_view kLaterDate     = "2026-04-01T10:00:00Z";
constexpr std::string_view kEarlierDate   = "2026-02-01T10:00:00Z";

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "UpdateMessageTextTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

const std::set<std::string>& MappingLiterals() {
	static const std::set<std::string> literals = KytyTests::TranslatableLiterals(
	    fs::path(KYTY_TEST_SOURCE_DIR) / "src" / "launcher" / "gtk" / "coreMessageText.cpp");
	return literals;
}

std::set<SessionMessage>& KindsSeen() {
	static std::set<SessionMessage> seen;
	return seen;
}

// The outcome's message is shown as the launcher's text: a msgid of coreMessageText.cpp, shown by
// UpdateMessageText with its parts, and in the C locale the updater's own sentence. The sentence
// inside the unverified frame is checked the same way.
void CheckShown(const SessionOutcome& outcome, const std::string& name) {
	Check(outcome.detail.kind != SessionMessage::None,
	      name + ": \"" + outcome.message + "\" has a kind");
	const auto text = kyty::gtk::SessionMessageText(outcome);
	Check(text.msgid != nullptr && MappingLiterals().count(text.msgid) == 1,
	      name + ": the msgid is a literal of coreMessageText.cpp");
	Check(text.plural_msgid == nullptr || MappingLiterals().count(text.plural_msgid) == 1,
	      name + ": the plural msgid is a literal of coreMessageText.cpp");
	const std::string shown = kyty::gtk::UpdateMessageText(outcome);
	Check(shown == kyty::gtk::Render(text), name + ": UpdateMessageText shows the text");
	Check(shown == outcome.message, name +
	                                    ": in the C locale the text is the updater's sentence (\"" +
	                                    shown + "\" for \"" + outcome.message + "\")");
	KindsSeen().insert(outcome.detail.kind);
	if (outcome.detail.installed_unverified) {
		Check(std::string(text.msgid) == "Could not verify the currently installed release. %s",
		      name + ": the unverified frame");
		const std::string prefix =
		    std::string(kyty::updater::kInstalledReleaseUnverifiedMessage) + " ";
		Check(outcome.message.starts_with(prefix), name + ": the unverified message");
		auto inner                        = outcome;
		inner.message                     = outcome.message.substr(prefix.size());
		inner.detail.installed_unverified = false;
		CheckShown(inner, name + " (its reason)");
	}
}

// ---- An update session on fakes

// The install gate of the session: CanInstallUpdate as the test sets it.
class FakeGate final: public kyty::updater::InstallGate {
public:
	bool CanInstallUpdate() const override { return can_install; }
	void InstallStarted() override {}
	void InstallFinished() override {}

	bool can_install = true;
};

// FakeHttpClient, except that a request for one of the failures' URLs ends with that failure (the
// ends no fake response gives: a total timeout, a cancellation by the client), from the scheduler.
class FailingHttpClient final: public kyty::core::HttpClient {
public:
	explicit FailingHttpClient(FakeScheduler& scheduler): m_scheduler(scheduler), fake(scheduler) {}

	kyty::core::RequestId Get(const HttpRequest& request, ChunkFn on_chunk,
	                          DoneFn on_done) override {
		const auto failure = failures.find(request.url);
		if (failure == failures.end()) {
			return fake.Get(request, std::move(on_chunk), std::move(on_done));
		}
		const HttpFailure kind = failure->second;
		m_scheduler.AfterUnrecorded(std::chrono::milliseconds {0},
		                            [on_done = std::move(on_done), kind]() {
			                            HttpResult result;
			                            result.failure = kind;
			                            result.message = "injected";
			                            on_done(result);
		                            });
		return kOwnRequest;
	}
	void Cancel(kyty::core::RequestId id) override {
		if (id != kOwnRequest) {
			fake.Cancel(id);
		}
	}

	static constexpr kyty::core::RequestId kOwnRequest = 1u << 30u;

private:
	FakeScheduler& m_scheduler;

public:
	FakeHttpClient                     fake;
	std::map<std::string, HttpFailure> failures;
};

const std::string& Package() {
	static const std::string package = UpdaterGen::PatternBytes(5000, 7);
	return package;
}

UpdaterGen::ReleaseSpec Release(std::string_view tag, std::string_view published_at) {
	return UpdaterGen::ValidRelease(tag, published_at, UpdaterGen::DigestOf(Package()),
	                                Package().size());
}

FakeResponse Ok(std::string body) {
	FakeResponse response;
	response.body = std::move(body);
	return response;
}

FakeResponse Ok(const UpdaterGen::ReleaseSpec& spec) {
	return Ok(UpdaterGen::ReleaseDocument(spec));
}

FakeResponse Status(int status) {
	FakeResponse response;
	response.status = status;
	return response;
}

FakeResponse Stalled(std::string body, size_t after) {
	FakeResponse response      = Ok(std::move(body));
	response.stall_after_bytes = after;
	return response;
}

std::string AssetUrl(std::string_view tag = kLatestTag) {
	return kyty::updater::AssetDownloadUrl(tag, kyty::updater::AssetNameForTag(tag));
}

struct Session {
	KytyTests::TempDir                 temp {"kyty_update_text"};
	fs::path                           install = temp.MakeDirectories("install");
	FakeScheduler                      scheduler;
	FailingHttpClient                  http {scheduler};
	KytyTests::FakeProcessRunner       runner;
	FakeGate                           gate;
	std::vector<SessionOutcome>        outcomes;
	std::unique_ptr<UpdateSession>     session;
	kyty::updater::UpdateSessionConfig config;

	Session() {
		Check(UpdaterGen::WriteExecutable(install / "kyty_updater", "#!/bin/sh\nexit 0\n"),
		      "the installed helper");
		config.installed_tag     = std::string(kInstalledTag);
		config.install_dir       = install;
		config.launcher_relative = "kyty_launcher";
		config.helper            = install / "kyty_updater";
		config.launcher_pid      = 4242;
		config.working_dir       = temp.Path();
		Restart();
	}

	void Restart() {
		session.reset();
		session = std::make_unique<UpdateSession>(
		    config, http, scheduler, runner, gate,
		    [this](const SessionOutcome& outcome) { outcomes.push_back(outcome); });
	}

	void Latest(FakeResponse response) {
		http.fake.AddResponse(kyty::updater::LatestReleaseUrl(), std::move(response));
	}
	void Installed(FakeResponse response) {
		http.fake.AddResponse(kyty::updater::ReleaseByTagUrl(kInstalledTag), std::move(response));
	}
	void Asset(FakeResponse response) { http.fake.AddResponse(AssetUrl(), std::move(response)); }
	void ScriptNewer() {
		Latest(Ok(Release(kLatestTag, kLaterDate)));
		Installed(Ok(Release(kInstalledTag, kInstalledDate)));
	}

	// Check(), then Install() when the check offered the release; the last outcome.
	const SessionOutcome& Run(bool install_offer) {
		session->Check(true);
		scheduler.RunUntilIdle();
		if (install_offer && session->State() == SessionState::Offering) {
			session->Install();
			scheduler.RunUntilIdle();
		}
		Check(!outcomes.empty(), "the session reported an outcome");
		return outcomes.back();
	}
};

// The session's message in state after the check (and the installation of the offer when
// install_offer), checked as a shown text; the last outcome.
SessionOutcome CheckSessionMessage(Session& session, bool install_offer, SessionState state,
                                   const std::string& name) {
	const auto last = session.Run(install_offer);
	Check(last.state == state && !last.message.empty(),
	      name + ": the session ends with a message (\"" + last.message + "\")");
	CheckShown(last, name);
	return last;
}

// ---- Tests

void TestCheckFailures() {
	{
		Session session;
		session.Latest(Ok(std::string(2 * 1024 * 1024 + 1, ' ')));
		CheckSessionMessage(session, false, SessionState::Failed, "a 2 MiB + 1 byte response");
	}
	for (const auto& [failure, name]: std::vector<std::pair<HttpFailure, std::string>> {
	         {HttpFailure::TimedOut, "a timed-out check"},
	         {HttpFailure::Cancelled, "a cancelled check"}}) {
		Session session;
		session.http.failures[kyty::updater::LatestReleaseUrl()] = failure;
		CheckSessionMessage(session, false, SessionState::Failed, name);
	}
	for (const int status: {404, 500}) {
		Session session;
		session.Latest(Status(status));
		const auto last =
		    CheckSessionMessage(session, false, SessionState::Failed,
		                        "HTTP status " + std::to_string(status) + " for the check");
		Check(last.detail.number == status, "the check's HTTP status is a part");
	}
	{
		Session session;
		session.Latest(Stalled(UpdaterGen::ReleaseDocument(Release(kLatestTag, kLaterDate)), 10));
		const auto last =
		    CheckSessionMessage(session, false, SessionState::Failed, "a stalled check");
		Check(last.message.find(" 15 seconds.") != std::string::npos && last.detail.number == 15,
		      "the stalled check names 15 seconds");
	}
	{
		Session    session; // no scripted response: the fake answers with a network failure
		const auto last =
		    CheckSessionMessage(session, false, SessionState::Failed, "a network failure");
		Check(last.detail.kind == SessionMessage::CheckNetwork && !last.detail.text.empty() &&
		          last.message.ends_with(last.detail.text),
		      "the network's reason is the dynamic part");
	}
	{
		Session session;
		session.Latest(Ok("{\"tag_name\": \"v1\""));
		CheckSessionMessage(session, false, SessionState::Failed, "an invalid release");
	}
	{
		Session session;
		auto    draft = Release(kLatestTag, kLaterDate);
		draft.draft   = "true";
		session.Latest(Ok(draft));
		CheckSessionMessage(session, false, SessionState::Failed, "a draft release");
	}
	{
		// A relative installation directory: refused before any request.
		Session session;
		session.config.install_dir = "relative/install";
		session.Restart();
		const auto last = CheckSessionMessage(session, false, SessionState::Failed,
		                                      "a relative installation directory");
		Check(last.detail.text == "install_dir" && last.detail.path == "relative/install",
		      "the field and its path are the parts");
	}
}

void TestCheckAnswers() {
	{
		Session session;
		session.Latest(Ok(Release(kLatestTag, kEarlierDate)));
		session.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
		CheckSessionMessage(session, false, SessionState::NoNewer, "no newer release");
	}
	{
		Session session;
		auto    missing = Release(kLatestTag, kLaterDate);
		missing.assets.pop_back();
		session.Latest(Ok(missing));
		session.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
		CheckSessionMessage(session, false, SessionState::Failed, "a missing asset");
	}
	{
		Session session;
		auto    bad              = Release(kLatestTag, kLaterDate);
		bad.assets.back().digest = UpdaterGen::JsonString("sha256:1234");
		session.Latest(Ok(bad));
		session.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
		CheckSessionMessage(session, false, SessionState::Failed, "an invalid asset");
	}
	// The installed release cannot be verified: its reason is shown as a sentence of its own.
	{
		Session session;
		session.Latest(Ok(Release(kLatestTag, kLaterDate)));
		session.Installed(Status(404));
		const auto last =
		    CheckSessionMessage(session, false, SessionState::Failed, "an unverified release");
		Check(last.detail.installed_unverified && last.detail.kind == SessionMessage::CheckStatus,
		      "the unverified release's reason has its own kind");
	}
	{
		Session session;
		session.Latest(Ok(Release(kLatestTag, kLaterDate)));
		session.Installed(Ok(Release("2026.01.01-0000000", kInstalledDate)));
		const auto last = CheckSessionMessage(session, false, SessionState::Failed,
		                                      "GitHub answers with another release");
		Check(last.detail.kind == SessionMessage::OtherRelease &&
		          last.detail.text == "2026.01.01-0000000",
		      "the other release's tag is the part");
	}
}

void TestDownloadFailures() {
	for (const int status: {404, 500}) {
		Session session;
		session.ScriptNewer();
		session.Asset(Status(status));
		CheckSessionMessage(session, true, SessionState::Failed,
		                    "HTTP status " + std::to_string(status) + " for the download");
	}
	{
		Session session;
		session.ScriptNewer();
		session.Asset(Stalled(Package(), 100));
		const auto last =
		    CheckSessionMessage(session, true, SessionState::Failed, "a stalled download");
		Check(last.message.find(" 30 seconds.") != std::string::npos,
		      "the stalled download names 30 seconds");
	}
	for (const auto& [failure, name]: std::vector<std::pair<HttpFailure, std::string>> {
	         {HttpFailure::TimedOut, "a timed-out download"},
	         {HttpFailure::Cancelled, "a cancelled download"},
	         {HttpFailure::Network, "a network failure of the download"}}) {
		Session session;
		session.ScriptNewer();
		session.http.failures[AssetUrl()] = failure;
		CheckSessionMessage(session, true, SessionState::Failed, name);
	}
	{
		Session session;
		session.ScriptNewer();
		session.Asset(Ok(Package().substr(0, 3000)));
		const auto last =
		    CheckSessionMessage(session, true, SessionState::Failed, "a short download");
		Check(last.message.find("(3000 of 5000 bytes)") != std::string::npos &&
		          last.detail.received == 3000 && last.detail.size == 5000,
		      "the short download names its bytes");
	}
	{
		Session session;
		session.ScriptNewer();
		session.Asset(Ok(Package() + "more"));
		CheckSessionMessage(session, true, SessionState::Failed, "an oversized download");
	}
	{
		Session     session;
		std::string corrupt = Package();
		corrupt[100]        = static_cast<char>(corrupt[100] ^ 0x5a);
		session.ScriptNewer();
		session.Asset(Ok(corrupt));
		CheckSessionMessage(session, true, SessionState::Failed, "a download with a wrong digest");
	}
	{
		// A write of the package fails (an injected ENOSPC).
		Session session;
		session.ScriptNewer();
		session.Asset(Ok(Package()));
		kyty::updater::SetDownloadWriteFaultForTesting([] { return ENOSPC; });
		const auto last =
		    CheckSessionMessage(session, true, SessionState::Failed, "a failed package write");
		kyty::updater::SetDownloadWriteFaultForTesting({});
		Check(last.detail.kind == SessionMessage::DownloadWrite &&
		          last.detail.error_number == ENOSPC,
		      "the package write's errno is the part");
	}
	{
		// A release tag that is not a URL path segment cannot be downloaded.
		Session session;
		session.Latest(Ok(Release("..", kLaterDate)));
		session.Installed(Ok(Release(kInstalledTag, kInstalledDate)));
		const auto last =
		    CheckSessionMessage(session, true, SessionState::Failed, "a refused asset URL");
		Check(last.detail.kind == SessionMessage::RefusedUrl && !last.detail.text.empty(),
		      "the refused URL is the part");
	}
}

void TestWorkspaceFailures() {
	for (const auto step: {WorkspaceStep::CreateWorkspace, WorkspaceStep::CopyHelper,
	                       WorkspaceStep::CreatePayload}) {
		Session session;
		session.ScriptNewer();
		session.Asset(Ok(Package()));
		kyty::updater::SetUpdateWorkspaceFaultForTesting(
		    [step](WorkspaceStep current) { return current == step ? EACCES : 0; });
		const auto last =
		    CheckSessionMessage(session, true, SessionState::Failed,
		                        "workspace step " + std::to_string(static_cast<int>(step)));
		kyty::updater::SetUpdateWorkspaceFaultForTesting({});
		Check(last.detail.error_number == EACCES && last.message.ends_with(ErrorText(EACCES)),
		      "the workspace step's errno is the part");
	}
}

void TestProcessFailures() {
	using kyty::updater::ProcessResult;
	const auto run = [](const std::string& name, const std::function<void(Session&)>& setup) {
		Session session;
		session.ScriptNewer();
		session.Asset(Ok(Package()));
		setup(session);
		return CheckSessionMessage(session, true, SessionState::Failed, name);
	};
	run("tar exit code with standard error", [](Session& session) {
		session.runner.run_results.push_back(
		    ProcessResult {2 << 8, "", "tar: This does not look like a tar archive\n"});
	});
	run("tar exit code", [](Session& session) {
		session.runner.run_results.push_back(ProcessResult {2 << 8, "", ""});
	});
	run("tar signal",
	    [](Session& session) { session.runner.run_results.push_back(ProcessResult {9, "", ""}); });
	run("tar stopped", [](Session& session) {
		session.runner.run_results.push_back(ProcessResult {0x137f, "", ""});
	});
	run("tar missing", [](Session& session) {
		ProcessResult result;
		result.error                = "Cannot start /usr/bin/tar: " + ErrorText(ENOENT);
		result.failure.kind         = kyty::updater::ProcessErrorKind::Spawn;
		result.failure.program      = "/usr/bin/tar";
		result.failure.error_number = ENOENT;
		result.failure.message      = result.error;
		session.runner.run_results.push_back(result);
	});
	run("a runner's text without a kind", [](Session& session) {
		session.runner.run_results.push_back(ProcessResult {-1, "the runner's own words", ""});
	});
	run("validation", [](Session& session) {
		session.runner.run_results.push_back(ProcessResult {0, "", ""});
		session.runner.run_results.push_back(
		    ProcessResult {1 << 8, "", "Release is missing an executable: kyty_updater\n"});
	});
	run("a game started before the handoff", [](Session& session) {
		session.runner.on_run = [&session](const fs::path& program) {
			if (program.filename() == "kyty_updater") {
				session.gate.can_install = false;
			}
		};
	});
	run("the installer cannot start", [](Session& session) {
		session.runner.detached_errors.push_back("Cannot start kyty_updater: " + ErrorText(EACCES));
	});
}

void TestGameRunning() {
	// Install while a game runs: refused, the offer stays.
	Session session;
	session.ScriptNewer();
	session.gate.can_install = false;
	CheckSessionMessage(session, true, SessionState::Offering, "Install while a game runs");
}

// The kinds a session cannot reach: Compare() answers Unknown only for a release without a tag
// or a publication time, which the check refuses before it compares; the package file is new in a
// new workspace (O_EXCL cannot fail on it); and the session drops the verifier after Finish().
// Their outcomes are made here with the updater's sentences.
void TestUnreachableKinds() {
	SessionOutcome order;
	order.state       = SessionState::Failed;
	order.message     = std::string(kyty::updater::kUnknownReleaseOrderMessage);
	order.detail.kind = SessionMessage::UnknownReleaseOrder;
	CheckShown(order, "the unknown order");

	SessionOutcome create;
	create.state               = SessionState::Failed;
	create.message             = "Cannot create /i/.kyty-update-x/package: " + ErrorText(EEXIST);
	create.detail.kind         = SessionMessage::DownloadCreate;
	create.detail.path         = "/i/.kyty-update-x/package";
	create.detail.error_number = EEXIST;
	CheckShown(create, "the package cannot be created");

	SessionOutcome finished;
	finished.state       = SessionState::Failed;
	finished.message     = "The download is already finished.";
	finished.detail.kind = SessionMessage::DownloadFinished;
	CheckShown(finished, "the download is already finished");
}

// Every kind was shown once at least, nothing is shown without a message, and a message without a
// kind is shown as it is.
void TestCoverage() {
	for (int kind = static_cast<int>(SessionMessage::None) + 1;
	     kind <= static_cast<int>(SessionMessage::InstallerNotStarted); kind++) {
		Check(KindsSeen().count(static_cast<SessionMessage>(kind)) == 1,
		      "the session message kind " + std::to_string(kind) + " was shown");
	}
	SessionOutcome nothing;
	nothing.state = SessionState::Failed;
	Check(kyty::gtk::SessionMessageText(nothing).msgid == nullptr &&
	          kyty::gtk::UpdateMessageText(nothing).empty(),
	      "no message: no text (a cancel)");
	// A message without a kind (a release error the session does not classify) is never dropped.
	SessionOutcome unclassified;
	unclassified.state   = SessionState::Failed;
	unclassified.message = "A release error without a kind \xff.";
	Check(kyty::gtk::SessionMessageText(unclassified).msgid == nullptr &&
	          kyty::gtk::UpdateMessageText(unclassified) ==
	              kyty::core::SanitizeUtf8(unclassified.message),
	      "a message without a kind is shown as it is, with invalid UTF-8 replaced");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("UpdateMessageTextTests");
	Check(!MappingLiterals().empty(), "coreMessageText.cpp has translatable literals");
	TestCheckFailures();
	TestCheckAnswers();
	TestDownloadFailures();
	TestWorkspaceFailures();
	TestProcessFailures();
	TestGameRunning();
	TestUnreachableKinds();
	TestCoverage();
	std::printf("UpdateMessageTextTests: all tests passed (%zu kinds, %zu literals of "
	            "coreMessageText.cpp)\n",
	            KindsSeen().size(), MappingLiterals().size());
	return 0;
}
