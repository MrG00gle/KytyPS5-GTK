#pragma once

// The identity of a Game_Entry, shared by the title sound controller (titleSoundController.h) and
// the run state (runState.h).

#include <filesystem>

namespace kyty::core {

// A Game_Entry's identity: its canonical path (GameEntry::path).
using EntryId = std::filesystem::path;

} // namespace kyty::core
