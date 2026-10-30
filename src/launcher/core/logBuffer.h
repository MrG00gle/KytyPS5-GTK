#pragma once

// The Emulator's log (task 15.5; design.md, "Log model"; Requirements 10.5, 10.7, 10.8): the lines
// that the Emulator writes to standard output and standard error, as the log view shows them and
// "Save log" writes them.

#include "launcher/core/ringBuffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace kyty::core {

enum class Stream { Stdout, Stderr };

// Splits each stream's bytes into lines and keeps the most recent ones:
// - a line ends at '\n' only ("\r" stays in the line); each stream has its own partial-line
//   accumulator, so chunks may end anywhere, also inside a UTF-8 sequence, and the lines of each
//   stream keep their order;
// - a complete line is stored without its '\n' after SanitizeUtf8 (text.h), so each ill-formed
//   UTF-8 sequence becomes U+FFFD; a line has no length limit;
// - the buffer keeps the newest retained_lines lines; older lines are dropped.
class LogCollector {
public:
	// The default capacity; Requirement 10.5 asks for at least 100,000 lines.
	static constexpr std::size_t kDefaultRetainedLines = 200'000;

	// A capacity of 0 counts as 1.
	explicit LogCollector(std::size_t retained_lines = kDefaultRetainedLines);

	// Adds bytes that the stream delivered.
	void Append(Stream stream, std::span<const std::byte> bytes);

	// At exit (Requirement 10.7): each non-empty accumulator becomes a line, standard output's
	// first; both accumulators are then empty.
	void FlushPartialLines();

	// The lines added since the previous call (or since construction or Clear()), oldest first: at
	// most Capacity() of them, because older ones were already dropped. The log view appends
	// them and removes the rows that the buffer dropped.
	std::vector<std::string> TakeNewLines();

	[[nodiscard]] const RingBuffer<std::string>& Lines() const { return m_lines; }

	// Every retained line followed by '\n', as "Save log" writes the file (Requirement 10.8):
	// splitting the text on '\n' gives back exactly Lines().
	[[nodiscard]] std::string ToText() const;

	// Empties the buffer and both accumulators (a new run clears the log, Requirement 10.1).
	void Clear();

private:
	void AddLine(const std::string& raw);

	RingBuffer<std::string>    m_lines;
	std::array<std::string, 2> m_partial;   // indexed by Stream: Stdout, Stderr
	uint64_t                   m_added = 0; // lines added since construction or Clear()
	uint64_t                   m_taken = 0; // m_added at the last TakeNewLines()
};

} // namespace kyty::core
