#include "launcher/core/titleSoundController.h"

#include <utility>

namespace kyty::core {

TitleSoundCommand TitleSoundController::Select(std::optional<EntryId> entry) {
	// As the Qt launcher's SetTitle(running ? "" : entry): while a game runs no sound is wanted.
	std::optional<EntryId> wanted = m_running ? std::nullopt : std::move(entry);
	if (wanted == m_sound_entry) {
		return {};
	}
	m_sound_entry = std::move(wanted);
	m_generation++;
	TitleSoundCommand command;
	if (!m_sound_entry) {
		m_pending      = false;
		command.action = TitleSoundAction::StopAndCancel;
		return command;
	}
	m_pending          = true;
	command.action     = TitleSoundAction::StartDecode;
	command.generation = m_generation;
	command.entry      = *m_sound_entry;
	return command;
}

TitleSoundCommand TitleSoundController::DecodeFinished(uint64_t                       generation,
                                                       Expected<Pcm, TitleSoundError> result) {
	// m_pending holds only while the latest request is outstanding: never while a game runs, and
	// never without m_sound_entry.
	if (generation != m_generation || !m_pending) {
		return {};
	}
	m_pending = false;
	TitleSoundCommand command;
	command.generation = generation;
	command.entry      = *m_sound_entry;
	if (result) {
		command.action = TitleSoundAction::Play;
		command.pcm    = std::move(*result);
		return command;
	}
	if (result.error() != TitleSoundError::Absent && result.error() != TitleSoundError::Cancelled) {
		command.warning = "No title sound for " + m_sound_entry->string() + ": " +
		                  std::string(DescribeTitleSoundError(result.error()));
	}
	return command;
}

TitleSoundCommand TitleSoundController::RunStarted() {
	m_running = true;
	m_pending = false;
	m_sound_entry.reset();
	m_generation++;
	TitleSoundCommand command;
	command.action = TitleSoundAction::StopAndCancel;
	return command;
}

TitleSoundCommand TitleSoundController::GameExited() {
	m_running = false;
	return {};
}

} // namespace kyty::core
