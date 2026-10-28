#pragma once

// A cancellation flag shared between the thread that requests cancellation (usually the GTK main
// thread) and a worker that polls it (scans, decodes, archive jobs). Cancellation is one-way: once
// set, the flag stays set.

#include <atomic>

namespace kyty::core {

class CancelToken {
public:
	CancelToken()  = default;
	~CancelToken() = default;

	CancelToken(const CancelToken&)            = delete;
	CancelToken& operator=(const CancelToken&) = delete;
	CancelToken(CancelToken&&)                 = delete;
	CancelToken& operator=(CancelToken&&)      = delete;

	// Requests cancellation. Safe to call from any thread, any number of times.
	void Cancel() noexcept { m_cancelled.store(true, std::memory_order_release); }

	// True once Cancel() has been called. Safe to call from any thread.
	[[nodiscard]] bool IsCancelled() const noexcept {
		return m_cancelled.load(std::memory_order_acquire);
	}

private:
	std::atomic<bool> m_cancelled {false};
};

} // namespace kyty::core
