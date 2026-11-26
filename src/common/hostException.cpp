#include "common/hostException.h"

#include <atomic>
#include <cstdio>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#include <ucontext.h> // IWYU pragma: keep

// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::HostException {

static std::atomic<Handler> g_handler {nullptr};
static_assert(decltype(g_handler)::is_always_lock_free);

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX

class ThreadSignalStack {
public:
	ThreadSignalStack() {
		const auto page_size = static_cast<size_t>(::getpagesize());
		const auto stack_size =
		    (std::max<size_t>(64 * 1024, MINSIGSTKSZ) + page_size - 1) & ~(page_size - 1);
		if (::posix_memalign(&m_memory, page_size, stack_size) != 0) {
			return;
		}

		stack_t stack {};
		stack.ss_sp   = m_memory;
		stack.ss_size = stack_size;
		if (::sigaltstack(&stack, &m_previous) != 0) {
			std::free(m_memory);
			m_memory = nullptr;
		}
	}

	~ThreadSignalStack() {
		if (m_memory != nullptr && ::sigaltstack(&m_previous, nullptr) == 0) {
			std::free(m_memory);
		}
	}

	[[nodiscard]] bool IsInitialized() const { return m_memory != nullptr; }

	KYTY_CLASS_NO_COPY(ThreadSignalStack)

private:
	void*   m_memory = nullptr;
	stack_t m_previous {};
};

bool InitializeThreadSignalStack() {
	// Keep fault handling off guest stacks, which GPU tracking can make read-only.
	thread_local ThreadSignalStack signal_stack;
	return signal_stack.IsInitialized();
}

int GuestSignalDispatchSignal() {
	return SIGRTMIN + 3;
}

#endif


// x86-64 page-fault error code: bit 4 = instruction fetch, bit 1 = write, otherwise a read.
static AccessViolationType DecodeAccess(const ucontext_t* context) {
	const auto error = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_ERR]);
	if ((error & 0x10u) != 0) {
		return AccessViolationType::Execute;
	}
	if ((error & 0x2u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}

// A resolved fault returns and retries the instruction; an unresolved one restores the default
// action so the retry terminates the process.
static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
	auto* context = static_cast<ucontext_t*>(native_context);

	ExceptionInfo info {};
	const auto* registers  = context->uc_mcontext.gregs;
	info.exception_address = static_cast<uint64_t>(registers[REG_RIP]);
	info.rax               = static_cast<uint64_t>(registers[REG_RAX]);
	info.rbx               = static_cast<uint64_t>(registers[REG_RBX]);
	info.rcx               = static_cast<uint64_t>(registers[REG_RCX]);
	info.rdx               = static_cast<uint64_t>(registers[REG_RDX]);
	info.rsi               = static_cast<uint64_t>(registers[REG_RSI]);
	info.rdi               = static_cast<uint64_t>(registers[REG_RDI]);
	info.rbp               = static_cast<uint64_t>(registers[REG_RBP]);
	info.rsp               = static_cast<uint64_t>(registers[REG_RSP]);
	info.r8                = static_cast<uint64_t>(registers[REG_R8]);
	info.r9                = static_cast<uint64_t>(registers[REG_R9]);
	info.r10               = static_cast<uint64_t>(registers[REG_R10]);
	info.r11               = static_cast<uint64_t>(registers[REG_R11]);
	info.r12               = static_cast<uint64_t>(registers[REG_R12]);
	info.r13               = static_cast<uint64_t>(registers[REG_R13]);
	info.r14               = static_cast<uint64_t>(registers[REG_R14]);
	info.r15               = static_cast<uint64_t>(registers[REG_R15]);
	info.native_code    = static_cast<uint32_t>(signal_number);
	info.native_context = context;
	if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		info.type                   = ExceptionType::AccessViolation;
		info.access_violation_type  = DecodeAccess(context);
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	}

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return;
	}

	struct sigaction restore {};
	restore.sa_handler = SIG_DFL;
	sigemptyset(&restore.sa_mask);
	::sigaction(signal_number, &restore, nullptr);
}


static bool InstallPlatformHandler() {
	struct sigaction action {};
	action.sa_sigaction = SignalHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
	sigaddset(&action.sa_mask, GuestSignalDispatchSignal());

	// Some protection faults raise SIGBUS; SIGILL routes instructions the host
	// cannot execute to the x64 emulator.
	for (const int signal_number: {SIGSEGV, SIGBUS, SIGILL}) {
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			printf("sigaction(%d) failed\n", signal_number);
			return false;
		}
	}
	return true;
}

// The first handler stays installed; later calls only report whether it is theirs.
bool InstallHandler(Handler handler) {
	static const bool installed = [handler] {
		g_handler.store(handler, std::memory_order_release);
		return InstallPlatformHandler();
	}();
	return installed && g_handler.load(std::memory_order_acquire) == handler;
}

} // namespace Common::HostException
