#pragma once

// Title_Sound decoding (Requirements 8.7, 8.9): sce_sys/snd0.at9, an ATRAC9 stream in a RIFF/WAVE
// container, decoded to interleaved 32-bit float PCM through the emulator's AjmAt9Decoder
// (src/libs/ajm/atrac9_decoder.h) as the Qt launcher's TitleSound::Decode did
// (src/launcher/src/titleSound.cpp). The GTK TitleSoundPlayer runs it on a worker thread.

#include "launcher/core/cancelToken.h"
#include "launcher/core/expected.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace kyty::core {

// Interleaved samples: samples.size() is a multiple of channels.
struct Pcm {
	std::vector<float> samples;
	int                channels = 0;
	int                rate     = 0; // samples per second and channel

	friend bool operator==(const Pcm&, const Pcm&) = default;
};

enum class TitleSoundError {
	Absent,        // the entry has no snd0.at9 (reported by the player; no warning, 8.9)
	Unreadable,    // the file exists but cannot be read (reported by the player)
	TooLarge,      // more than kMaxTitleSoundFileBytes
	InvalidHeader, // not a RIFF/WAVE file with a valid ATRAC9 fmt chunk and a data chunk
	Decode,        // the decoder rejected the data, or the PCM exceeds kMaxTitleSoundPcmBytes
	Cancelled      // the CancelToken was set
};

// The largest snd0.at9 that is decoded (Requirement 8.9): exactly 32 MiB is accepted.
inline constexpr uint64_t kMaxTitleSoundFileBytes = uint64_t {32} << 20u;
// The largest decoded PCM, as in the Qt launcher.
inline constexpr uint64_t kMaxTitleSoundPcmBytes = uint64_t {128} << 20u;

// A short English reason for a warning ("the title sound is larger than 32 MiB").
std::string_view DescribeTitleSoundError(TitleSoundError error);

// Decodes a snd0.at9 file. In order:
// - more than kMaxTitleSoundFileBytes: TooLarge;
// - the container is checked before the decoder sees any byte: "RIFF" with a size that fits in
//   the buffer (bytes after the RIFF chunk are ignored), "WAVE", chunks whose sizes fit inside the
//   RIFF chunk, a "fmt " chunk of at least 0x34 bytes with format tag 0xFFFE, the ATRAC9 sub-format
//   GUID, and a codec configuration that LibAtrac9 accepts, then a "data" chunk after it whose size
//   fits; any defect is InvalidHeader;
// - the RIFF chunk up to the end of the data chunk is decoded with
//   AjmAt9Decoder(0, 0, Float, AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER) in batches of 16 Ki
//   samples, honoring the "fact" chunk's sample count and skip; a decoder error, a partial final
//   superframe, no samples at all, or more than kMaxTitleSoundPcmBytes of PCM is Decode;
// - token is checked before every batch and at the end: Cancelled.
Expected<Pcm, TitleSoundError> DecodeTitleSound(std::span<const std::byte> at9,
                                                const CancelToken&         token);

} // namespace kyty::core
