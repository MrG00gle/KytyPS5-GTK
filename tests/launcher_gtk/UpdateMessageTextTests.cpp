// The update UI's copies of the updater's sentences (src/launcher/gtk/updateUi.h
// UpdateMessageKeyFor and UpdateMessageText; phase 7 plan D28, W5; phase 6 p6-u12-5): a real
// kyty::updater::UpdateSession, on FakeHttpClient and FakeScheduler (no network) with
// FakeProcessRunner and a fake install gate, is driven into every failure and answer the updater
// words. For each message the session reports, the test asserts that it has a key whose msgid
// (and plural msgid) is one of the literals of updateUi.cpp's N_(), _(), NGettext(), and
// NGettextNoop() calls, read from the source file, that UpdateMessageText equals the gettext
// lookup of the key with its numbers, and that in the C locale (no catalog: gettext returns the
// msgid) this is the updater's own sentence again. A rewording of a sentence in src/updater, or of
// its copy in updateUi.cpp, therefore fails here. No display, no gtk_init.

#include "launcher/core/httpClient.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/updateUi.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/gtk/fakeServices.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/downloadVerifier.h"
#include "updater/core/releaseMetadata.h"
#include "updater/core/updatePolicy.h"
#include "updater/core/updateSession.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::core::HttpFailure;
using kyty::core::HttpRequest;
using kyty::core::HttpResult;
using kyty::updater::SessionOutcome;
using kyty::updater::SessionState;
using kyty::updater::UpdateSession;
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

// ---- The literals of updateUi.cpp

bool IsIdentifier(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Skips white space and comments from position.
size_t SkipSpace(const std::string& text, size_t position) {
	while (position < text.size()) {
		if (std::isspace(static_cast<unsigned char>(text[position])) != 0) {
			position++;
		} else if (text.compare(position, 2, "//") == 0) {
			position = text.find('\n', position);
			position = position == std::string::npos ? text.size() : position;
		} else if (text.compare(position, 2, "/*") == 0) {
			position = text.find("*/", position);
			position = position == std::string::npos ? text.size() : position + 2;
		} else {
			break;
		}
	}
	return position;
}

// The string literal at position (adjacent literals joined, the escapes of these sources: \" \\
// \n), and the position after it; nullopt when no literal starts there.
std::optional<std::pair<std::string, size_t>> Literal(const std::string& text, size_t position) {
	std::string value;
	bool        found = false;
	position          = SkipSpace(text, position);
	while (position < text.size() && text[position] == '"') {
		found = true;
		for (position++; position < text.size() && text[position] != '"'; position++) {
			if (text[position] == '\\' && position + 1 < text.size()) {
				const char escaped = text[++position];
				value += escaped == 'n' ? '\n' : escaped;
			} else {
				value += text[position];
			}
		}
		position = SkipSpace(text, position + 1);
	}
	if (!found) {
		return std::nullopt;
	}
	return std::make_pair(value, position);
}

// Every msgid of the N_(), _(), NGettext(), and NGettextNoop() calls in the file (both forms of
// the plural ones).
std::set<std::string> TranslatableLiterals(const fs::path& file) {
	std::ifstream input(file, std::ios::binary);
	Check(input.good(), "read " + file.string());
	std::stringstream buffer;
	buffer << input.rdbuf();
	const std::string     text = buffer.str();
	std::set<std::string> literals;
	for (size_t i = 0; i < text.size(); i++) {
		if (!IsIdentifier(text[i]) || (i > 0 && IsIdentifier(text[i - 1]))) {
			continue;
		}
		size_t end = i;
		while (end < text.size() && IsIdentifier(text[end])) {
			end++;
		}
		const std::string name = text.substr(i, end - i);
		const size_t      open = SkipSpace(text, end);
		const bool        known =
		    name == "_" || name == "N_" || name == "NGettext" || name == "NGettextNoop";
		if (!known || open >= text.size() || text[open] != '(') {
			i = end - 1;
			continue;
		}
		auto first = Literal(text, open + 1);
		if (first) {
			literals.insert(first->first);
			if ((name == "NGettext" || name == "NGettextNoop") && first->second < text.size() &&
			    text[first->second] == ',') {
				if (auto second = Literal(text, first->second + 1)) {
					literals.insert(second->first);
				}
			}
		}
		i = end - 1;
	}
	return literals;
}

const std::set<std::string>& UpdateUiLiterals() {
	static const std::set<std::string> literals = TranslatableLiterals(
	    fs::path(KYTY_TEST_SOURCE_DIR) / "src" / "launcher" / "gtk" / "updateUi.cpp");
	return literals;
}

// ---- The checks

// The gettext lookup of key (g_dgettext or g_dngettext in the kyty-launcher domain) with its
// numbers.
std::string Lookup(const kyty::gtk::UpdateMessageKey& key) {
	using Count               = unsigned long long;
	const auto&       numbers = key.numbers;
	const char* const text =
	    key.plural_msgid != nullptr
	        ? g_dngettext(GETTEXT_PACKAGE, key.msgid, key.plural_msgid,
	                      static_cast<unsigned long>(numbers.empty() ? 0 : numbers.back()))
	        : g_dgettext(GETTEXT_PACKAGE, key.msgid);
	gchar* formatted = nullptr;
	switch (numbers.size()) {
		case 0: formatted = g_strdup(text); break;
		case 1: formatted = g_strdup_printf(text, static_cast<Count>(numbers[0])); break;
		default:
			formatted = g_strdup_printf(text, static_cast<Count>(numbers[0]),
			                            static_cast<Count>(numbers[1]));
			break;
	}
	std::string result = formatted != nullptr ? formatted : "";
	g_free(formatted);
	return result;
}

std::set<std::string>& KeysSeen() {
	static std::set<std::string> seen;
	return seen;
}

// message is a sentence the update UI translates: it has a key made of updateUi.cpp's literals,
// UpdateMessageText shows the lookup of that key, and in the C locale that is message itself.
void CheckTranslated(const std::string& message, const std::string& name) {
	const auto key = kyty::gtk::UpdateMessageKeyFor(message);
	Check(key.has_value() && key->msgid != nullptr,
	      name + ": \"" + message + "\" has a key (a sentence of updateUi.cpp)");
	Check(UpdateUiLiterals().count(key->msgid) == 1,
	      name + ": the msgid \"" + key->msgid + "\" is a literal of updateUi.cpp");
	Check(key->plural_msgid == nullptr || UpdateUiLiterals().count(key->plural_msgid) == 1,
	      name + ": the plural msgid is a literal of updateUi.cpp");
	const std::string shown = kyty::gtk::UpdateMessageText(message);
	Check(shown == Lookup(*key),
	      name + ": UpdateMessageText shows the lookup of its key (\"" + shown + "\")");
	Check(shown == message, name + ": in the C locale the text is the updater's sentence (\"" +
	                            shown + "\" for \"" + message + "\")");
	KeysSeen().insert(key->msgid);
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

std::string AssetUrl() {
	return kyty::updater::AssetDownloadUrl(kLatestTag, kyty::updater::AssetNameForTag(kLatestTag));
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
		session                  = std::make_unique<UpdateSession>(
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
// install_offer), checked as a translated sentence.
void CheckSessionMessage(Session& session, bool install_offer, SessionState state,
                         const std::string& name) {
	const auto& last = session.Run(install_offer);
	Check(last.state == state && !last.message.empty(),
	      name + ": the session ends with a message (\"" + last.message + "\")");
	CheckTranslated(last.message, name);
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
		CheckSessionMessage(session, false, SessionState::Failed,
		                    "HTTP status " + std::to_string(status) + " for the check");
	}
	{
		Session session;
		session.Latest(Stalled(UpdaterGen::ReleaseDocument(Release(kLatestTag, kLaterDate)), 10));
		CheckSessionMessage(session, false, SessionState::Failed, "a stalled check");
		Check(session.outcomes.back().message.find(" 15 seconds.") != std::string::npos,
		      "the stalled check names 15 seconds");
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
	// The session cannot reach the unknown order: Compare() answers Unknown only for a release
	// without a tag or a publication time, which the check refuses before it compares (invalid,
	// unstable, or unverified). Its key is checked on the updater's constant.
	CheckTranslated(std::string(kyty::updater::kUnknownReleaseOrderMessage), "the unknown order");
	// The installed release cannot be verified: the reason is shown as a sentence of its own.
	{
		Session session;
		session.Latest(Ok(Release(kLatestTag, kLaterDate)));
		session.Installed(Status(404));
		const auto&       last = session.Run(false);
		const std::string prefix =
		    std::string(kyty::updater::kInstalledReleaseUnverifiedMessage) + " ";
		Check(last.state == SessionState::Failed && last.message.starts_with(prefix),
		      "an unverified installed release: \"" + last.message + "\"");
		const std::string reason = last.message.substr(prefix.size());
		CheckTranslated(reason, "the reason of an unverified installed release");
		Check(!kyty::gtk::UpdateMessageKeyFor(last.message),
		      "the unverified message with its reason has no key of its own");
		Check(UpdateUiLiterals().count("Could not verify the currently installed release. %s") == 1,
		      "updateUi.cpp translates the unverified frame");
		Check(kyty::gtk::UpdateMessageText(last.message) == last.message,
		      "the unverified message is shown as the updater says it, in the C locale");
	}
	// Anything else (a network reason) is shown as it is.
	{
		Session     session; // no scripted response: the fake answers with a network failure
		const auto& last = session.Run(false);
		Check(last.state == SessionState::Failed && !kyty::gtk::UpdateMessageKeyFor(last.message),
		      "a network failure's reason has no key: \"" + last.message + "\"");
		Check(kyty::gtk::UpdateMessageText(last.message) == last.message,
		      "a reason is shown as it is");
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
		CheckSessionMessage(session, true, SessionState::Failed, "a stalled download");
		Check(session.outcomes.back().message.find(" 30 seconds.") != std::string::npos,
		      "the stalled download names 30 seconds");
	}
	for (const auto& [failure, name]: std::vector<std::pair<HttpFailure, std::string>> {
	         {HttpFailure::TimedOut, "a timed-out download"},
	         {HttpFailure::Cancelled, "a cancelled download"}}) {
		Session session;
		session.ScriptNewer();
		session.http.failures[AssetUrl()] = failure;
		CheckSessionMessage(session, true, SessionState::Failed, name);
	}
	{
		Session session;
		session.ScriptNewer();
		session.Asset(Ok(Package().substr(0, 3000)));
		CheckSessionMessage(session, true, SessionState::Failed, "a short download");
		Check(session.outcomes.back().message.find("(3000 of 5000 bytes)") != std::string::npos,
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
}

void TestGameRunning() {
	{
		// Install while a game runs: refused, the offer stays.
		Session session;
		session.ScriptNewer();
		session.gate.can_install = false;
		CheckSessionMessage(session, true, SessionState::Offering, "Install while a game runs");
	}
	{
		// A game started while the release was prepared: cancelled before the handoff.
		Session session;
		session.ScriptNewer();
		session.Asset(Ok(Package()));
		session.runner.on_run = [&session](const fs::path& program) {
			if (program.filename() == "kyty_updater") {
				session.gate.can_install = false;
			}
		};
		CheckSessionMessage(session, true, SessionState::Failed, "a game started meanwhile");
	}
}

// Every fixed message and every sentence with numbers was reached once at least.
void TestCoverage() {
	const std::vector<std::string_view> expected = {
	    kyty::updater::kNoNewerReleaseMessage,
	    kyty::updater::kUnknownReleaseOrderMessage,
	    kyty::updater::kCloseGameMessage,
	    kyty::updater::kGameRunningCancelledMessage,
	    kyty::updater::kInvalidReleaseMessage,
	    kyty::updater::kUnstableReleaseMessage,
	    kyty::updater::kInvalidAssetMessage,
	    kyty::updater::kMissingAssetMessage,
	    kyty::updater::kDownloadTooLargeMessage,
	    kyty::updater::kDownloadDigestMessage,
	    kyty::updater::kDownloadAbortedMessage,
	    "The update response is larger than 2 MiB.",
	    "The update check timed out.",
	    "The update check was cancelled.",
	    "The download timed out.",
	    "The update check failed with HTTP status %llu.",
	    "The download failed with HTTP status %llu.",
	    "The update check received no data for %llu second.",
	    "The download received no data for %llu second.",
	    "The downloaded file is smaller than the release asset (%llu of %llu byte).",
	};
	for (const auto msgid: expected) {
		Check(KeysSeen().count(std::string(msgid)) == 1,
		      "a session message reached the key \"" + std::string(msgid) + "\"");
	}
	Check(KeysSeen().size() == expected.size(), "every key the session reached is expected (" +
	                                                std::to_string(KeysSeen().size()) + " keys)");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("UpdateMessageTextTests");
	Check(!UpdateUiLiterals().empty(), "updateUi.cpp has translatable literals");
	TestCheckFailures();
	TestCheckAnswers();
	TestDownloadFailures();
	TestGameRunning();
	TestCoverage();
	std::printf(
	    "UpdateMessageTextTests: all tests passed (%zu keys, %zu literals of updateUi.cpp)\n",
	    KeysSeen().size(), UpdateUiLiterals().size());
	return 0;
}
