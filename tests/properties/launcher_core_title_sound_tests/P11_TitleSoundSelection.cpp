// Feature: linux-gtk-modernization, Property 11: Title sound follows the latest selection
//
// Validates Requirements 8.7, 8.8, and 8.10. Sequences of 0-60 events from titleSoundGenerators.h
// (selections of three entries, cleared selections, decode results for the latest, an earlier, or
// a never-issued request, run starts, game exits) drive a TitleSoundController and a simulated
// player that carries out its commands. After every event: at most one sound plays (a Play never
// arrives while one plays); a playing sound belongs to the most recent selection made while no
// game was running, with no run started since; nothing plays while a game runs. A selection while
// idle that changes the wanted sound starts a decode with a new generation (or stops and cancels
// for no selection); a run start stops and cancels; a game exit replays nothing. Every result of
// an earlier selection or of a request made before a run started is discarded, and only the
// latest request's first successful result plays, with its own PCM.

#include "launcher/core/expected.h"
#include "launcher/core/titleSound.h"
#include "launcher/core/titleSoundController.h"
#include "support/property.h"
#include "support/titleSoundGenerators.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

namespace TitleSoundGen = KytyTests::TitleSoundGen;

using kyty::core::EntryId;
using kyty::core::Pcm;
using kyty::core::TitleSoundAction;
using kyty::core::TitleSoundError;
using TitleSoundGen::EventKind;

struct Request {
	uint64_t generation = 0;
	EntryId  entry;
};

struct Playing {
	uint64_t generation = 0;
	EntryId  entry;
	Pcm      pcm;
};

// A PCM buffer that identifies the event that delivered it.
Pcm MarkedPcm(size_t event_index, uint64_t generation) {
	Pcm pcm;
	pcm.channels = 1;
	pcm.rate     = 48000;
	pcm.samples  = {static_cast<float>(event_index), static_cast<float>(generation)};
	return pcm;
}

} // namespace

KYTY_PROPERTY(11, "Title sound follows the latest selection") {
	return rc::check(name, []() {
		const auto events = TitleSoundGen::GenerateEvents();

		kyty::core::TitleSoundController controller;
		// The reference state.
		bool                   running = false;
		std::optional<EntryId> wanted; // the sound the current idle selection wants
		std::optional<EntryId> last_idle_selection;
		bool                   run_since_idle_selection = false;
		std::vector<Request>   requests; // every StartDecode, in order
		std::optional<size_t>  latest;   // index of the request that may still play
		std::set<uint64_t>     reported; // generations that delivered a result
		std::optional<Playing> playing;  // the simulated player
		uint64_t               last_generation = 0;
		bool                   played = false, discarded = false;

		for (size_t index = 0; index < events.size(); index++) {
			const auto& event = events[index];
			RC_LOG() << index << ": " << TitleSoundGen::EventName(event.kind);
			kyty::core::TitleSoundCommand command;
			switch (event.kind) {
				case EventKind::Select:
				case EventKind::Clear: {
					std::optional<EntryId> selection;
					if (event.kind == EventKind::Select) {
						selection = TitleSoundGen::Entries()[event.entry];
						RC_LOG() << " " << selection->string();
					}
					RC_LOG() << "\n";
					command = controller.Select(selection);
					if (!running) {
						last_idle_selection      = selection;
						run_since_idle_selection = false;
					}
					const auto now_wanted = running ? std::nullopt : selection;
					if (now_wanted == wanted) {
						RC_ASSERT(command.action == TitleSoundAction::None);
						break;
					}
					wanted = now_wanted;
					latest.reset();
					playing.reset(); // StartDecode and StopAndCancel both stop playback
					if (wanted) {
						RC_ASSERT(command.action == TitleSoundAction::StartDecode);
						RC_ASSERT(command.entry == *wanted);
						RC_ASSERT(command.generation > last_generation);
						last_generation = command.generation;
						requests.push_back({command.generation, command.entry});
						latest = requests.size() - 1;
					} else {
						RC_ASSERT(command.action == TitleSoundAction::StopAndCancel);
					}
					break;
				}
				case EventKind::DecodeFinished: {
					// Map the pick onto a request: 0-5 the latest, the one before, ...; 6 and 7
					// generations that were never requested.
					uint64_t              generation = 0;
					std::optional<size_t> request;
					if (event.pick < 6 && !requests.empty()) {
						const size_t back = static_cast<size_t>(event.pick) % requests.size();
						request           = requests.size() - 1 - back;
						generation        = requests[*request].generation;
					} else {
						generation = event.pick == 6 ? 0 : controller.Generation() + 1;
					}
					RC_LOG() << " generation " << generation
					         << (event.success
					                 ? " ok"
					                 : std::string(" ") +
					                       std::string(
					                           kyty::core::DescribeTitleSoundError(event.error)))
					         << "\n";
					kyty::core::Expected<Pcm, TitleSoundError> result =
					    event.success ? kyty::core::Expected<Pcm, TitleSoundError>(
					                        MarkedPcm(index, generation))
					                  : kyty::core::MakeUnexpected(event.error);
					command = controller.DecodeFinished(generation, result);

					const bool current =
					    request && latest && *request == *latest && reported.count(generation) == 0;
					if (request) {
						reported.insert(generation);
					}
					if (!current) {
						RC_ASSERT(command.action == TitleSoundAction::None);
						RC_ASSERT(command.warning.empty());
						discarded = true;
						break;
					}
					if (event.success) {
						RC_ASSERT(command.action == TitleSoundAction::Play);
						// Never two sounds at once.
						RC_ASSERT(!playing.has_value());
						RC_ASSERT(command.generation == generation);
						RC_ASSERT(command.entry == requests[*request].entry);
						RC_ASSERT(command.pcm == MarkedPcm(index, generation));
						playing = Playing {generation, command.entry, command.pcm};
						played  = true;
					} else {
						RC_ASSERT(command.action == TitleSoundAction::None);
						const bool warns = event.error != TitleSoundError::Absent &&
						                   event.error != TitleSoundError::Cancelled;
						RC_ASSERT(command.warning.empty() == !warns);
						if (warns) {
							RC_ASSERT(command.warning.find(requests[*request].entry.string()) !=
							          std::string::npos);
						}
					}
					break;
				}
				case EventKind::RunStarted:
					RC_LOG() << "\n";
					command = controller.RunStarted();
					RC_ASSERT(command.action == TitleSoundAction::StopAndCancel);
					running                  = true;
					run_since_idle_selection = true;
					wanted.reset();
					latest.reset();
					playing.reset();
					break;
				case EventKind::GameExited:
					RC_LOG() << "\n";
					command = controller.GameExited();
					RC_ASSERT(command.action == TitleSoundAction::None);
					running = false;
					break;
			}

			// Invariants after every event.
			RC_ASSERT(controller.GameRunning() == running);
			RC_ASSERT(controller.SoundEntry() == wanted);
			RC_ASSERT(controller.Generation() >= last_generation);
			if (playing) {
				RC_ASSERT(!running);
				RC_ASSERT(!run_since_idle_selection);
				RC_ASSERT(last_idle_selection.has_value());
				RC_ASSERT(playing->entry == *last_idle_selection);
				RC_ASSERT(latest.has_value());
				RC_ASSERT(playing->generation == requests[*latest].generation);
			}
		}

		RC_TAG(played && discarded ? "played and discarded"
		       : played            ? "played"
		       : discarded         ? "discarded only"
		                           : "nothing decoded");
	});
}
