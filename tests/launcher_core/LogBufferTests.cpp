// Example tests of the log buffer (src/launcher/core/ringBuffer.h and logBuffer.h, task 15.5):
// chunk boundaries inside lines and inside UTF-8 sequences, interleaved streams, invalid bytes,
// '\r', unterminated last lines, capacity eviction, TakeNewLines, ToText, and Clear.

#include "launcher/core/logBuffer.h"
#include "launcher/core/ringBuffer.h"
#include "support/noDisplay.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kyty::core::LogCollector;
using kyty::core::RingBuffer;
using kyty::core::Stream;
using Lines = std::vector<std::string>;

constexpr std::string_view kReplacement = "\xEF\xBF\xBD"; // U+FFFD

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "LogBufferTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void Append(LogCollector& log, Stream stream, std::string_view bytes) {
	log.Append(stream, std::as_bytes(std::span(bytes.data(), bytes.size())));
}

Lines Retained(const LogCollector& log) {
	Lines lines;
	for (std::size_t i = 0; i < log.Lines().Size(); i++) {
		lines.push_back(log.Lines()[i]);
	}
	return lines;
}

std::string Show(const Lines& lines) {
	std::string text = "[";
	for (const auto& line: lines) {
		text += (text.size() > 1 ? ", '" : "'") + line + "'";
	}
	return text + "]";
}

void CheckLines(const Lines& actual, const Lines& expected, const std::string& context) {
	Check(actual == expected, context + ": " + Show(actual) + ", expected " + Show(expected));
}

void TestRingBuffer() {
	RingBuffer<int> ring(3);
	Check(ring.Capacity() == 3 && ring.Size() == 0 && ring.Empty() && ring.DroppedCount() == 0,
	      "a new ring buffer is empty");
	ring.PushBack(1);
	ring.PushBack(2);
	Check(ring.Size() == 2 && !ring.Empty() && ring[0] == 1 && ring[1] == 2,
	      "values in order before the buffer is full");
	for (int value = 3; value <= 7; value++) {
		ring.PushBack(value);
	}
	Check(ring.Size() == 3 && ring[0] == 5 && ring[1] == 6 && ring[2] == 7,
	      "the newest values are kept, oldest first");
	Check(ring.DroppedCount() == 4, "four values were dropped");
	ring.Clear();
	Check(ring.Size() == 0 && ring.Empty() && ring.DroppedCount() == 0 && ring.Capacity() == 3,
	      "Clear empties the buffer and keeps the capacity");
	ring.PushBack(8);
	Check(ring.Size() == 1 && ring[0] == 8, "values after Clear");

	RingBuffer<int> zero(0);
	Check(zero.Capacity() == 1, "a capacity of 0 counts as 1");
	zero.PushBack(1);
	zero.PushBack(2);
	Check(zero.Size() == 1 && zero[0] == 2 && zero.DroppedCount() == 1, "capacity 1");
}

void TestChunkBoundaries() {
	LogCollector log;
	Append(log, Stream::Stdout, "hel");
	CheckLines(log.TakeNewLines(), {}, "no line before its newline");
	Append(log, Stream::Stdout, "lo\nwor");
	Append(log, Stream::Stdout, "ld\n");
	Append(log, Stream::Stdout, "");
	Append(log, Stream::Stdout, "\n\nlast\n");
	CheckLines(log.TakeNewLines(), {"hello", "world", "", "", "last"}, "lines across chunks");
	CheckLines(log.TakeNewLines(), {}, "nothing new");

	// A UTF-8 sequence split between chunks stays one character.
	Append(log, Stream::Stderr, "caf\xC3");
	Append(log, Stream::Stderr, "\xA9 \xE6\x97");
	Append(log, Stream::Stderr, "\xA5\xF0\x9F");
	Append(log, Stream::Stderr, "\x8E\xAE\n");
	CheckLines(log.TakeNewLines(), {"caf\xC3\xA9 \xE6\x97\xA5\xF0\x9F\x8E\xAE"},
	           "a split UTF-8 sequence");
}

void TestInterleavedStreams() {
	LogCollector log;
	Append(log, Stream::Stdout, "out ");
	Append(log, Stream::Stderr, "err 1\nerr ");
	Append(log, Stream::Stdout, "1\nout 2\n");
	Append(log, Stream::Stderr, "2\n");
	CheckLines(log.TakeNewLines(), {"err 1", "out 1", "out 2", "err 2"},
	           "each stream keeps its partial line and its order");

	// A partial UTF-8 sequence on one stream is not joined with the other stream's bytes.
	Append(log, Stream::Stdout, "\xC3");
	Append(log, Stream::Stderr, "\xA9\n");
	Append(log, Stream::Stdout, "\n");
	CheckLines(log.TakeNewLines(), {std::string(kReplacement), std::string(kReplacement)},
	           "the streams' bytes are never mixed");
}

void TestInvalidUtf8AndCarriageReturn() {
	LogCollector log;
	Append(log, Stream::Stdout,
	       "a\xFF"
	       "b\n");
	Append(log, Stream::Stdout, "\xC0\xAF\n");         // overlong '/'
	Append(log, Stream::Stdout, "\xED\xA0\x80x\n");    // a surrogate
	Append(log, Stream::Stdout, "\xE6\x97\n");         // truncated at the line end
	Append(log, Stream::Stdout, "\xF4\x90\x80\x80\n"); // above U+10FFFF
	Append(log, Stream::Stdout, "dos\r\n\r\n");
	Append(log, Stream::Stdout, std::string_view("nul\0byte\n", 9));
	const std::string r(kReplacement);
	CheckLines(log.TakeNewLines(),
	           {"a" + r + "b", r + r, r + r + r + "x", r, r + r + r + r, "dos\r", "\r",
	            std::string("nul\0byte", 8)},
	           "invalid UTF-8 becomes U+FFFD; '\\r' and NUL stay");
}

void TestFlushPartialLines() {
	LogCollector log;
	Append(log, Stream::Stderr, "err tail");
	Append(log, Stream::Stdout, "line\nout tail");
	CheckLines(log.TakeNewLines(), {"line"}, "unterminated lines wait");
	log.FlushPartialLines();
	CheckLines(log.TakeNewLines(), {"out tail", "err tail"}, "flushed, standard output first");
	log.FlushPartialLines();
	CheckLines(log.TakeNewLines(), {}, "a second flush adds nothing");

	Append(log, Stream::Stdout, "x\n");
	log.FlushPartialLines();
	CheckLines(log.TakeNewLines(), {"x"}, "empty accumulators add no line");

	Append(log, Stream::Stderr, "\xE6\x97");
	log.FlushPartialLines();
	CheckLines(log.TakeNewLines(), {std::string(kReplacement)},
	           "a flushed partial line is sanitized");

	// After a flush the next bytes start a new line.
	Append(log, Stream::Stdout, "a");
	log.FlushPartialLines();
	Append(log, Stream::Stdout, "b\n");
	CheckLines(log.TakeNewLines(), {"a", "b"}, "bytes after a flush");
}

void TestCapacityEviction() {
	LogCollector log(3);
	Check(log.Lines().Capacity() == 3, "capacity 3");
	Append(log, Stream::Stdout, "1\n2\n");
	CheckLines(log.TakeNewLines(), {"1", "2"}, "two lines");
	Append(log, Stream::Stdout, "3\n4\n5\n6\n7\n");
	CheckLines(Retained(log), {"5", "6", "7"}, "the newest three are retained");
	Check(log.Lines().DroppedCount() == 4, "four lines were dropped");
	CheckLines(log.TakeNewLines(), {"5", "6", "7"},
	           "TakeNewLines after eviction gives only the retained new lines");
	Append(log, Stream::Stderr, "8\n");
	CheckLines(log.TakeNewLines(), {"8"}, "one new line after eviction");
	CheckLines(Retained(log), {"6", "7", "8"}, "retained after one more line");

	LogCollector single(0);
	Check(single.Lines().Capacity() == 1, "a capacity of 0 counts as 1");
	Append(single, Stream::Stdout, "a\nb\n");
	CheckLines(Retained(single), {"b"}, "capacity 1 keeps the newest line");
}

void TestDefaultCapacity() {
	LogCollector log;
	Check(log.Lines().Capacity() == 200'000 && LogCollector::kDefaultRetainedLines == 200'000,
	      "the default capacity is 200,000 lines");
	Check(log.Lines().Capacity() >= 100'000, "at least 100,000 lines (Requirement 10.5)");
	std::string chunk;
	for (int i = 0; i < 200'005; i++) {
		chunk += std::to_string(i);
		chunk += '\n';
	}
	Append(log, Stream::Stdout, chunk);
	Check(log.Lines().Size() == 200'000, "200,000 lines are retained");
	Check(log.Lines()[0] == "5" && log.Lines()[199'999] == "200004",
	      "the oldest five lines were dropped");
	Check(log.Lines().DroppedCount() == 5, "five lines were dropped");
	const auto fresh = log.TakeNewLines();
	Check(fresh.size() == 200'000 && fresh.front() == "5" && fresh.back() == "200004",
	      "TakeNewLines gives the retained lines");
}

void TestToText() {
	LogCollector log;
	Check(log.ToText().empty(), "an empty log has no text");
	Append(log, Stream::Stdout, "first\n\nthird \xFF\n");
	Append(log, Stream::Stderr, "crlf\r\n");
	const std::string text = log.ToText();
	Check(text == "first\n\nthird " + std::string(kReplacement) + "\ncrlf\r\n",
	      "ToText writes each line and '\\n'");

	// Splitting the text on '\n' gives back the retained lines.
	Lines       split;
	std::size_t start = 0;
	for (std::size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\n') {
			split.push_back(text.substr(start, i - start));
			start = i + 1;
		}
	}
	Check(start == text.size(), "the text ends with a newline");
	CheckLines(split, Retained(log), "ToText round trip");

	LogCollector small(2);
	Append(small, Stream::Stdout, "a\nb\nc\n");
	Check(small.ToText() == "b\nc\n", "ToText writes only the retained lines");
}

void TestClear() {
	LogCollector log(4);
	Append(log, Stream::Stdout, "1\n2\n3\n4\n5\nout partial");
	Append(log, Stream::Stderr, "err partial");
	log.Clear();
	Check(log.Lines().Size() == 0 && log.Lines().DroppedCount() == 0 && log.ToText().empty(),
	      "Clear empties the buffer");
	CheckLines(log.TakeNewLines(), {}, "nothing new after Clear");
	log.FlushPartialLines();
	CheckLines(log.TakeNewLines(), {}, "Clear empties both accumulators");
	Append(log, Stream::Stdout, "new\n");
	CheckLines(log.TakeNewLines(), {"new"}, "lines after Clear");
	CheckLines(Retained(log), {"new"}, "retained after Clear");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("LogBufferTests");
	TestRingBuffer();
	TestChunkBoundaries();
	TestInterleavedStreams();
	TestInvalidUtf8AndCarriageReturn();
	TestFlushPartialLines();
	TestCapacityEviction();
	TestDefaultCapacity();
	TestToText();
	TestClear();
	std::printf("LogBufferTests: all tests passed\n");
	return 0;
}
