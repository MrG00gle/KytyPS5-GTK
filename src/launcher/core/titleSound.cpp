#include "launcher/core/titleSound.h"

#include "libs/ajm/atrac9_decoder.h"

#include <array>
#include <cstring>

namespace kyty::core {

namespace {

namespace Ajm = Libs::Audio::Ajm;

constexpr size_t kBatchSamples    = 16 * 1024;
constexpr size_t kMaxPcmSamples   = kMaxTitleSoundPcmBytes / sizeof(float);
constexpr size_t kMinFmtChunkSize = 0x34;
// LibAtrac9's bit reader may read a few bytes past the end of a frame, so the decoder input gets
// zero bytes after the data chunk.
constexpr size_t kDecoderPadding = 64;

// KSDATAFORMAT_SUBTYPE_ATRAC9 {47E142D2-36BA-4D8D-88FC-61654F8C836C}, as stored in the fmt chunk.
constexpr std::array<uint8_t, 16> kAtrac9Guid = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                                 0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

const uint8_t* Bytes(std::span<const std::byte> data, size_t offset) {
	return reinterpret_cast<const uint8_t*>(data.data()) + offset;
}

uint16_t ReadLe16(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8u));
}

uint32_t ReadLe32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
	       (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
}

bool IsFourCc(const uint8_t* p, const char (&id)[5]) {
	return std::memcmp(p, id, 4) == 0;
}

// Checks the RIFF container as described in titleSound.h and returns the offset just past the
// data chunk's payload.
Expected<size_t, TitleSoundError> CheckContainer(std::span<const std::byte> file) {
	const auto invalid = MakeUnexpected(TitleSoundError::InvalidHeader);
	if (file.size() < 12 || !IsFourCc(Bytes(file, 0), "RIFF") ||
	    !IsFourCc(Bytes(file, 8), "WAVE")) {
		return invalid;
	}
	const size_t riff_size = ReadLe32(Bytes(file, 4));
	if (riff_size < 4 || riff_size > file.size() - 8) {
		return invalid;
	}
	const size_t end        = 8 + riff_size;
	size_t       offset     = 12;
	bool         format_set = false;
	// end is at most 32 MiB + 8, so none of these sums overflows.
	while (offset + 8 <= end) {
		const uint8_t* chunk   = Bytes(file, offset);
		const size_t   size    = ReadLe32(chunk + 4);
		const size_t   payload = offset + 8;
		if (size > end - payload) {
			return invalid;
		}
		if (IsFourCc(chunk, "fmt ")) {
			const uint8_t*  format = chunk + 8;
			Atrac9CodecInfo info {};
			if (size < kMinFmtChunkSize || ReadLe16(format) != 0xfffe ||
			    std::memcmp(format + 24, kAtrac9Guid.data(), kAtrac9Guid.size()) != 0 ||
			    !Ajm::AjmAt9GetCodecInfo(format + 44, info)) {
				return invalid;
			}
			format_set = true;
		} else if (IsFourCc(chunk, "data")) {
			if (!format_set) {
				return invalid;
			}
			return payload + size;
		}
		offset = payload + size + (size & 1u);
	}
	return invalid;
}

} // namespace

std::string_view DescribeTitleSoundError(TitleSoundError error) {
	switch (error) {
		case TitleSoundError::Absent: return "the game has no title sound";
		case TitleSoundError::Unreadable: return "the title sound file cannot be read";
		case TitleSoundError::TooLarge: return "the title sound file is larger than 32 MiB";
		case TitleSoundError::InvalidHeader:
			return "the title sound file is not a valid RIFF ATRAC9 file";
		case TitleSoundError::Decode: return "the title sound cannot be decoded";
		case TitleSoundError::Cancelled: return "the title sound decoding was cancelled";
	}
	return "unknown title sound error";
}

Expected<Pcm, TitleSoundError> DecodeTitleSound(std::span<const std::byte> at9,
                                                const CancelToken&         token) {
	if (at9.size() > kMaxTitleSoundFileBytes) {
		return MakeUnexpected(TitleSoundError::TooLarge);
	}
	const auto data_end = CheckContainer(at9);
	if (!data_end) {
		return MakeUnexpected(data_end.error());
	}
	const size_t         size = *data_end;
	std::vector<uint8_t> input(size + kDecoderPadding, 0);
	std::memcpy(input.data(), at9.data(), size);

	Ajm::AjmAt9Decoder   decoder(0, 0, Ajm::AjmSampleEncoding::Float,
	                             Ajm::AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER);
	Ajm::AjmGaplessState gapless;
	std::vector<float>   batch(kBatchSamples);
	Pcm                  pcm;
	size_t               offset = 0;
	while (offset < size && !gapless.IsEnd()) {
		if (token.IsCancelled()) {
			return MakeUnexpected(TitleSoundError::Cancelled);
		}
		const auto result = decoder.Decode(input.data() + offset, size - offset, batch.data(),
		                                   batch.size() * sizeof(float), true, &gapless);
		if ((result.result != 0 && result.result != Ajm::AJM_RESULT_NOT_ENOUGH_ROOM) ||
		    result.input_consumed == 0) {
			return MakeUnexpected(TitleSoundError::Decode);
		}
		const size_t samples = result.output_written / sizeof(float);
		if (samples > kMaxPcmSamples - pcm.samples.size()) {
			return MakeUnexpected(TitleSoundError::Decode);
		}
		pcm.samples.insert(pcm.samples.end(), batch.begin(),
		                   batch.begin() + static_cast<std::ptrdiff_t>(samples));
		offset += result.input_consumed;
	}
	if (token.IsCancelled()) {
		return MakeUnexpected(TitleSoundError::Cancelled);
	}
	const auto format = decoder.GetFormat();
	if (pcm.samples.empty() || format.channel_num == 0 || format.sampling_frequency == 0 ||
	    pcm.samples.size() % format.channel_num != 0) {
		return MakeUnexpected(TitleSoundError::Decode);
	}
	pcm.channels = static_cast<int>(format.channel_num);
	pcm.rate     = static_cast<int>(format.sampling_frequency);
	return pcm;
}

} // namespace kyty::core
