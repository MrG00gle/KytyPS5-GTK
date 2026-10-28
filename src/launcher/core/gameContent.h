#pragma once

// Archive-aware reads of the files inside a Game_Entry (Requirements 7.8, 7.9, 7.16, 8.x), ported
// from the Qt launcher's GameContent (src/launcher/src/gameContent.cpp). A directory entry's files
// are host files below it; an archive entry's files are members of the .zar, addressed as
// "<archive>.zar!/<member>" (Common::MakeArchivePath) and read through Common::File.

#include "launcher/core/gameMetadata.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kyty::core {

// The read limits the Qt launcher used: param.json 1 MiB (kMaxParamJsonBytes), images and the
// title sound 32 MiB.
inline constexpr uint64_t kMaxImageBytes      = uint64_t {32} << 20u;
inline constexpr uint64_t kMaxTitleSoundBytes = uint64_t {32} << 20u;

// The path of `relative` ("sce_sys/param.json") inside the entry: entry_path / relative for a
// directory, Common::MakeArchivePath(entry_path, relative) for an archive.
std::filesystem::path ResolveEntryFile(const std::filesystem::path& entry_path,
                                       GameEntry::Kind kind, std::string_view relative);

// True when `relative` is a regular file of the entry (symbolic links inside a directory entry are
// followed, as the Qt launcher did).
bool EntryFileExists(const std::filesystem::path& entry_path, GameEntry::Kind kind,
                     std::string_view relative);

// The whole content of `relative`, or nullopt when it is missing, not a regular file, larger than
// max_bytes, or cannot be read in full. Host files are checked with std::filesystem::status first,
// so a FIFO or device is never opened.
std::optional<std::string> ReadEntryFile(const std::filesystem::path& entry_path,
                                         GameEntry::Kind kind, std::string_view relative,
                                         uint64_t max_bytes);

// sce_sys/param.json, at most kMaxParamJsonBytes.
std::optional<std::string> ReadParamJson(const std::filesystem::path& entry_path,
                                         GameEntry::Kind              kind);
// sce_sys/icon0.png, at most kMaxImageBytes (Requirement 7.16: the caller shows a placeholder when
// this is nullopt or not a decodable PNG).
std::optional<std::string> ReadIcon0(const std::filesystem::path& entry_path, GameEntry::Kind kind);
// sce_sys/pic0.png, at most kMaxImageBytes.
std::optional<std::string> ReadPic0(const std::filesystem::path& entry_path, GameEntry::Kind kind);
// sce_sys/snd0.at9, at most kMaxTitleSoundBytes.
std::optional<std::string> ReadSnd0(const std::filesystem::path& entry_path, GameEntry::Kind kind);

} // namespace kyty::core
