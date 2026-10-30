// Feature: linux-gtk-modernization, Property 25: Log collection
//
// Validates Requirements 10.5, 10.7, and 10.8. Each case draws two byte streams (standard output
// and standard error, 0-400 bytes each) from a pool of ASCII, '\n', '\r', NUL, valid 2-, 3-, and
// 4-byte UTF-8, and ill-formed sequences, cuts each into chunks of 1-32 bytes (so chunks end
// inside lines and inside UTF-8 sequences), and interleaves the chunks at random, keeping each
// stream's order. A LogCollector with a capacity of 1-12 lines or the 200,000-line default gets
// the chunks, with TakeNewLines calls in between, then FlushPartialLines. The expected lines come
// from the reference split of each stream on '\n' (the last piece, when not empty, is the
// unterminated line): a complete line belongs to the chunk that holds its '\n', so the expected
// order is by chunk, then by position, followed by the unterminated lines, standard output's
// first. Every collected line is well-formed UTF-8, equals SanitizeUtf8 of its reference line
// (unchanged when that is well-formed, with U+FFFD otherwise), and the lines of each stream keep
// their order. TakeNewLines returns the lines added since the previous call (at most the
// capacity); the buffer retains the most recent min(total, capacity) lines; splitting ToText() on
// '\n' gives exactly the retained lines.

#include "launcher/core/logBuffer.h"
#include "launcher/core/text.h"
#include "support/property.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kyty::core::LogCollector;
using kyty::core::Stream;

int Choose(int min, int max) {
	return *rc::gen::resize(100, rc::gen::inRange(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

std::string StreamBytes() {
	static const std::vector<std::string> pieces = {
	    "\n", "\n", "\n", "\r", std::string(1, '\0'), "a", "Z", "0", " ", "line", "\xC3\xA9",
	    "\xD0\x96", "\xE6\x97\xA5", "\xE2\x82\xAC", "\xF0\x9F\x8E\xAE",
	    // Ill-formed: stray continuation bytes, bytes never valid, a lone lead byte, a truncated
	    // sequence, an overlong form, a surrogate, a code point above U+10FFFF.
	    "\x80", "\xBF", "\xFF", "\xFE", "\xC3", "\xE6\x97", "\xC0\xAF", "\xED\xA0\x80",
	    "\xF4\x90\x80\x80"};
	std::string bytes;
	const int   length = Choose(0, 401);
	while (static_cast<int>(bytes.size()) < length) {
		bytes += pieces[static_cast<size_t>(Choose(0, static_cast<int>(pieces.size())))];
	}
	return bytes;
}

// Chunk end offsets of 1-32 bytes each (often 1-3).
std::vector<size_t> ChunkEnds(size_t size) {
	std::vector<size_t> ends;
	size_t              offset = 0;
	while (offset < size) {
		const int length = OneIn(3) ? Choose(1, 4) : Choose(1, 33);
		offset           = std::min(size, offset + static_cast<size_t>(length));
		ends.push_back(offset);
	}
	return ends;
}

// The standard UTF-8 well-formedness rules (Unicode Table 3-7), independent of SanitizeUtf8.
bool IsWellFormedUtf8(std::string_view text) {
	size_t i = 0;
	while (i < text.size()) {
		const auto byte   = static_cast<unsigned char>(text[i]);
		size_t     length = 0;
		unsigned   low = 0x80, high = 0xBF;
		if (byte < 0x80) {
			i++;
			continue;
		}
		if (byte >= 0xC2 && byte <= 0xDF) {
			length = 2;
		} else if (byte >= 0xE0 && byte <= 0xEF) {
			length = 3;
			low    = byte == 0xE0 ? 0xA0 : 0x80;
			high   = byte == 0xED ? 0x9F : 0xBF;
		} else if (byte >= 0xF0 && byte <= 0xF4) {
			length = 4;
			low    = byte == 0xF0 ? 0x90 : 0x80;
			high   = byte == 0xF4 ? 0x8F : 0xBF;
		} else {
			return false;
		}
		if (i + length > text.size()) {
			return false;
		}
		for (size_t k = 1; k < length; k++) {
			const auto next = static_cast<unsigned char>(text[i + k]);
			if (next < (k == 1 ? low : 0x80) || next > (k == 1 ? high : 0xBF)) {
				return false;
			}
		}
		i += length;
	}
	return true;
}

struct ExpectedLine {
	size_t      chunk    = 0; // the index, in interleaved order, of the chunk holding the '\n'
	size_t      position = 0; // the '\n' offset in its stream
	std::string raw;          // the reference split's piece
};

std::vector<std::string> Retained(const LogCollector& log) {
	std::vector<std::string> lines;
	for (size_t i = 0; i < log.Lines().Size(); i++) {
		lines.push_back(log.Lines()[i]);
	}
	return lines;
}

// The last n lines of lines[0, end).
std::vector<std::string> Newest(const std::vector<std::string>& lines, size_t end, size_t n) {
	return {lines.begin() + static_cast<std::ptrdiff_t>(end - n),
	        lines.begin() + static_cast<std::ptrdiff_t>(end)};
}

} // namespace

KYTY_PROPERTY(25, "Log collection") {
	return rc::check(name, []() {
		const std::string         bytes[2] = {StreamBytes(), StreamBytes()};
		const std::vector<size_t> ends[2]  = {ChunkEnds(bytes[0].size()),
		                                      ChunkEnds(bytes[1].size())};

		// The interleaving: (stream, chunk index within the stream), each stream in order.
		struct Chunk {
			int    stream = 0;
			size_t begin  = 0;
			size_t end    = 0;
		};
		std::vector<Chunk> chunks;
		size_t             next[2] = {0, 0};
		while (next[0] < ends[0].size() || next[1] < ends[1].size()) {
			int s = OneIn(2) ? 0 : 1;
			if (next[s] == ends[s].size()) {
				s = 1 - s;
			}
			const size_t begin = next[s] == 0 ? 0 : ends[s][next[s] - 1];
			chunks.push_back({s, begin, ends[s][next[s]]});
			next[s]++;
		}

		// The reference split of each stream; each complete line is placed at its chunk.
		std::vector<ExpectedLine> complete;
		std::string               unterminated[2];
		for (int s = 0; s < 2; s++) {
			size_t start = 0;
			for (size_t i = 0; i < bytes[s].size(); i++) {
				if (bytes[s][i] != '\n') {
					continue;
				}
				size_t chunk = 0;
				while (!(chunks[chunk].stream == s && chunks[chunk].begin <= i &&
				         i < chunks[chunk].end)) {
					chunk++;
				}
				complete.push_back({chunk, i, bytes[s].substr(start, i - start)});
				start = i + 1;
			}
			unterminated[s] = bytes[s].substr(start);
		}
		std::sort(complete.begin(), complete.end(),
		          [](const ExpectedLine& a, const ExpectedLine& b) {
			          return a.chunk != b.chunk ? a.chunk < b.chunk : a.position < b.position;
		          });
		std::vector<std::string> raw;
		for (const auto& line: complete) {
			raw.push_back(line.raw);
		}
		for (const auto& line: unterminated) {
			if (!line.empty()) {
				raw.push_back(line);
			}
		}
		std::vector<std::string> expected;
		for (const auto& line: raw) {
			expected.push_back(kyty::core::SanitizeUtf8(line));
		}

		const bool   default_capacity = OneIn(4);
		const size_t capacity         = default_capacity ? LogCollector::kDefaultRetainedLines
		                                                 : static_cast<size_t>(Choose(1, 13));
		LogCollector log              = default_capacity ? LogCollector() : LogCollector(capacity);
		RC_ASSERT(log.Lines().Capacity() == capacity);
		RC_ASSERT(log.Lines().Capacity() >= (default_capacity ? 100'000u : 1u));
		RC_LOG() << "stdout " << bytes[0].size() << " bytes, stderr " << bytes[1].size()
		         << " bytes, " << chunks.size() << " chunks, capacity " << capacity << ", "
		         << expected.size() << " lines\n";

		size_t     total        = 0; // complete lines added so far
		size_t     taken        = 0; // total at the last TakeNewLines
		bool       evicted_take = false;
		const auto take         = [&](size_t now) {
			const auto fresh = log.TakeNewLines();
			const auto count = std::min(now - taken, std::min(now, capacity));
			RC_ASSERT(fresh == Newest(expected, now, count));
			evicted_take = evicted_take || count < now - taken;
			taken        = now;
		};
		for (size_t c = 0; c < chunks.size(); c++) {
			const auto&            chunk = chunks[c];
			const std::string_view piece(bytes[chunk.stream].data() + chunk.begin,
			                             chunk.end - chunk.begin);
			log.Append(chunk.stream == 0 ? Stream::Stdout : Stream::Stderr,
			           std::as_bytes(std::span(piece.data(), piece.size())));
			while (total < complete.size() && complete[total].chunk == c) {
				total++;
			}
			if (OneIn(4)) {
				take(total);
			}
		}

		// Before the flush, only complete lines are there.
		RC_ASSERT(Retained(log) ==
		          Newest(expected, complete.size(), std::min(complete.size(), capacity)));
		log.FlushPartialLines();
		take(expected.size());

		const auto retained = Retained(log);
		const auto kept     = std::min(expected.size(), capacity);
		RC_ASSERT(retained == Newest(expected, expected.size(), kept));
		RC_ASSERT(log.Lines().DroppedCount() == expected.size() - kept);
		for (size_t i = 0; i < kept; i++) {
			const auto& line      = retained[i];
			const auto& reference = raw[expected.size() - kept + i];
			RC_ASSERT(IsWellFormedUtf8(line));
			if (IsWellFormedUtf8(reference)) {
				RC_ASSERT(line == reference);
			} else {
				RC_ASSERT(line.find("\xEF\xBF\xBD") != std::string::npos);
			}
		}

		// ToText round trip.
		const auto               text = log.ToText();
		std::vector<std::string> split;
		size_t                   start = 0;
		for (size_t i = 0; i < text.size(); i++) {
			if (text[i] == '\n') {
				split.push_back(text.substr(start, i - start));
				start = i + 1;
			}
		}
		RC_ASSERT(start == text.size());
		RC_ASSERT(split == retained);

		RC_CLASSIFY(expected.size() > capacity, "lines evicted");
		RC_CLASSIFY(evicted_take, "TakeNewLines after eviction");
		RC_CLASSIFY(!unterminated[0].empty() || !unterminated[1].empty(), "unterminated last line");
		RC_TAG(default_capacity ? "default capacity" : "small capacity");
	});
}
