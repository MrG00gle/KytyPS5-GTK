#pragma once

// A fixed-capacity ring buffer that keeps the most recent values (task 15.5; design.md, "Log
// model"): LogCollector keeps the Emulator's log lines in one.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace kyty::core {

template <typename T>
class RingBuffer {
public:
	// A capacity of 0 counts as 1. Storage grows with the values, up to the capacity.
	explicit RingBuffer(std::size_t capacity): m_capacity(capacity == 0 ? 1 : capacity) {}

	[[nodiscard]] std::size_t Capacity() const { return m_capacity; }
	[[nodiscard]] std::size_t Size() const { return m_values.size(); }
	[[nodiscard]] bool        Empty() const { return m_values.empty(); }

	// Appends value; when the buffer is full, the oldest value is dropped first.
	void PushBack(T value) {
		if (m_values.size() < m_capacity) {
			m_values.push_back(std::move(value));
			return;
		}
		m_values[m_head] = std::move(value);
		m_head           = (m_head + 1) % m_capacity;
		m_dropped++;
	}

	// The value at index from the oldest (0) to the newest (Size() - 1); index must be below
	// Size().
	[[nodiscard]] const T& operator[](std::size_t index) const {
		return m_values[(m_head + index) % m_values.size()];
	}

	// Removes every value; the capacity stays and DroppedCount() starts again at 0.
	void Clear() {
		m_values.clear();
		m_head    = 0;
		m_dropped = 0;
	}

	// How many values PushBack dropped since construction or the last Clear().
	[[nodiscard]] uint64_t DroppedCount() const { return m_dropped; }

private:
	std::size_t    m_capacity;
	std::vector<T> m_values;
	std::size_t    m_head    = 0; // the index of the oldest value once the buffer is full
	uint64_t       m_dropped = 0;
};

} // namespace kyty::core
