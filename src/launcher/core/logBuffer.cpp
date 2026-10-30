#include "launcher/core/logBuffer.h"

#include "launcher/core/text.h"

#include <algorithm>
#include <cstring>

namespace kyty::core {

namespace {

std::size_t Index(Stream stream) {
	return stream == Stream::Stdout ? 0 : 1;
}

} // namespace

LogCollector::LogCollector(std::size_t retained_lines): m_lines(retained_lines) {}

void LogCollector::AddLine(const std::string& raw) {
	m_lines.PushBack(SanitizeUtf8(raw));
	m_added++;
}

void LogCollector::Append(Stream stream, std::span<const std::byte> bytes) {
	auto&       partial = m_partial[Index(stream)];
	const char* data    = reinterpret_cast<const char*>(bytes.data());
	std::size_t size    = bytes.size();
	while (size > 0) {
		const auto* newline = static_cast<const char*>(std::memchr(data, '\n', size));
		if (newline == nullptr) {
			partial.append(data, size);
			return;
		}
		const auto length = static_cast<std::size_t>(newline - data);
		partial.append(data, length);
		AddLine(partial);
		partial.clear();
		data += length + 1;
		size -= length + 1;
	}
}

void LogCollector::FlushPartialLines() {
	for (auto& partial: m_partial) {
		if (!partial.empty()) {
			AddLine(partial);
			partial.clear();
		}
	}
}

std::vector<std::string> LogCollector::TakeNewLines() {
	const auto fresh = static_cast<std::size_t>(
	    std::min<uint64_t>(m_added - m_taken, static_cast<uint64_t>(m_lines.Size())));
	std::vector<std::string> lines;
	lines.reserve(fresh);
	for (std::size_t i = m_lines.Size() - fresh; i < m_lines.Size(); i++) {
		lines.push_back(m_lines[i]);
	}
	m_taken = m_added;
	return lines;
}

std::string LogCollector::ToText() const {
	std::size_t total = 0;
	for (std::size_t i = 0; i < m_lines.Size(); i++) {
		total += m_lines[i].size() + 1;
	}
	std::string text;
	text.reserve(total);
	for (std::size_t i = 0; i < m_lines.Size(); i++) {
		text += m_lines[i];
		text += '\n';
	}
	return text;
}

void LogCollector::Clear() {
	m_lines.Clear();
	for (auto& partial: m_partial) {
		partial.clear();
	}
	m_added = 0;
	m_taken = 0;
}

} // namespace kyty::core
