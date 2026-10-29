// Example tests of the title sound state machine (src/launcher/core/titleSoundController.h,
// task 14.2; Requirements 8.7-8.10): a selection starts a decode and its result plays once; a
// newer selection, no selection, or a run start makes every earlier result stale; selecting the
// pending or playing entry again changes nothing; nothing plays while a game runs and leaving the
// game does not replay; failures warn with the entry and the reason, except an absent sound.

#include "launcher/core/expected.h"
#include "launcher/core/titleSound.h"
#include "launcher/core/titleSoundController.h"
#include "support/noDisplay.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace {

using kyty::core::EntryId;
using kyty::core::Expected;
using kyty::core::MakeUnexpected;
using kyty::core::Pcm;
using kyty::core::TitleSoundAction;
using kyty::core::TitleSoundCommand;
using kyty::core::TitleSoundController;
using kyty::core::TitleSoundError;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "TitleSoundControllerTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

const EntryId kGameA = "/games/Game A";
const EntryId kGameB = "/games/archives/Game B.zar";

Pcm MakePcm(float marker) {
	Pcm pcm;
	pcm.samples  = {marker, -marker, marker, -marker};
	pcm.channels = 2;
	pcm.rate     = 48000;
	return pcm;
}

Expected<Pcm, TitleSoundError> Ok(float marker) {
	return MakePcm(marker);
}

Expected<Pcm, TitleSoundError> Fail(TitleSoundError error) {
	return MakeUnexpected(error);
}

void CheckNone(const TitleSoundCommand& command, const std::string& what) {
	Check(command.action == TitleSoundAction::None && command.warning.empty(), what + ": None");
}

void CheckStartDecode(const TitleSoundCommand& command, uint64_t generation, const EntryId& entry,
                      const std::string& what) {
	Check(command.action == TitleSoundAction::StartDecode, what + ": StartDecode");
	Check(command.generation == generation, what + ": generation " +
	                                            std::to_string(command.generation) + ", expected " +
	                                            std::to_string(generation));
	Check(command.entry == entry, what + ": entry " + command.entry.string());
	Check(command.warning.empty(), what + ": no warning");
}

void CheckPlay(const TitleSoundCommand& command, uint64_t generation, const EntryId& entry,
               float marker, const std::string& what) {
	Check(command.action == TitleSoundAction::Play, what + ": Play");
	Check(command.generation == generation, what + ": generation");
	Check(command.entry == entry, what + ": entry");
	Check(command.pcm == MakePcm(marker), what + ": the decoded PCM");
	Check(command.warning.empty(), what + ": no warning");
}

void CheckStopAndCancel(const TitleSoundCommand& command, const std::string& what) {
	Check(command.action == TitleSoundAction::StopAndCancel && command.warning.empty(),
	      what + ": StopAndCancel");
}

void TestSelectAndPlay() {
	TitleSoundController controller;
	Check(controller.Generation() == 0 && !controller.GameRunning() && !controller.SoundEntry(),
	      "initial state");
	CheckNone(controller.Select(std::nullopt), "no selection at first");
	CheckStartDecode(controller.Select(kGameA), 1, kGameA, "select A");
	Check(controller.SoundEntry() == kGameA, "A's sound is pending");
	CheckPlay(controller.DecodeFinished(1, Ok(0.5f)), 1, kGameA, 0.5f, "A decoded");
	CheckNone(controller.DecodeFinished(1, Ok(0.5f)), "a second result for the same request");
	CheckNone(controller.Select(kGameA), "A again while it plays");
	CheckStartDecode(controller.Select(kGameB), 2, kGameB, "select B");
	CheckPlay(controller.DecodeFinished(2, Ok(0.25f)), 2, kGameB, 0.25f, "B decoded");
	CheckStopAndCancel(controller.Select(std::nullopt), "clear the selection");
	Check(!controller.SoundEntry() && controller.Generation() == 3, "nothing pending or playing");
	CheckNone(controller.Select(std::nullopt), "clear it again");
	CheckStartDecode(controller.Select(kGameB), 4, kGameB, "select B again after clearing");
}

void TestStaleResults() {
	TitleSoundController controller;
	CheckStartDecode(controller.Select(kGameA), 1, kGameA, "select A");
	CheckStartDecode(controller.Select(kGameB), 2, kGameB, "select B before A decoded");
	CheckNone(controller.DecodeFinished(1, Ok(0.5f)), "A's late result is discarded");
	CheckNone(controller.DecodeFinished(1, Fail(TitleSoundError::Decode)),
	          "A's late failure is discarded without a warning");
	CheckStartDecode(controller.Select(kGameA), 3, kGameA, "back to A");
	CheckNone(controller.DecodeFinished(2, Ok(0.25f)), "B's late result is discarded");
	CheckNone(controller.Select(kGameA), "A again while its decode is pending");
	CheckPlay(controller.DecodeFinished(3, Ok(0.75f)), 3, kGameA, 0.75f, "A's new result plays");
	CheckNone(controller.DecodeFinished(0, Ok(0.75f)), "generation 0 is never a request");
	CheckNone(controller.DecodeFinished(4, Ok(0.75f)), "a future generation is not a request");

	CheckStopAndCancel(controller.Select(std::nullopt), "clear while nothing is pending");
	CheckNone(controller.DecodeFinished(3, Ok(0.75f)), "a result after clearing");
}

void TestRun() {
	TitleSoundController controller;
	CheckStartDecode(controller.Select(kGameA), 1, kGameA, "select A");
	CheckStopAndCancel(controller.RunStarted(), "the game starts while A decodes");
	Check(controller.GameRunning() && !controller.SoundEntry(), "running, no sound");
	CheckNone(controller.DecodeFinished(1, Ok(0.5f)), "A's result after the run started");
	CheckNone(controller.Select(kGameB), "select B while the game runs");
	CheckNone(controller.Select(std::nullopt), "clear while the game runs");
	CheckNone(controller.Select(kGameA), "select A while the game runs");
	const auto generation = controller.Generation();
	CheckNone(controller.GameExited(), "the game exits: no replay");
	Check(!controller.GameRunning() && !controller.SoundEntry() &&
	          controller.Generation() == generation,
	      "idle without a sound");
	CheckStartDecode(controller.Select(kGameB), generation + 1, kGameB, "select B after the run");
	CheckPlay(controller.DecodeFinished(generation + 1, Ok(0.25f)), generation + 1, kGameB, 0.25f,
	          "B plays after the run");

	// A run that starts while a sound plays stops it.
	CheckStopAndCancel(controller.RunStarted(), "the game starts while B plays");
	CheckStopAndCancel(controller.RunStarted(), "a second run start still stops");
	CheckNone(controller.GameExited(), "exit");
	CheckNone(controller.GameExited(), "a second exit");
	const auto next = controller.Generation() + 1;
	CheckStartDecode(controller.Select(kGameB), next, kGameB,
	                 "the same entry selected again after the run plays again");
}

void TestFailures() {
	struct Case {
		TitleSoundError error;
		bool            warns;
	};
	for (const auto& [error, warns]:
	     {Case {TitleSoundError::Absent, false}, Case {TitleSoundError::Cancelled, false},
	      Case {TitleSoundError::Unreadable, true}, Case {TitleSoundError::TooLarge, true},
	      Case {TitleSoundError::InvalidHeader, true}, Case {TitleSoundError::Decode, true}}) {
		const std::string    what(kyty::core::DescribeTitleSoundError(error));
		TitleSoundController controller;
		CheckStartDecode(controller.Select(kGameB), 1, kGameB, what + ": select B");
		const auto command = controller.DecodeFinished(1, Fail(error));
		Check(command.action == TitleSoundAction::None, what + ": nothing to play");
		if (warns) {
			Check(command.warning.find(kGameB.string()) != std::string::npos,
			      what + ": the warning names the entry: " + command.warning);
			Check(command.warning.find(what) != std::string::npos,
			      what + ": the warning gives the reason: " + command.warning);
		} else {
			Check(command.warning.empty(), what + ": no warning");
		}
		CheckNone(controller.DecodeFinished(1, Fail(error)), what + ": reported once");
		CheckNone(controller.Select(kGameB), what + ": B stays selected without a new decode");
		CheckStartDecode(controller.Select(kGameA), 2, kGameA, what + ": the next selection works");
	}
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("TitleSoundControllerTests");
	TestSelectAndPlay();
	TestStaleResults();
	TestRun();
	TestFailures();
	std::printf("TitleSoundControllerTests: all passed\n");
	return 0;
}
