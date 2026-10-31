#pragma once

// Plays the selected game's Title_Sound (task 19.5; plan D24, HC-14; Requirements 8.7-8.10): it
// carries out the commands of core::TitleSoundController, which decides what loads, plays, stops,
// and is discarded.
// - StartDecode: playback stops, the previous decode's CancelToken is cancelled, and a GTask thread
//   reads sce_sys/snd0.at9 (core::ReadSnd0; Absent when core::EntryFileExists is false, TooLarge
//   for a file of a directory entry over 32 MiB, Unreadable for any other read failure) and decodes
//   it (core::DecodeTitleSound with the token); the result goes to DecodeFinished on the main
//   thread, so a stale result is discarded by its generation.
// - Play: the PCM goes once to an SDL audio stream on the default playback device
//   (SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK), opened on the first sound and
//   kept; SDL_SetAudioStreamFormat for each sound's channels and rate).
// - StopAndCancel (another selection, no selection, a run starts): SDL_ClearAudioStream and
//   SDL_PauseAudioStreamDevice at once, so playback stops within 100 ms, and the pending decode is
//   cancelled.
// - The controller's warnings (a current sound that cannot be used, 8.9) and SDL failures (no audio
//   device) are logged with g_warning, naming the entry and the reason; an absent sound logs
//   nothing.
// SDL never installs its signal handlers (SDL_HINT_NO_SIGNAL_HANDLERS). The audio subsystem starts
// with the first sound. Destroying the player stops playback and cancels the pending decode; no
// result is applied afterwards.

#include "launcher/core/cancelToken.h"
#include "launcher/core/entryId.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/titleSoundController.h"

#include <cstdint>
#include <gio/gio.h>
#include <memory>
#include <optional>

struct SDL_AudioStream;

namespace kyty::gtk {

class TitleSoundPlayer {
public:
	TitleSoundPlayer();
	~TitleSoundPlayer();

	TitleSoundPlayer(const TitleSoundPlayer&)            = delete;
	TitleSoundPlayer& operator=(const TitleSoundPlayer&) = delete;

	// The selection changed to entry, or to nothing (nullptr).
	void Select(const core::GameEntry* entry);
	// The Emulator starts (8.10): stop now and discard every pending decode.
	void RunStarted();
	// The Emulator exited: nothing plays until the next selection change.
	void GameExited();

	// The entry whose sound is playing: put to the device, not stopped, and not played to the end.
	[[nodiscard]] std::optional<core::EntryId> PlayingEntry() const;
	[[nodiscard]] bool Playing() const { return PlayingEntry().has_value(); }

private:
	struct Decode;

	static void RunDecode(GTask* task, gpointer source, gpointer task_data,
	                      GCancellable* cancellable);
	static void OnDecoded(GObject* source, GAsyncResult* result, gpointer data);

	void Execute(core::TitleSoundCommand command, core::GameEntry::Kind kind);
	void StartDecode(uint64_t generation, const core::EntryId& entry, core::GameEntry::Kind kind);
	void Play(const core::TitleSoundCommand& command);
	void StopAndCancel();

	core::TitleSoundController         m_controller;
	std::shared_ptr<core::CancelToken> m_cancel; // the pending decode's token
	std::shared_ptr<bool>              m_alive  = std::make_shared<bool>(true);
	bool                               m_audio  = false; // SDL_INIT_AUDIO done
	SDL_AudioStream*                   m_stream = nullptr;
	std::optional<core::EntryId>       m_playing; // put to the stream and not stopped since
};

} // namespace kyty::gtk
