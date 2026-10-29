#pragma once

// What the GTK TitleSoundPlayer loads, plays, stops, and discards (Requirements 8.7-8.10). Pure
// state, driven by the main thread: the player reports selections, decode results, and run state
// changes, and carries out the returned command. Every decode request carries a generation
// number; a result whose generation is not the latest request's is stale and discarded, so at
// most one sound plays and it belongs to the latest selection made while no game runs. Ported
// from the Qt launcher's TitleSound::SetTitle (src/launcher/src/titleSound.cpp) and its callers
// in mainDialog.cpp.

#include "launcher/core/expected.h"
#include "launcher/core/titleSound.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace kyty::core {

// A Game_Entry's identity: its canonical path (GameEntry::path).
using EntryId = std::filesystem::path;

enum class TitleSoundAction {
	None,          // nothing to do
	StartDecode,   // stop playback, cancel any pending decode, then read and decode the entry's
	               // snd0.at9 and report DecodeFinished(generation, result)
	Play,          // play pcm once on the default audio output device
	StopAndCancel, // stop playback and cancel any pending decode
};

struct TitleSoundCommand {
	TitleSoundAction action     = TitleSoundAction::None;
	uint64_t         generation = 0; // StartDecode, Play: the request's generation
	EntryId          entry;          // StartDecode, Play: the entry whose sound it is
	Pcm              pcm;            // Play
	// A warning to log, naming the entry and the reason (Requirement 8.9); empty for none. Only a
	// current request's failure warns, and never for an absent or cancelled sound.
	std::string warning;
};

class TitleSoundController {
public:
	// The selection changed to entry, or to no selection. While no game runs and the entry's sound
	// is not already the one pending or playing: StartDecode with a new generation, or
	// StopAndCancel for no selection. While a game runs: None (Requirement 8.10; leaving the game
	// does not replay the sound). Selecting the entry whose sound is pending or playing again is
	// None, as in the Qt launcher, so a list refresh does not restart it.
	TitleSoundCommand Select(std::optional<EntryId> entry);

	// A decode requested with generation finished. Play for the latest request's sound while no
	// game runs; None for a stale result (an earlier selection, or a request made before a run
	// started) and for a failure, which carries a warning unless the sound is absent or the decode
	// was cancelled.
	TitleSoundCommand DecodeFinished(uint64_t generation, Expected<Pcm, TitleSoundError> result);

	// The Emulator starts: StopAndCancel; every outstanding request becomes stale.
	TitleSoundCommand RunStarted();

	// The Emulator exited: None (no replay; the next selection change plays again).
	TitleSoundCommand GameExited();

	// The current generation (0 at first); it grows with every StartDecode and StopAndCancel, and
	// only a pending request of the current generation can play.
	[[nodiscard]] uint64_t Generation() const { return m_generation; }
	[[nodiscard]] bool     GameRunning() const { return m_running; }
	// The entry whose sound is pending or playing, if any.
	[[nodiscard]] const std::optional<EntryId>& SoundEntry() const { return m_sound_entry; }

private:
	uint64_t               m_generation = 0;
	bool                   m_running    = false;
	bool                   m_pending    = false; // the latest request has not reported yet
	std::optional<EntryId> m_sound_entry;
};

} // namespace kyty::core
