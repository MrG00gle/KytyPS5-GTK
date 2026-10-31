#include "launcher/gtk/services/titleSoundPlayer.h"

#include "launcher/core/gameContent.h"
#include "launcher/core/titleSound.h"

#include <SDL3/SDL.h>
#include <cstddef>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace kyty::gtk {

namespace {

constexpr std::string_view kTitleSound = "sce_sys/snd0.at9";

core::Expected<core::Pcm, core::TitleSoundError> ReadAndDecode(const core::EntryId&     entry,
                                                               core::GameEntry::Kind    kind,
                                                               const core::CancelToken& cancel) {
	using core::TitleSoundError;
	if (cancel.IsCancelled()) {
		return core::MakeUnexpected(TitleSoundError::Cancelled);
	}
	if (!core::EntryFileExists(entry, kind, kTitleSound)) {
		return core::MakeUnexpected(TitleSoundError::Absent);
	}
	const auto bytes = core::ReadSnd0(entry, kind);
	if (!bytes) {
		// ReadSnd0 refuses a file over the limit; only a directory entry's file size can be read
		// here (the GTK layer does not open archives itself).
		if (kind == core::GameEntry::Kind::Directory) {
			std::error_code ec;
			const auto      size =
			    std::filesystem::file_size(core::ResolveEntryFile(entry, kind, kTitleSound), ec);
			if (!ec && size > core::kMaxTitleSoundFileBytes) {
				return core::MakeUnexpected(TitleSoundError::TooLarge);
			}
		}
		return core::MakeUnexpected(TitleSoundError::Unreadable);
	}
	return core::DecodeTitleSound(std::as_bytes(std::span(bytes->data(), bytes->size())), cancel);
}

} // namespace

// One decode request; the GTask owns it.
struct TitleSoundPlayer::Decode {
	std::shared_ptr<bool>                            alive;
	TitleSoundPlayer*                                player     = nullptr;
	uint64_t                                         generation = 0;
	core::EntryId                                    entry;
	core::GameEntry::Kind                            kind = core::GameEntry::Kind::Directory;
	std::shared_ptr<core::CancelToken>               cancel;
	core::Expected<core::Pcm, core::TitleSoundError> result;
};

TitleSoundPlayer::TitleSoundPlayer() = default;

TitleSoundPlayer::~TitleSoundPlayer() {
	*m_alive = false;
	if (m_cancel) {
		m_cancel->Cancel();
	}
	if (m_stream != nullptr) {
		SDL_DestroyAudioStream(m_stream);
	}
	if (m_audio) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
}

void TitleSoundPlayer::Select(const core::GameEntry* entry) {
	const auto kind = entry != nullptr ? entry->kind : core::GameEntry::Kind::Directory;
	Execute(m_controller.Select(entry != nullptr ? std::optional<core::EntryId>(entry->path)
	                                             : std::nullopt),
	        kind);
}

void TitleSoundPlayer::RunStarted() {
	Execute(m_controller.RunStarted(), core::GameEntry::Kind::Directory);
}

void TitleSoundPlayer::GameExited() {
	Execute(m_controller.GameExited(), core::GameEntry::Kind::Directory);
}

std::optional<core::EntryId> TitleSoundPlayer::PlayingEntry() const {
	if (!m_playing || m_stream == nullptr || SDL_AudioStreamDevicePaused(m_stream) ||
	    SDL_GetAudioStreamQueued(m_stream) <= 0) {
		return std::nullopt;
	}
	return m_playing;
}

void TitleSoundPlayer::Execute(core::TitleSoundCommand command, core::GameEntry::Kind kind) {
	if (!command.warning.empty()) {
		g_warning("%s", command.warning.c_str());
	}
	switch (command.action) {
		case core::TitleSoundAction::None: break;
		case core::TitleSoundAction::StartDecode:
			StopAndCancel();
			StartDecode(command.generation, command.entry, kind);
			break;
		case core::TitleSoundAction::Play: Play(command); break;
		case core::TitleSoundAction::StopAndCancel: StopAndCancel(); break;
	}
}

void TitleSoundPlayer::StartDecode(uint64_t generation, const core::EntryId& entry,
                                   core::GameEntry::Kind kind) {
	m_cancel           = std::make_shared<core::CancelToken>();
	auto* decode       = new Decode;
	decode->alive      = m_alive;
	decode->player     = this;
	decode->generation = generation;
	decode->entry      = entry;
	decode->kind       = kind;
	decode->cancel     = m_cancel;
	GTask* task        = g_task_new(nullptr, nullptr, &OnDecoded, nullptr);
	g_task_set_task_data(task, decode, [](gpointer data) { delete static_cast<Decode*>(data); });
	g_task_run_in_thread(task, &RunDecode);
	g_object_unref(task);
}

void TitleSoundPlayer::RunDecode(GTask* task, gpointer /*source*/, gpointer task_data,
                                 GCancellable* /*cancellable*/) {
	auto* decode   = static_cast<Decode*>(task_data);
	decode->result = ReadAndDecode(decode->entry, decode->kind, *decode->cancel);
	g_task_return_boolean(task, TRUE);
}

void TitleSoundPlayer::OnDecoded(GObject* /*source*/, GAsyncResult* result, gpointer /*data*/) {
	auto* decode = static_cast<Decode*>(g_task_get_task_data(G_TASK(result)));
	if (!*decode->alive) {
		return;
	}
	auto* player = decode->player;
	if (decode->cancel == player->m_cancel) {
		player->m_cancel.reset();
	}
	player->Execute(
	    player->m_controller.DecodeFinished(decode->generation, std::move(decode->result)),
	    decode->kind);
}

void TitleSoundPlayer::Play(const core::TitleSoundCommand& command) {
	const core::Pcm&    pcm = command.pcm;
	const SDL_AudioSpec spec {SDL_AUDIO_F32, pcm.channels, pcm.rate};
	if (!m_audio) {
		SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
			g_warning("No title sound for %s: no audio output: %s", command.entry.c_str(),
			          SDL_GetError());
			return;
		}
		m_audio = true;
	}
	if (m_stream == nullptr) {
		m_stream =
		    SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (m_stream == nullptr) {
			g_warning("No title sound for %s: cannot open the audio output: %s",
			          command.entry.c_str(), SDL_GetError());
			return;
		}
	}
	SDL_ClearAudioStream(m_stream);
	const auto bytes = static_cast<int>(pcm.samples.size() * sizeof(float));
	if (!SDL_SetAudioStreamFormat(m_stream, &spec, nullptr) ||
	    !SDL_PutAudioStreamData(m_stream, pcm.samples.data(), bytes) ||
	    !SDL_FlushAudioStream(m_stream) || !SDL_ResumeAudioStreamDevice(m_stream)) {
		g_warning("No title sound for %s: cannot play it: %s", command.entry.c_str(),
		          SDL_GetError());
		StopAndCancel();
		return;
	}
	m_playing = command.entry;
	g_debug("title sound of %s playing (%d channels, %d Hz, %zu samples)", command.entry.c_str(),
	        pcm.channels, pcm.rate, pcm.samples.size());
}

void TitleSoundPlayer::StopAndCancel() {
	if (m_cancel) {
		m_cancel->Cancel();
		m_cancel.reset();
	}
	if (m_stream != nullptr) {
		SDL_ClearAudioStream(m_stream);
		SDL_PauseAudioStreamDevice(m_stream);
	}
	if (m_playing) {
		g_debug("title sound of %s stopped", m_playing->c_str());
		m_playing.reset();
	}
}

} // namespace kyty::gtk
