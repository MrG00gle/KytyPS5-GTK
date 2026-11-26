#ifndef KYTY_COMMON_THREADS_H_
#define KYTY_COMMON_THREADS_H_

#include "common/common.h"

#include <condition_variable>
#include <mutex>

namespace Common {

void InitializeThreads();

using wait_poll_func_t = void (*)();

namespace Thread {

void SleepMicro(uint32_t micros);
void SleepNano(uint64_t nanos);
bool IsMainThread();
// Unique per thread; never reused by another thread.
int GetThreadIdUnique();

} // namespace Thread

// Recursive.
class Mutex {
public:
	void Lock() { m_mutex.lock(); }
	void Unlock() { m_mutex.unlock(); }

private:
	std::recursive_mutex m_mutex;

	friend class CondVar;
};

class CondVar {
public:
	CondVar() = default;

	void Wait(Mutex* mutex);
	bool WaitFor(Mutex* mutex, uint32_t micros);
	void Signal() { m_cv.notify_one(); }
	void SignalAll() { m_cv.notify_all(); }

	static void SetWaitPollCallback(wait_poll_func_t callback);

	KYTY_CLASS_NO_COPY(CondVar);

private:
	std::condition_variable_any m_cv;
};

class LockGuard {
public:
	using mutex_type = Mutex;

	// NOLINTNEXTLINE(google-runtime-references)
	explicit LockGuard(mutex_type& m): m_mutex(m) { m_mutex.Lock(); }

	~LockGuard() { m_mutex.Unlock(); }

	KYTY_CLASS_NO_COPY(LockGuard);

private:
	mutex_type& m_mutex;
};

} // namespace Common

#endif /* KYTY_COMMON_THREADS_H_ */
