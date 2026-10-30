// Feature: linux-gtk-modernization, Property 40: Update session requests and decisions
//
// Validates Requirements 13.1, 13.5, 13.12, and 13.13. Each case draws the installed release tag,
// a latest-release response (a network failure, a stall over 15 s, more than 2 MiB, malformed
// JSON, a draft, a prerelease, a release without the package or with an invalid one, or a stable
// release with its package), the latest release's publication time before, at, or after the
// installed release's (the same instant also in another UTC offset), a latest tag equal to the
// installed one in about one case in five, and, for a different tag, the installed-release
// response (a network failure, HTTP 404, a stall, more than 2 MiB, malformed JSON, another
// release, or the installed release); responses arrive in random chunks within the stall limit.
// UpdateSession::Check runs on FakeHttpClient and FakeScheduler; when it offers a release,
// Install() runs with a gate that allows it and a download that succeeds or fails. Every request
// URL must start with https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/ or
// https://github.com/MrG00gle/KytyPS5-GTK/releases/download/ and carry its kind's limits; the
// package is requested exactly when the latest release is valid, has the
// <tag>-Linux-x86_64.tar.gz package, and was published after the installed release; otherwise the
// session ends with the error that the model names or with "No newer release is available.".

#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/property.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/updatePolicy.h"
#include "updater/core/updateSession.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::updater::SessionOutcome;
using kyty::updater::SessionState;
using KytyTests::FakeResponse;
using std::chrono::milliseconds;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
T Pick(const std::vector<T>& values) {
	return values[Choose<size_t>(0, values.size())];
}

enum class Latest {
	NetworkFailure,
	Stalled,
	TooLarge,
	Malformed,
	Draft,
	Prerelease,
	NoPackage,
	InvalidPackage,
	Stable
};
enum class Installed { NetworkFailure, Missing, Stalled, TooLarge, Malformed, Other, Stable };
enum class When { Before, At, After };

const char* LatestName(Latest kind) {
	switch (kind) {
		case Latest::NetworkFailure: return "latest: network failure";
		case Latest::Stalled: return "latest: stalled";
		case Latest::TooLarge: return "latest: too large";
		case Latest::Malformed: return "latest: malformed";
		case Latest::Draft: return "latest: draft";
		case Latest::Prerelease: return "latest: prerelease";
		case Latest::NoPackage: return "latest: no package";
		case Latest::InvalidPackage: return "latest: invalid package";
		case Latest::Stable: return "latest: stable";
	}
	return "?";
}

const char* InstalledName(Installed kind) {
	switch (kind) {
		case Installed::NetworkFailure: return "installed: network failure";
		case Installed::Missing: return "installed: missing";
		case Installed::Stalled: return "installed: stalled";
		case Installed::TooLarge: return "installed: too large";
		case Installed::Malformed: return "installed: malformed";
		case Installed::Other: return "installed: another release";
		case Installed::Stable: return "installed: stable";
	}
	return "?";
}

constexpr std::string_view kInstalledDate = "2026-03-01T10:00:00Z";

std::string DrawDate(When when) {
	switch (when) {
		case When::Before:
			return Pick<std::string>(
			    {"2026-03-01T09:59:59Z", "2025-12-31T23:00:00Z", "2026-03-01T11:59:59+02:00"});
		case When::At:
			return Pick<std::string>({"2026-03-01T10:00:00Z", "2026-03-01T07:00:00-03:00"});
		case When::After:
			return Pick<std::string>(
			    {"2026-03-01T10:00:01Z", "2026-04-15T08:30:00Z", "2026-03-01T10:30:00+00:20"});
	}
	return {};
}

// A body delivered in random chunks, each within the stall limit of its request.
FakeResponse Delivered(std::string body, milliseconds limit) {
	FakeResponse response;
	response.body    = std::move(body);
	response.latency = milliseconds {Choose<int64_t>(0, limit.count())};
	if (!response.body.empty() && OneIn(2)) {
		const int chunks = Choose(1, 5);
		for (int i = 0; i < chunks; i++) {
			response.chunk_sizes.push_back(Choose<size_t>(0, response.body.size() / 2 + 2));
		}
		response.chunk_interval = milliseconds {Choose<int64_t>(0, limit.count())};
	}
	return response;
}

FakeResponse Failure(int status = 200) {
	FakeResponse response;
	response.status          = status;
	response.network_failure = status == 200;
	return response;
}

// A response that sends part of body and then nothing more.
FakeResponse Stalled(std::string body) {
	FakeResponse response;
	response.stall_after_bytes = Choose<size_t>(0, body.size());
	response.body              = std::move(body);
	return response;
}

// More than 2 MiB: a release document padded in its "body" member.
std::string Oversized(UpdaterGen::ReleaseSpec spec) {
	const size_t size  = kyty::updater::kMaxReleaseMetadataBytes + Choose<size_t>(1, 2000);
	spec.other_members = {{"body", UpdaterGen::JsonString(std::string(size, 'x'))}};
	return UpdaterGen::ReleaseDocument(spec);
}

class FakeRunner final: public kyty::updater::ProcessRunner {
public:
	kyty::updater::ProcessResult Run(const fs::path&, std::vector<std::string>,
	                                 const fs::path&) override {
		runs++;
		return {0, "", ""};
	}
	kyty::core::Expected<void, std::string> StartDetached(const fs::path&, std::vector<std::string>,
	                                                      const fs::path&,
	                                                      const fs::path&) override {
		detached++;
		return {};
	}

	int runs     = 0;
	int detached = 0;
};

class OpenGate final: public kyty::updater::InstallGate {
public:
	bool CanInstallUpdate() const override { return true; }
	void InstallStarted() override { started++; }
	void InstallFinished() override { finished++; }

	int started  = 0;
	int finished = 0;
};

bool HasWorkspace(const fs::path& install) {
	std::error_code ec;
	for (auto it = fs::directory_iterator(install, ec); !ec && it != fs::directory_iterator();
	     it.increment(ec)) {
		if (it->path().filename().string().starts_with(".kyty-update-")) {
			return true;
		}
	}
	return false;
}

} // namespace

KYTY_PROPERTY(40, "Update session requests and decisions") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p40");
		const auto               install = temp.MakeDirectories("install");
		RC_ASSERT(UpdaterGen::WriteExecutable(install / "kyty_updater", "#!/bin/sh\nexit 0\n"));

		// The releases.
		const std::string installed_tag = "2026.03.01-" + std::to_string(Choose(1000, 9999));
		const bool        same_tag      = OneIn(5);
		const std::string latest_tag =
		    same_tag ? installed_tag
		             : Pick<std::string>({"2026.04.01-abcdef0", "v2", "release 7",
		                                  "\xC3\xA9t\xC3\xA9", "2026.03.01-0000"});
		// Later in half the cases, and a stable latest release in half the cases, so that about
		// one case in eight downloads.
		const auto        when    = OneIn(2) ? When::After : Pick<When>({When::Before, When::At});
		const std::string package = UpdaterGen::PatternBytes(Choose<size_t>(1, 3000), 3);
		auto latest_spec = UpdaterGen::ValidRelease(latest_tag, DrawDate(when),
		                                            UpdaterGen::DigestOf(package), package.size());
		const auto latest_kind =
		    OneIn(2) ? Latest::Stable
		             : Pick<Latest>({Latest::NetworkFailure, Latest::Stalled, Latest::TooLarge,
		                             Latest::Malformed, Latest::Draft, Latest::Prerelease,
		                             Latest::NoPackage, Latest::InvalidPackage});
		switch (latest_kind) {
			case Latest::Draft: latest_spec.draft = "true"; break;
			case Latest::Prerelease: latest_spec.prerelease = "true"; break;
			case Latest::NoPackage: latest_spec.assets.pop_back(); break;
			case Latest::InvalidPackage:
				if (OneIn(2)) {
					latest_spec.assets.back().digest = UpdaterGen::JsonString("sha256:00");
				} else {
					latest_spec.assets.back().size = Pick<std::string>({"0", "-1", "1.5"});
				}
				break;
			default: break;
		}
		const auto installed_spec =
		    UpdaterGen::ValidRelease(installed_tag, kInstalledDate, UpdaterGen::DigestOf("old"), 3);
		const auto installed_kind =
		    Choose(0, 3) < 2 ? Installed::Stable
		                     : Pick<Installed>({Installed::NetworkFailure, Installed::Missing,
		                                        Installed::Stalled, Installed::TooLarge,
		                                        Installed::Malformed, Installed::Other});

		// The scripted responses.
		KytyTests::FakeScheduler  scheduler;
		KytyTests::FakeHttpClient http(scheduler);
		const auto                latest_document = UpdaterGen::ReleaseDocument(latest_spec);
		const auto                metadata_limit  = milliseconds {14999};
		switch (latest_kind) {
			case Latest::NetworkFailure:
				http.AddResponse(kyty::updater::LatestReleaseUrl(), Failure());
				break;
			case Latest::Stalled:
				http.AddResponse(kyty::updater::LatestReleaseUrl(), Stalled(latest_document));
				break;
			case Latest::TooLarge:
				http.AddResponse(kyty::updater::LatestReleaseUrl(),
				                 Delivered(Oversized(latest_spec), metadata_limit));
				break;
			case Latest::Malformed:
				http.AddResponse(
				    kyty::updater::LatestReleaseUrl(),
				    Delivered(latest_document.substr(0, Choose<size_t>(0, latest_document.size())),
				              metadata_limit));
				break;
			default:
				http.AddResponse(kyty::updater::LatestReleaseUrl(),
				                 Delivered(latest_document, metadata_limit));
				break;
		}
		const auto installed_url      = kyty::updater::ReleaseByTagUrl(installed_tag);
		const auto installed_document = UpdaterGen::ReleaseDocument(installed_spec);
		switch (installed_kind) {
			case Installed::NetworkFailure: http.AddResponse(installed_url, Failure()); break;
			case Installed::Missing: http.AddResponse(installed_url, Failure(404)); break;
			case Installed::Stalled:
				http.AddResponse(installed_url, Stalled(installed_document));
				break;
			case Installed::TooLarge:
				http.AddResponse(installed_url,
				                 Delivered(Oversized(installed_spec), metadata_limit));
				break;
			case Installed::Malformed:
				http.AddResponse(installed_url, Delivered("{\"tag_name\":", metadata_limit));
				break;
			case Installed::Other: {
				auto other     = installed_spec;
				other.tag_name = UpdaterGen::JsonString("someone-else");
				http.AddResponse(installed_url,
				                 Delivered(UpdaterGen::ReleaseDocument(other), metadata_limit));
				break;
			}
			case Installed::Stable:
				http.AddResponse(installed_url, Delivered(installed_document, metadata_limit));
				break;
		}
		const auto asset_url =
		    kyty::updater::AssetDownloadUrl(latest_tag, kyty::updater::AssetNameForTag(latest_tag));
		const bool download_succeeds = OneIn(2);
		if (download_succeeds) {
			http.AddResponse(asset_url, Delivered(package, milliseconds {29999}));
		}

		// The model.
		std::optional<std::string> failure; // the expected message, or a prefix (installed)
		bool                       prefix   = false;
		bool                       download = false;
		const std::string          stall    = "The update check received no data for 15 seconds.";
		const std::string          large    = "The update response is larger than 2 MiB.";
		switch (latest_kind) {
			case Latest::NetworkFailure:
				failure = "The update check failed: simulated network failure";
				break;
			case Latest::Stalled: failure = stall; break;
			case Latest::TooLarge: failure = large; break;
			case Latest::Malformed:
				failure = std::string(kyty::updater::kInvalidReleaseMessage);
				break;
			case Latest::Draft:
			case Latest::Prerelease:
				failure = std::string(kyty::updater::kUnstableReleaseMessage);
				break;
			default: break;
		}
		if (!failure && !same_tag) {
			if (installed_kind != Installed::Stable) {
				failure = std::string(kyty::updater::kInstalledReleaseUnverifiedMessage) + " ";
				prefix  = true;
			} else if (when == When::After) {
				if (latest_kind == Latest::NoPackage) {
					failure = std::string(kyty::updater::kMissingAssetMessage);
				} else if (latest_kind == Latest::InvalidPackage) {
					failure = std::string(kyty::updater::kInvalidAssetMessage);
				} else {
					download = true;
				}
			}
		}

		// The session.
		FakeRunner                         runner;
		OpenGate                           gate;
		std::vector<SessionOutcome>        outcomes;
		kyty::updater::UpdateSessionConfig config;
		config.installed_tag      = installed_tag;
		config.install_dir        = install;
		config.launcher_relative  = "kyty_launcher";
		config.helper             = install / "kyty_updater";
		config.launcher_pid       = 4242;
		config.working_dir        = temp.Path();
		const bool user_activated = OneIn(2);
		{
			kyty::updater::UpdateSession session(
			    config, http, scheduler, runner, gate,
			    [&outcomes](const SessionOutcome& outcome) { outcomes.push_back(outcome); });
			session.Check(user_activated);
			scheduler.RunUntilIdle();
			RC_ASSERT(!outcomes.empty());
			RC_ASSERT(outcomes.back().user_activated == user_activated);
			if (session.State() == SessionState::Offering) {
				RC_ASSERT(outcomes.back().release.has_value());
				RC_ASSERT(outcomes.back().release->tag == latest_tag);
				session.Install();
				scheduler.RunUntilIdle();
			}
			RC_LOG() << LatestName(latest_kind) << ", " << InstalledName(installed_kind)
			         << ", same tag " << same_tag << ", when " << static_cast<int>(when)
			         << ", final state " << static_cast<int>(session.State()) << ": "
			         << outcomes.back().message << "\n";

			// Every request goes to the fork's releases, with its limits.
			bool asset_requested = false;
			for (const auto& request: http.Requests()) {
				const bool metadata = request.url.starts_with(kyty::updater::kReleaseApiPrefix);
				RC_ASSERT(metadata ||
				          request.url.starts_with(kyty::updater::kReleaseDownloadPrefix));
				RC_ASSERT(kyty::updater::IsAllowedUpdateUrl(request.url));
				if (metadata) {
					RC_ASSERT(request.max_bytes == kyty::updater::kMaxReleaseMetadataBytes);
					RC_ASSERT(request.stall_timeout == milliseconds {15000});
				} else {
					RC_ASSERT(request.url == asset_url);
					RC_ASSERT(request.max_bytes == package.size());
					RC_ASSERT(request.stall_timeout == milliseconds {30000});
					asset_requested = true;
				}
			}
			RC_ASSERT(asset_requested == download);

			const auto& last = outcomes.back();
			if (download) {
				if (download_succeeds) {
					RC_ASSERT(session.State() == SessionState::Handoff);
					RC_ASSERT(runner.runs == 2 && runner.detached == 1);
				} else {
					RC_ASSERT(session.State() == SessionState::Failed);
					RC_ASSERT(last.message.starts_with("The download failed: "));
					RC_ASSERT(!HasWorkspace(install));
				}
				RC_ASSERT(gate.started == 1);
			} else if (failure) {
				RC_ASSERT(session.State() == SessionState::Failed);
				if (prefix) {
					RC_ASSERT(last.message.starts_with(*failure));
					RC_ASSERT(last.message.size() > failure->size());
				} else {
					RC_ASSERT(last.message == *failure);
				}
			} else {
				RC_ASSERT(session.State() == SessionState::NoNewer);
				RC_ASSERT(last.message == kyty::updater::kNoNewerReleaseMessage);
			}
			if (!download) {
				RC_ASSERT(gate.started == 0 && runner.runs == 0 && !HasWorkspace(install));
			}
		}
		RC_ASSERT(gate.finished == (download && !download_succeeds ? 1 : 0));
		RC_TAG(download  ? (download_succeeds ? "download, handoff" : "download, failed")
		       : failure ? "error"
		                 : "no newer release");
	});
}
