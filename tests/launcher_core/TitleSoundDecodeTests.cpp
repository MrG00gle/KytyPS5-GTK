// Example tests of Title_Sound decoding (src/launcher/core/titleSound.h, task 14.2; Requirements
// 8.7, 8.9). A synthetic ATRAC9 RIFF file, built from the encoded mono block of
// tests/Ngs2SamplerTests.cpp (48 kHz, 256 samples per frame, 4 frames in a 384-byte superframe),
// decodes to the expected channels, rate, and sample count, also with a "fact" chunk, unknown
// chunks, and bytes after the RIFF chunk; 32 MiB is accepted and 32 MiB + 1 rejected; every header
// defect is InvalidHeader before the decoder sees the file; a truncated or empty payload is Decode;
// a cancelled token gives Cancelled.

#include "launcher/core/cancelToken.h"
#include "launcher/core/titleSound.h"
#include "support/noDisplay.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kyty::core::CancelToken;
using kyty::core::DecodeTitleSound;
using kyty::core::Pcm;
using kyty::core::TitleSoundError;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "TitleSoundDecodeTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

const char* ErrorName(TitleSoundError error) {
	switch (error) {
		case TitleSoundError::Absent: return "Absent";
		case TitleSoundError::Unreadable: return "Unreadable";
		case TitleSoundError::TooLarge: return "TooLarge";
		case TitleSoundError::InvalidHeader: return "InvalidHeader";
		case TitleSoundError::Decode: return "Decode";
		case TitleSoundError::Cancelled: return "Cancelled";
	}
	return "?";
}

// ---- Fixture builder

constexpr size_t kSuperframeBytes = 384;
constexpr int    kFrameSamples    = 256;
constexpr int    kFrames          = 4;

// ATRAC9 configuration words: 48 kHz, 4 frames of 96 bytes per superframe, mono or dual mono.
constexpr uint8_t kMonoConfig[4]     = {0xfe, 0x70, 0x0b, 0xf0};
constexpr uint8_t kDualMonoConfig[4] = {0xfe, 0x72, 0x0b, 0xf0};

void AppendLe16(std::string& out, uint32_t value) {
	out += static_cast<char>(value & 0xffu);
	out += static_cast<char>((value >> 8u) & 0xffu);
}

void AppendLe32(std::string& out, uint32_t value) {
	AppendLe16(out, value & 0xffffu);
	AppendLe16(out, value >> 16u);
}

void PutLe32(std::string& out, size_t offset, uint32_t value) {
	std::string bytes;
	AppendLe32(bytes, value);
	out.replace(offset, 4, bytes);
}

// A chunk: id, size (the payload size unless given), payload, and a pad byte for an odd size.
std::string Chunk(std::string_view id, const std::string& payload,
                  std::optional<uint32_t> size_field = std::nullopt) {
	std::string chunk(id);
	AppendLe32(chunk, size_field.value_or(static_cast<uint32_t>(payload.size())));
	chunk += payload;
	if (payload.size() % 2 != 0) {
		chunk += '\0';
	}
	return chunk;
}

// "RIFF", the size (4 + body unless given), "WAVE", body.
std::string Riff(const std::string& body, std::optional<uint32_t> size_field = std::nullopt) {
	std::string file = "RIFF";
	AppendLe32(file, size_field.value_or(static_cast<uint32_t>(4 + body.size())));
	file += "WAVE";
	file += body;
	return file;
}

// A WAVEFORMATEXTENSIBLE fmt chunk payload for ATRAC9 (0x34 bytes unless size is given).
std::string FmtPayload(const uint8_t (&config)[4], int channels, size_t size = 0x34) {
	std::string fmt;
	AppendLe16(fmt, 0xfffe);                                               // format tag
	AppendLe16(fmt, static_cast<uint32_t>(channels));                      // channels
	AppendLe32(fmt, 48000);                                                // samples per second
	AppendLe32(fmt, 48000 * kSuperframeBytes / (kFrames * kFrameSamples)); // bytes per second
	AppendLe16(fmt, kSuperframeBytes);                                     // block align
	AppendLe16(fmt, 0);                                                    // bits per sample
	AppendLe16(fmt, 0x22);                                                 // extension size
	AppendLe16(fmt, kFrameSamples);                                        // samples per block
	AppendLe32(fmt, channels == 1 ? 0x4u : 0x3u);                          // channel mask
	constexpr uint8_t kGuid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                               0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	fmt.append(reinterpret_cast<const char*>(kGuid), sizeof(kGuid));
	AppendLe32(fmt, 1); // version
	fmt.append(reinterpret_cast<const char*>(config), 4);
	fmt.resize(size, '\0'); // reserved bytes, or a cut chunk
	return fmt;
}

// superframes superframes of encoded audio: per frame, one block per channel (the mono block of
// tests/Ngs2SamplerTests.cpp, with the bit that frames 1-3 set), then zero padding.
std::string AudioPayload(int channels, int superframes) {
	constexpr uint8_t kMonoBlock[36] = {0, 0, 0x04, 0x20, 0x04, 0xc0, 0, 0, 0x1f, 0xc0};
	std::string       data;
	for (int superframe = 0; superframe < superframes; superframe++) {
		std::string block_bytes;
		for (int frame = 0; frame < kFrames; frame++) {
			for (int channel = 0; channel < channels; channel++) {
				std::string block(reinterpret_cast<const char*>(kMonoBlock), sizeof(kMonoBlock));
				if (frame != 0) {
					block[0] = static_cast<char>(block[0] | 0x80);
				}
				block_bytes += block;
			}
		}
		block_bytes.resize(kSuperframeBytes, '\0');
		data += block_bytes;
	}
	return data;
}

std::string FactPayload(uint32_t total_samples, uint32_t skip_samples) {
	std::string fact;
	AppendLe32(fact, total_samples);
	AppendLe32(fact, skip_samples);
	AppendLe32(fact, 0);
	return fact;
}

// A mono or dual-mono file with superframes superframes.
std::string MakeFile(int channels, int superframes) {
	const auto& config = channels == 1 ? kMonoConfig : kDualMonoConfig;
	return Riff(Chunk("fmt ", FmtPayload(config, channels)) +
	            Chunk("data", AudioPayload(channels, superframes)));
}

kyty::core::Expected<Pcm, TitleSoundError> Decode(const std::string& file) {
	const CancelToken token;
	return DecodeTitleSound(std::as_bytes(std::span(file.data(), file.size())), token);
}

void CheckPcm(const kyty::core::Expected<Pcm, TitleSoundError>& result, int channels, size_t frames,
              const std::string& what) {
	Check(result.has_value(),
	      what + ": decodes (got " + (result ? "" : ErrorName(result.error())) + ")");
	Check(result->channels == channels, what + ": channel count");
	Check(result->rate == 48000, what + ": sample rate");
	Check(result->samples.size() == frames * static_cast<size_t>(channels),
	      what + ": sample count " + std::to_string(result->samples.size()) + ", expected " +
	          std::to_string(frames * static_cast<size_t>(channels)));
	bool audible = false;
	for (const float sample: result->samples) {
		Check(std::isfinite(sample), what + ": finite samples");
		audible = audible || std::fabs(sample) > 0.00001f;
	}
	Check(audible, what + ": the samples are not all silent");
}

void CheckError(const std::string& file, TitleSoundError expected, const std::string& what) {
	const auto result = Decode(file);
	Check(!result.has_value(), what + ": rejected");
	Check(result.error() == expected,
	      what + ": " + ErrorName(expected) + " expected, got " + ErrorName(result.error()));
}

// ---- Tests

void TestDecode() {
	// 40 superframes give 40960 samples, so the 16 Ki sample batches fill up twice.
	CheckPcm(Decode(MakeFile(1, 40)), 1, 40 * kFrames * kFrameSamples, "mono, 40 superframes");
	CheckPcm(Decode(MakeFile(1, 1)), 1, kFrames * kFrameSamples, "mono, 1 superframe");
	CheckPcm(Decode(MakeFile(2, 3)), 2, 3 * kFrames * kFrameSamples, "dual mono, 3 superframes");

	// Unknown chunks (one of odd size, so a pad byte follows) are skipped; a fact chunk limits the
	// samples and skips the first ones; bytes after the RIFF chunk are ignored.
	const auto fmt  = Chunk("fmt ", FmtPayload(kMonoConfig, 1));
	const auto data = Chunk("data", AudioPayload(1, 3));
	CheckPcm(Decode(Riff(Chunk("LIST", "odd") + fmt + Chunk("junk", "abcd") + data) + "trailer"), 1,
	         3 * kFrames * kFrameSamples, "unknown chunks and trailing bytes");
	CheckPcm(Decode(Riff(fmt + Chunk("fact", FactPayload(2500, 100)) + data)), 1, 2500,
	         "fact chunk with 2500 samples after skipping 100");
	CheckPcm(Decode(Riff(fmt + Chunk("fact", FactPayload(1000, 0)) + data)), 1, 1000,
	         "fact chunk ending inside a superframe");
	// A chunk after the data chunk is not audio: decoding stops at the end of the data chunk.
	CheckPcm(Decode(Riff(fmt + data + Chunk("LIST", "after the audio"))), 1,
	         3 * kFrames * kFrameSamples, "chunk after the data chunk");
	// A fmt chunk longer than 0x34 bytes.
	CheckPcm(Decode(Riff(Chunk("fmt ", FmtPayload(kMonoConfig, 1, 0x40)) + data)), 1,
	         3 * kFrames * kFrameSamples, "fmt chunk of 0x40 bytes");

	// Same input, same output.
	const auto file = MakeFile(2, 2);
	Check(*Decode(file) == *Decode(file), "decoding is deterministic");
}

void TestSizeLimit() {
	Check(kyty::core::kMaxTitleSoundFileBytes == 32u << 20u, "the limit is 32 MiB");
	// Exactly 32 MiB: a valid file followed by padding after the RIFF chunk.
	auto file = MakeFile(1, 2);
	file.resize(32u << 20u, '\0');
	CheckPcm(Decode(file), 1, 2 * kFrames * kFrameSamples, "a file of exactly 32 MiB");
	file.push_back('\0');
	CheckError(file, TitleSoundError::TooLarge, "a file of 32 MiB + 1 byte");
	CheckError(std::string((32u << 20u) + 1u, 'x'), TitleSoundError::TooLarge,
	           "32 MiB + 1 byte of garbage");
}

void TestHeaderDefects() {
	const auto fmt   = Chunk("fmt ", FmtPayload(kMonoConfig, 1));
	const auto audio = AudioPayload(1, 2);
	const auto data  = Chunk("data", audio);
	const auto good  = Riff(fmt + data);
	CheckPcm(Decode(good), 1, 2 * kFrames * kFrameSamples, "the unmodified file");

	std::vector<std::pair<std::string, std::string>> cases;
	cases.emplace_back("empty file", "");
	cases.emplace_back("11 bytes", good.substr(0, 11));
	cases.emplace_back("RIFF and WAVE only", good.substr(0, 12));
	cases.emplace_back("fmt chunk header cut", good.substr(0, 12) + "fmt ");
	auto bytes = good;
	bytes[3]   = 'X';
	cases.emplace_back("RIFX instead of RIFF", bytes);
	bytes     = good;
	bytes[11] = 'X';
	cases.emplace_back("WAVX instead of WAVE", bytes);
	cases.emplace_back("RIFF size larger than the file",
	                   Riff(fmt + data, static_cast<uint32_t>(4 + fmt.size() + data.size() + 1)));
	cases.emplace_back("RIFF size below 4", Riff(fmt + data, 3));
	cases.emplace_back("RIFF size 0xFFFFFFFF", Riff(fmt + data, 0xffffffffu));
	cases.emplace_back("RIFF size ends before the data chunk", Riff(fmt + data, 4));
	cases.emplace_back("file cut inside the audio", good.substr(0, good.size() - 100));
	cases.emplace_back("fmt chunk size beyond the RIFF chunk",
	                   Riff(Chunk("fmt ", FmtPayload(kMonoConfig, 1), 0x1000) + data));
	cases.emplace_back("fmt chunk of 0x30 bytes",
	                   Riff(Chunk("fmt ", FmtPayload(kMonoConfig, 1, 0x30)) + data));
	cases.emplace_back("fmt chunk of 0x33 bytes",
	                   Riff(Chunk("fmt ", FmtPayload(kMonoConfig, 1, 0x33)) + data));
	auto fmt_payload = FmtPayload(kMonoConfig, 1);
	fmt_payload[0]   = 0x01;
	fmt_payload[1]   = 0x00;
	cases.emplace_back("PCM format tag", Riff(Chunk("fmt ", fmt_payload) + data));
	fmt_payload = FmtPayload(kMonoConfig, 1);
	fmt_payload[24 + 15] ^= 0x01;
	cases.emplace_back("other sub-format GUID", Riff(Chunk("fmt ", fmt_payload) + data));
	constexpr uint8_t kNoSync[4] = {0x00, 0x70, 0x0b, 0xf0};
	cases.emplace_back("config without the 0xFE sync byte",
	                   Riff(Chunk("fmt ", FmtPayload(kNoSync, 1)) + data));
	constexpr uint8_t kBadDiscrete[4] = {0x30, 0x80, 0x00, 0x00}; // reserved bit 23 set
	cases.emplace_back("invalid discrete multichannel config",
	                   Riff(Chunk("fmt ", FmtPayload(kBadDiscrete, 1)) + data));
	cases.emplace_back("no data chunk", Riff(fmt));
	cases.emplace_back("no fmt chunk", Riff(data));
	cases.emplace_back("data chunk before the fmt chunk", Riff(data + fmt));
	cases.emplace_back("data size larger than the file",
	                   Riff(fmt + Chunk("data", audio, static_cast<uint32_t>(audio.size() + 1))));
	cases.emplace_back("data chunk header cut", Riff(fmt + "data"));
	cases.emplace_back("unknown chunk size beyond the RIFF chunk",
	                   Riff(Chunk("LIST", "x", 0x7fffffffu) + fmt + data));
	cases.emplace_back("not a RIFF file at all", std::string(1000, '\x5a'));

	for (const auto& [what, file]: cases) {
		CheckError(file, TitleSoundError::InvalidHeader, what);
	}

	// The checks are on the bytes, so editing a valid file's size fields is caught as well.
	bytes = good;
	PutLe32(bytes, 16, 0x30);
	CheckError(bytes, TitleSoundError::InvalidHeader, "fmt size field edited to 0x30");
}

void TestDecodeFailures() {
	const auto fmt = Chunk("fmt ", FmtPayload(kMonoConfig, 1));
	CheckError(Riff(fmt + Chunk("data", "")), TitleSoundError::Decode, "empty data chunk");
	CheckError(Riff(fmt + Chunk("data", AudioPayload(1, 1).substr(0, 100))),
	           TitleSoundError::Decode, "data chunk shorter than a superframe");
	CheckError(Riff(fmt + Chunk("data", AudioPayload(1, 2).substr(0, kSuperframeBytes + 100))),
	           TitleSoundError::Decode, "truncated final superframe");
	CheckError(Riff(fmt + Chunk("fact", FactPayload(5000, 0)) +
	                Chunk("data", AudioPayload(1, 2).substr(0, kSuperframeBytes + 200))),
	           TitleSoundError::Decode, "fact chunk promising more samples than the payload has");
}

void TestCancel() {
	const auto  file = MakeFile(1, 4);
	CancelToken token;
	token.Cancel();
	const auto cancelled =
	    DecodeTitleSound(std::as_bytes(std::span(file.data(), file.size())), token);
	Check(!cancelled && cancelled.error() == TitleSoundError::Cancelled,
	      "a cancelled token gives Cancelled");
	CheckPcm(Decode(file), 1, 4 * kFrames * kFrameSamples, "a new token decodes the same file");
	// The size and header checks come first: an oversized or invalid file is not Cancelled.
	CheckError(std::string((32u << 20u) + 1u, '\0'), TitleSoundError::TooLarge,
	           "too large before cancelled");
	const std::string invalid = "RIFF";
	const auto        result =
	    DecodeTitleSound(std::as_bytes(std::span(invalid.data(), invalid.size())), token);
	Check(!result && result.error() == TitleSoundError::InvalidHeader,
	      "an invalid header is InvalidHeader even when cancelled");
}

void TestDescribe() {
	std::set<std::string> texts;
	for (const auto error:
	     {TitleSoundError::Absent, TitleSoundError::Unreadable, TitleSoundError::TooLarge,
	      TitleSoundError::InvalidHeader, TitleSoundError::Decode, TitleSoundError::Cancelled}) {
		const std::string text(kyty::core::DescribeTitleSoundError(error));
		Check(!text.empty(), std::string("a description for ") + ErrorName(error));
		texts.insert(text);
	}
	Check(texts.size() == 6, "every error has its own description");
	Check(std::string(kyty::core::DescribeTitleSoundError(TitleSoundError::TooLarge))
	              .find("32 MiB") != std::string::npos,
	      "the size error names the limit");
	Check(!kyty::core::DescribeTitleSoundError(static_cast<TitleSoundError>(99)).empty(),
	      "an out-of-range value still has a description");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("TitleSoundDecodeTests");
	TestDecode();
	TestSizeLimit();
	TestHeaderDefects();
	TestDecodeFailures();
	TestCancel();
	TestDescribe();
	std::printf("TitleSoundDecodeTests: all passed\n");
	return 0;
}
