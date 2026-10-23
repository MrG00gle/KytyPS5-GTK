// SSE4a, VRSQRTPS and MWAITX emulation on Linux. Every reference result comes from calling the
// instruction emulator directly with a synthetic ucontext_t, so no case depends on whether the host
// CPU executes SSE4a natively. Patched code runs natively only as native trampolines or as
// trap-marked encodings that raise #UD on every host: the EXTRQ trampolines, and the VRSQRTPS
// trampoline and trap mark, whose blobs are AVX code.
#include "common/hostException.h"
#include "loader/guestInstructionPatcher.h"
#include "loader/x64InstructionEmulator.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

namespace {

using Common::HostException::ExceptionInfo;
using Common::HostException::ExceptionType;

// The SSE4a bytes are the point of this test, so blobs are spelled out byte by
// byte instead of going through an encoder. Every blob is a complete routine:
// the instruction under test, a store of the result, and ret. Encodings were
// disassembled to confirm the operand roles: EXTRQ immediate is unary
// (ModRM.r/m is both source and destination, the reg field is unused),
// INSERTQ immediate reads ModRM.r/m into the reg field.
constexpr uint8_t kExtrqXmm2Len40Index0[] = {
	0x66, 0x0f, 0x78, 0xc2, 0x28, 0x00, // extrq xmm2, 40, 0
	0xf3, 0x0f, 0x7f, 0x17,             // movdqu %xmm2, (%rdi)
	0xc3,                               // ret
};

constexpr uint8_t kExtrqXmm2Len64Index0[] = {
	0x66, 0x0f, 0x78, 0xc2, 0x40, 0x00, // extrq xmm2, 64, 0
	0xf3, 0x0f, 0x7f, 0x17,             // movdqu %xmm2, (%rdi)
	0xc3,                               // ret
};

constexpr uint8_t kInsertqXmm2Xmm0Len32Index0[] = {
	0xf2, 0x0f, 0x78, 0xd0, 0x20, 0x00, // insertq xmm2, xmm0, 32, 0
	0xf3, 0x0f, 0x7f, 0x17,             // movdqu %xmm2, (%rdi)
	0xc3,                               // ret
};

// REX.B moves the operand into the high register file. xmm10 is not an argument
// register, so the blob loads it from memory. EXTRQ has a single operand, so
// only the destination needs loading.
constexpr uint8_t kExtrqXmm10Len40Index0[] = {
	0xf3, 0x44, 0x0f, 0x6f, 0x16,             // movdqu (%rsi), %xmm10
	0x66, 0x41, 0x0f, 0x78, 0xc2, 0x28, 0x00, // extrq xmm10, 40, 0
	0xf3, 0x44, 0x0f, 0x7f, 0x17,             // movdqu %xmm10, (%rdi)
	0xc3,                                     // ret
};

// REX.R keeps the destination in xmm10 while REX.B puts the source in xmm9, and
// the inserted field sits at a nonzero index.
constexpr uint8_t kInsertqXmm10Xmm9Len16Index8[] = {
	0xf3, 0x44, 0x0f, 0x6f, 0x0e,             // movdqu (%rsi), %xmm9
	0xf3, 0x44, 0x0f, 0x6f, 0x12,             // movdqu (%rdx), %xmm10
	0xf2, 0x45, 0x0f, 0x78, 0xd1, 0x10, 0x08, // insertq xmm10, xmm9, 16, 8
	0xf3, 0x44, 0x0f, 0x7f, 0x17,             // movdqu %xmm10, (%rdi)
	0xc3,                                     // ret
};

constexpr uint8_t kInsertqXmm10Xmm9[] = {
	0xf3, 0x44, 0x0f, 0x6f, 0x0e, // movdqu (%rsi), %xmm9
	0xf3, 0x44, 0x0f, 0x6f, 0x12, // movdqu (%rdx), %xmm10
	0xf2, 0x45, 0x0f, 0x79, 0xd1, // insertq xmm10, xmm9
	0xf3, 0x44, 0x0f, 0x7f, 0x17, // movdqu %xmm10, (%rdi)
	0xc3,
};

constexpr uint8_t kInsertqXmm9Xmm9[] = {
	0xf3, 0x44, 0x0f, 0x6f, 0x0e, // movdqu (%rsi), %xmm9
	0xf2, 0x45, 0x0f, 0x79, 0xc9, // insertq xmm9, xmm9
	0xf3, 0x44, 0x0f, 0x7f, 0x0f, // movdqu %xmm9, (%rdi)
	0xc3,
};

constexpr uint8_t kUd2[] = {
	0x0f, 0x0b, // ud2
	0xc3,       // ret
};

// VRSQRTPS xmm1, xmm0 as the AMD CPU patcher leaves it when it cannot relocate the instruction: a
// reserved VEX.vvvv bit is cleared (c5 f8 -> c5 f0), which raises #UD on every host.
constexpr uint8_t kTrappedVrsqrtpsXmm1Xmm0[] = {
	0x0f, 0x10, 0x06,                         // movups (%rsi), %xmm0
	0xc5, 0xfc, 0x10, 0x0e,                   // vmovups ymm1, [rsi]: dirty the upper half of ymm1
	0xc5, 0xf0, 0x52, 0xc8,                   // vrsqrtps xmm1, xmm0 with the vvvv trap mark
	0x0f, 0x11, 0x0f,                         // movups %xmm1, (%rdi)
	0xc4, 0xe3, 0x7d, 0x19, 0x4f, 0x10, 0x01, // vextractf128 [rdi + 16], ymm1, 1
	0xc5, 0xf8, 0x77,                         // vzeroupper
	0xc3,                                     // ret
};

// The same sequence with the ordinary VEX.128 VRSQRTPS encoding (vvvv = 1111), as it appears in guest
// code before the patcher marks it as a trap.
constexpr uint8_t kPlainVrsqrtpsXmm1Xmm0[] = {
	0x0f, 0x10, 0x06,                         // movups (%rsi), %xmm0
	0xc5, 0xfc, 0x10, 0x0e,                   // vmovups ymm1, [rsi]: dirty the upper half of ymm1
	0xc5, 0xf8, 0x52, 0xc8,                   // vrsqrtps xmm1, xmm0
	0x0f, 0x11, 0x0f,                         // movups %xmm1, (%rdi)
	0xc4, 0xe3, 0x7d, 0x19, 0x4f, 0x10, 0x01, // vextractf128 [rdi + 16], ymm1, 1
	0xc5, 0xf8, 0x77,                         // vzeroupper
	0xc3,                                     // ret
};

// The same again behind a never taken indirect branch. A function with an indirect branch does not
// let the patcher relocate neighbours, and the 4-byte VRSQRTPS cannot hold a jump on its own, so it
// is left in place with the trap mark.
constexpr uint8_t kPlainVrsqrtpsIndirectBranch[] = {
	0x31, 0xc0,                               // xor %eax, %eax
	0x85, 0xc0,                               // test %eax, %eax
	0x74, 0x02,                               // jz over
	0xff, 0xe0,                               // jmp *%rax (never taken)
	0x0f, 0x10, 0x06,                         // movups (%rsi), %xmm0
	0xc5, 0xfc, 0x10, 0x0e,                   // vmovups ymm1, [rsi]: dirty the upper half of ymm1
	0xc5, 0xf8, 0x52, 0xc8,                   // vrsqrtps xmm1, xmm0
	0x0f, 0x11, 0x0f,                         // movups %xmm1, (%rdi)
	0xc4, 0xe3, 0x7d, 0x19, 0x4f, 0x10, 0x01, // vextractf128 [rdi + 16], ymm1, 1
	0xc5, 0xf8, 0x77,                         // vzeroupper
	0xc3,                                     // ret
};

constexpr uint8_t kMwaitx[] = {
	0x0f, 0x01, 0xfb, // mwaitx
	0xc3,             // ret
};

// VRSQRTPS inputs: xmm0 receives the first four words, and the last four dirty the upper half of
// ymm1, which the VEX.128 form must clear.
constexpr uint32_t kRsqrtInput[8] {std::bit_cast<uint32_t>(4.0f), 0x00000000u, 0xbf800000u, 0x7f800000u,
                                   0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};

constexpr size_t kPageSize = 4096;

std::atomic<int> g_illegal_hits {0};

static_assert(decltype(g_illegal_hits)::is_always_lock_free);

void Check(bool value, const char *text) {
	if (!value) {
		std::fprintf(stderr, "sse4a_emulation: failed: %s\n", text);
		std::abort();
	}
}

// Counts the SIGILLs raised while patched code runs natively. The emulator still handles the
// instruction, so a failing case reaches its check instead of faulting again.
bool Handler(const ExceptionInfo &info) {
	if (info.type != ExceptionType::IllegalInstruction) {
		return false;
	}
	g_illegal_hits.fetch_add(1, std::memory_order_relaxed);
	if (Loader::X64InstructionEmulator::TryEmulate(info.native_context)) {
		return true;
	}
	// Returning after a refusal would re-run the faulting instruction forever, so end the run. Name
	// the refused instruction bytes so the failure can be told apart from a missing host feature.
	static const char message[] = "sse4a_emulation: failed: unexpected emulator refusal at bytes";
	static const char digits[]  = "0123456789abcdef";
	char              bytes[8 * 3 + 1] {};
	const auto       *code = reinterpret_cast<const uint8_t *>(info.exception_address);
	for (size_t i = 0; i < 8; ++i) {
		bytes[i * 3]     = ' ';
		bytes[i * 3 + 1] = digits[code[i] >> 4];
		bytes[i * 3 + 2] = digits[code[i] & 0xf];
	}
	bytes[8 * 3] = '\n';
	::write(STDERR_FILENO, message, sizeof(message) - 1);
	::write(STDERR_FILENO, bytes, sizeof(bytes));
	::_exit(1);
}

int IllegalHits() {
	return g_illegal_hits.load(std::memory_order_relaxed);
}

template <typename Fn, size_t N>
Fn MapCode(const uint8_t (&code)[N]) {
	void *page = ::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	Check(page != MAP_FAILED, "map test code page");
	std::memcpy(page, code, N);
	Check(::mprotect(page, kPageSize, PROT_READ | PROT_EXEC) == 0, "make test code page executable");
	return reinterpret_cast<Fn>(page);
}

using XmmValue = std::array<uint64_t, 2>;
using YmmWords = std::array<uint32_t, 8>;

// The signal context the Linux kernel hands to a SIGILL handler: general registers in
// uc_mcontext.gregs and an XSAVE area behind uc_mcontext.fpregs. Instruction bytes are copied into a
// zero-padded buffer, so the decoders may read a full instruction length past the end of any blob.
struct SyntheticState {
	ucontext_t          context {};
	alignas(64) uint8_t xsave[1024] {};
	alignas(16) uint8_t code[96] {};

	SyntheticState() { context.uc_mcontext.fpregs = reinterpret_cast<fpregset_t>(xsave); }
	SyntheticState(const SyntheticState &)            = delete;
	SyntheticState &operator=(const SyntheticState &) = delete;

	// Copies the blob and points RIP at the instruction that starts at offset.
	template <size_t N>
	void LoadCode(const uint8_t (&blob)[N], size_t offset) {
		static_assert(N + 16 <= sizeof(code), "the blob does not fit the padded code buffer");
		std::memcpy(code, blob, N);
		SetRip(code + offset);
	}
	void SetRip(const void *address) {
		context.uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(reinterpret_cast<uintptr_t>(address));
	}
	[[nodiscard]] uintptr_t Rip() const { return static_cast<uintptr_t>(context.uc_mcontext.gregs[REG_RIP]); }

	[[nodiscard]] uint8_t *XmmBytes(uint8_t index) {
		return reinterpret_cast<uint8_t *>(reinterpret_cast<_libc_fpstate *>(xsave)->_xmm[index].element);
	}
	void SetXmm(uint8_t index, const XmmValue &value) { std::memcpy(XmmBytes(index), value.data(), 16); }
	[[nodiscard]] XmmValue Xmm(uint8_t index) {
		XmmValue value {};
		std::memcpy(value.data(), XmmBytes(index), 16);
		return value;
	}

	// Marks the AVX component as present in the XSAVE header, as the kernel does for a thread that
	// has used YMM state, so the emulator can clear YMM[255:128] of a VEX.128 destination.
	void EnableAvxState() {
		const uint32_t magic    = 0x46505853; // FP_XSTATE_MAGIC1
		const uint32_t size     = sizeof(xsave);
		const uint64_t features = 0x7; // x87, SSE and AVX
		std::memcpy(xsave + 464, &magic, sizeof(magic));
		std::memcpy(xsave + 480, &size, sizeof(size));
		std::memcpy(xsave + 512, &features, sizeof(features));
	}
	[[nodiscard]] uint8_t *YmmHighBytes(uint8_t index) { return xsave + 576 + index * 16; }
};

// Emulates the instruction at RIP and checks that RIP moved past it.
void Emulate(SyntheticState &state, size_t length, const char *text) {
	const uintptr_t rip = state.Rip();
	Check(Loader::X64InstructionEmulator::TryEmulate(&state.context), text);
	Check(state.Rip() == rip + length, "TryEmulate advances RIP by the instruction length");
}

// Checks that the instruction at RIP is refused and RIP stays on it.
void CheckRefused(SyntheticState &state, const char *text) {
	const uintptr_t rip = state.Rip();
	Check(!Loader::X64InstructionEmulator::TryEmulate(&state.context), text);
	Check(state.Rip() == rip, "a refused instruction leaves RIP unchanged");
}

// Blobs receive their inputs through the SysV ABI, so the xmm argument registers
// are reachable as ordinary function parameters.
using BlobFn     = void (*)(uint64_t *out, double xmm0, double xmm1, double xmm2);
using LoadBlobFn = void (*)(uint64_t *out, const uint64_t *in);
using PairBlobFn = void (*)(uint64_t *out, const uint64_t *src, const uint64_t *dest);

void TestRefusals() {
	Check(!Loader::X64InstructionEmulator::TryEmulate(nullptr), "TryEmulate refuses a null native context");
	// A Linux signal frame without FP state has no XMM registers to emulate against.
	SyntheticState state;
	state.LoadCode(kExtrqXmm2Len40Index0, 0);
	state.context.uc_mcontext.fpregs = nullptr;
	CheckRefused(state, "TryEmulate refuses a saved context without FP state");
}

void TestExtrqImmediate() {
	SyntheticState state;
	state.LoadCode(kExtrqXmm2Len40Index0, 0);
	// The pattern is in xmm2; the emulator keeps bits [39:0] and zeroes the upper half.
	state.SetXmm(2, {0xaabbccddeeff1122ull, 0});
	Emulate(state, 6, "TryEmulate handles EXTRQ");
	const auto out = state.Xmm(2);
	Check(out[0] == 0x000000ddeeff1122ull, "extrq xmm2, 40, 0 keeps bits [39:0] of xmm2");
	Check(out[1] == 0, "extrq xmm2, 40, 0 zero-extends the upper 64 bits");
}

void TestExtrqFullWidth() {
	SyntheticState state;
	state.LoadCode(kExtrqXmm2Len64Index0, 0);
	state.SetXmm(2, {0x0123456789abcdefull, 0});
	Emulate(state, 6, "TryEmulate handles a full width EXTRQ");
	const auto out = state.Xmm(2);
	Check(out[0] == 0x0123456789abcdefull, "extrq xmm2, 64, 0 keeps the whole low 64 bits");
	Check(out[1] == 0, "extrq xmm2, 64, 0 zero-extends the upper 64 bits");
}

void TestInsertqImmediate() {
	SyntheticState state;
	state.LoadCode(kInsertqXmm2Xmm0Len32Index0, 0);
	state.SetXmm(0, {0x1122334455667788ull, 0});
	state.SetXmm(2, {0x0123456789abcdefull, 0});
	Emulate(state, 6, "TryEmulate handles INSERTQ");
	Check(state.Xmm(2)[0] == 0x0123456755667788ull, "insertq xmm2, xmm0, 32, 0 inserts bits [31:0] of xmm0");
}

void TestRexHighRegisterExtrq() {
	SyntheticState state;
	state.LoadCode(kExtrqXmm10Len40Index0, 5);
	state.SetXmm(10, {0xaabbccddeeff1122ull, 0});
	Emulate(state, 7, "TryEmulate handles a REX.B EXTRQ");
	const auto out = state.Xmm(10);
	Check(out[0] == 0x000000ddeeff1122ull, "REX.B destination xmm10 is emulated through the saved context");
	Check(out[1] == 0, "REX.B destination xmm10 zero-extends the upper 64 bits");
}

void TestRexHighRegisterInsertqWithIndex() {
	SyntheticState state;
	state.LoadCode(kInsertqXmm10Xmm9Len16Index8, 10);
	state.SetXmm(9, {0x8877665544332211ull, 0xdeadbeefcafebabeull});
	state.SetXmm(10, {0xaaaaaaaaaaaaaaaaull, 0xbbbbbbbbbbbbbbbbull});
	Emulate(state, 7, "TryEmulate handles a REX.R/REX.B INSERTQ");
	const auto out = state.Xmm(10);
	Check(out[0] == 0xaaaaaaaaaa2211aaull, "insertq xmm10, xmm9, 16, 8 inserts bits [15:0] at index 8");
	Check(out[1] == 0xbbbbbbbbbbbbbbbbull, "insertq xmm10, xmm9, 16, 8 leaves the upper 64 bits untouched");
}

// Register INSERTQ at offset site of blob, writing xmm dest from source xmm9. The aliased form uses
// xmm9 as both operands.
template <size_t N>
void CheckRegisterInsertq(const uint8_t (&blob)[N], size_t site, uint8_t dest, bool aliased) {
	for (const uint64_t controls: {0xc4c8ull, 0xc0c0ull, 0xffc1ull}) {
		const uint64_t source[2] {0x0123456789abcdefull, 0xdeadbeef00000000ull | controls};
		const uint64_t destination[2] {0x8877665544332211ull, 0xfedcba9876543210ull};
		const auto*    initial  = aliased ? source : destination;
		uint64_t       expected = initial[0];
		const auto     length   = (controls & 63) == 0 ? 64 : controls & 63;
		const auto     index    = (controls >> 8) & 63;
		for (uint64_t bit = 0; bit < length && index + bit < 64; ++bit) {
			const uint64_t mask = uint64_t {1} << (index + bit);
			expected = (expected & ~mask) | (((source[0] >> bit) & 1) << (index + bit));
		}
		SyntheticState state;
		state.LoadCode(blob, site);
		state.SetXmm(9, {source[0], source[1]});
		if (!aliased) {
			state.SetXmm(10, {destination[0], destination[1]});
		}
		Emulate(state, 5, "TryEmulate handles register INSERTQ");
		const auto out = state.Xmm(dest);
		Check(out[0] == expected && out[1] == initial[1],
		      "register INSERTQ uses upper source controls and preserves upper destination");
	}
}

void TestInsertqRegister() {
	CheckRegisterInsertq(kInsertqXmm10Xmm9, 10, 10, false);
	CheckRegisterInsertq(kInsertqXmm9Xmm9, 5, 9, true);
}

void TestMwaitxEmulated() {
	SyntheticState state;
	state.LoadCode(kMwaitx, 0);
	Emulate(state, 3, "TryEmulate handles MWAITX");
}

void TestUnknownInstructionRefused() {
	SyntheticState state;
	state.LoadCode(kUd2, 0);
	CheckRefused(state, "TryEmulate refuses an unknown instruction");
}

// Runs VRSQRTPS xmm1, xmm0 at RIP with the registers the blobs load before it (xmm0 = in[0..3] and
// ymm1 = in[0..7]) and returns ymm1 as the blobs store it to out[0..7].
YmmWords EmulateReciprocalSquareRoot(SyntheticState &state, const char *text) {
	state.EnableAvxState();
	std::memcpy(state.XmmBytes(0), kRsqrtInput, 16);
	std::memcpy(state.XmmBytes(1), kRsqrtInput, 16);
	std::memcpy(state.YmmHighBytes(1), kRsqrtInput + 4, 16);
	Emulate(state, 4, text);
	YmmWords out {};
	std::memcpy(out.data(), state.XmmBytes(1), 16);
	std::memcpy(out.data() + 4, state.YmmHighBytes(1), 16);
	return out;
}

void CheckExactReciprocalSquareRoot(const YmmWords &out) {
	Check(std::bit_cast<float>(out[0]) == 0.5f && out[1] == 0x7f800000u && out[2] == 0xffc00000u && out[3] == 0,
	      "VRSQRTPS produces the exact reciprocal square roots");
	Check(out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 0, "VEX.128 VRSQRTPS zeroes the upper half");
}

// The pre-marked VRSQRTPS is the reference for the patched copies below.
YmmWords TestTrappedReciprocalSquareRoot() {
	SyntheticState state;
	state.LoadCode(kTrappedVrsqrtpsXmm1Xmm0, 7);
	const auto out = EmulateReciprocalSquareRoot(state, "TryEmulate handles the pre-marked VRSQRTPS");
	CheckExactReciprocalSquareRoot(out);
	return out;
}

// The loader rewrites SSE4a instructions into native trampolines when the patcher is enabled. The
// trampoline area lives right behind the code page, like it does behind a mapped guest module.
struct PatchedCode {
	void                               *code = nullptr;
	Loader::GuestInstructionPatchResult result {};
};

template <size_t N>
PatchedCode MapPatchedCode(const uint8_t (&blob)[N], bool emulate_amd) {
	void *area = ::mmap(nullptr, 2 * kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
	                    MAP_PRIVATE | MAP_ANON, -1, 0);
	Check(area != MAP_FAILED, "map RWX code and trampoline area");
	std::memcpy(area, blob, N);
	auto *trampoline = static_cast<uint8_t *>(area) + kPageSize;
	Loader::RegisterGuestInstructionPatchModule(area, kPageSize, trampoline, kPageSize);
	const std::array<uintptr_t, 1> function_starts {reinterpret_cast<uintptr_t>(area)};
	// A host without SSE4a, RDPID and CLWB.
	const Loader::GuestInstructionHostFeatures host {false, false, false};
	PatchedCode patched;
	patched.code   = area;
	patched.result = Loader::PatchGuestInstructions(reinterpret_cast<uintptr_t>(area), N, function_starts,
	                                                false, emulate_amd, host);
	Loader::UnregisterGuestInstructionPatchModule(area);
	return patched;
}

using PatchCounts = Loader::InstructionPatchCounts Loader::GuestInstructionPatchResult::*;

template <size_t N>
void CheckUnpatched(const uint8_t (&blob)[N], PatchCounts counts, const char *text) {
	const auto unpatched = MapPatchedCode(blob, false);
	Check((unpatched.result.*counts).found == 0, text);
	Check(std::memcmp(unpatched.code, blob, N) == 0, "the patcher leaves the code unchanged without emulate_amd");
}

// EXTRQ becomes a native trampoline, which runs on every host without a single SIGILL and must
// produce the emulated result.
template <typename Fn, size_t N, typename Invoke>
void CheckPatchedNative(const uint8_t (&blob)[N], const XmmValue &reference, Invoke invoke, const char *text) {
	CheckUnpatched(blob, &Loader::GuestInstructionPatchResult::extrq, "patcher stays off without emulate_amd");
	const auto  patched = MapPatchedCode(blob, true);
	const auto &counts  = patched.result.extrq;
	Check(counts.found == 1, text);
	Check(counts.native == 1, "native trampoline count");
	Check(counts.trapped == 0, "trapped (signal path) count");
	uint64_t  out[2] {};
	const int before = IllegalHits();
	invoke(reinterpret_cast<Fn>(patched.code), out);
	Check(IllegalHits() == before, "patched code raises no SIGILL");
	Check(out[0] == reference[0] && out[1] == reference[1], "patched result equals emulated result");
}

// Returns the only copy of the instruction bytes in the patched code page and its trampoline area.
const uint8_t *FindPatchedInstruction(const void *area, const uint8_t *instruction, size_t length) {
	const auto    *bytes = static_cast<const uint8_t *>(area);
	const uint8_t *found = nullptr;
	int            count = 0;
	for (size_t i = 0; i + length <= 2 * kPageSize; ++i) {
		if (std::memcmp(bytes + i, instruction, length) == 0) {
			found = bytes + i;
			++count;
		}
	}
	Check(count == 1, "the patched code holds exactly one copy of the trapped instruction");
	return found;
}

// INSERTQ is marked as a trap: the patcher keeps its encoding, either at the original site or
// relocated into the trampoline area, where it raises #UD on a host without SSE4a. It is not run
// natively, because a host with SSE4a would execute it without the emulator. The emulator's result
// on the patched copy must equal the reference.
template <size_t N, typename Setup>
void CheckPatchedTrap(const uint8_t (&blob)[N], size_t site, size_t length, uint8_t dest, Setup setup,
                      const char *text) {
	SyntheticState reference_state;
	reference_state.LoadCode(blob, site);
	setup(reference_state);
	Emulate(reference_state, length, "TryEmulate handles INSERTQ");
	const auto reference = reference_state.Xmm(dest);

	CheckUnpatched(blob, &Loader::GuestInstructionPatchResult::insertq, "patcher stays off without emulate_amd");
	const auto  patched = MapPatchedCode(blob, true);
	const auto &counts  = patched.result.insertq;
	Check(counts.found == 1, text);
	Check(counts.native == 0, "native trampoline count");
	Check(counts.trapped == 1, "trapped (signal path) count");
	const auto *trap = FindPatchedInstruction(patched.code, blob + site, length);

	SyntheticState state;
	state.SetRip(trap);
	setup(state);
	Emulate(state, length, "TryEmulate handles the trapped INSERTQ in the patched code");
	Check(state.Xmm(dest) == reference, "patched result equals emulated result");
}

void TestPatchedExtrqImmediate() {
	SyntheticState state;
	state.LoadCode(kExtrqXmm2Len40Index0, 0);
	state.SetXmm(2, {0xaabbccddeeff1122ull, 0});
	Emulate(state, 6, "TryEmulate handles EXTRQ");
	CheckPatchedNative<BlobFn>(
	    kExtrqXmm2Len40Index0, state.Xmm(2),
	    [](BlobFn fn, uint64_t *out) { fn(out, 0.0, 0.0, std::bit_cast<double>(0xaabbccddeeff1122ull)); },
	    "EXTRQ immediate found by the patcher");
}

void TestPatchedExtrqRexHigh() {
	const uint64_t in[2] {0xaabbccddeeff1122ull, 0x5555555555555555ull};
	SyntheticState state;
	state.LoadCode(kExtrqXmm10Len40Index0, 5);
	state.SetXmm(10, {in[0], in[1]});
	Emulate(state, 7, "TryEmulate handles a REX.B EXTRQ");
	CheckPatchedNative<LoadBlobFn>(
	    kExtrqXmm10Len40Index0, state.Xmm(10), [&in](LoadBlobFn fn, uint64_t *out) { fn(out, in); },
	    "REX EXTRQ found by the patcher");
}

void TestPatchedInsertqImmediate() {
	CheckPatchedTrap(
	    kInsertqXmm2Xmm0Len32Index0, 0, 6, 2,
	    [](SyntheticState &state) {
		    state.SetXmm(0, {0x1122334455667788ull, 0});
		    state.SetXmm(2, {0x0123456789abcdefull, 0});
	    },
	    "INSERTQ immediate found by the patcher");
}

void TestPatchedInsertqIndexedRexHigh() {
	CheckPatchedTrap(
	    kInsertqXmm10Xmm9Len16Index8, 10, 7, 10,
	    [](SyntheticState &state) {
		    state.SetXmm(9, {0x8877665544332211ull, 0xdeadbeefcafebabeull});
		    state.SetXmm(10, {0xaaaaaaaaaaaaaaaaull, 0xbbbbbbbbbbbbbbbbull});
	    },
	    "indexed REX INSERTQ found by the patcher");
}

void TestPatchedInsertqRegister() {
	CheckPatchedTrap(
	    kInsertqXmm10Xmm9, 10, 5, 10,
	    [](SyntheticState &state) {
		    state.SetXmm(9, {0x0123456789abcdefull, 0xdeadbeef0000c4c8ull});
		    state.SetXmm(10, {0x8877665544332211ull, 0xfedcba9876543210ull});
	    },
	    "register INSERTQ found by the patcher");
}

template <size_t N>
size_t FindPlainVrsqrtps(const uint8_t (&blob)[N]) {
	size_t site = 0;
	while (site + 2 < N && !(blob[site] == 0xc5 && blob[site + 1] == 0xf8 && blob[site + 2] == 0x52)) {
		++site;
	}
	Check(site + 2 < N, "locate VRSQRTPS in the test blob");
	return site;
}

// The ordinary VEX.128 VRSQRTPS is relocated into a native trampoline.
void *TestPatchedReciprocalSquareRootNative() {
	CheckUnpatched(kPlainVrsqrtpsXmm1Xmm0, &Loader::GuestInstructionPatchResult::reciprocal_sqrt,
	               "patcher leaves VRSQRTPS alone without emulate_amd");
	const auto  patched = MapPatchedCode(kPlainVrsqrtpsXmm1Xmm0, true);
	const auto &counts  = patched.result.reciprocal_sqrt;
	Check(counts.found == 1, "patcher finds the ordinary VEX.128 VRSQRTPS");
	Check(counts.native == 1 && counts.trapped == 0, "VRSQRTPS native trampoline / trap count");
	const auto  *code = static_cast<const uint8_t *>(patched.code);
	const size_t site = FindPlainVrsqrtps(kPlainVrsqrtpsXmm1Xmm0);
	Check(code[site] != 0xc5 || code[site + 1] != 0xf8, "native trampoline replaced the VRSQRTPS");
	return patched.code;
}

// Behind an indirect branch the VRSQRTPS stays in place with the trap mark, and the emulator handles
// it at the patched site.
void *TestPatchedReciprocalSquareRootTrap(const YmmWords &reference) {
	CheckUnpatched(kPlainVrsqrtpsIndirectBranch, &Loader::GuestInstructionPatchResult::reciprocal_sqrt,
	               "patcher leaves VRSQRTPS alone without emulate_amd");
	const auto  patched = MapPatchedCode(kPlainVrsqrtpsIndirectBranch, true);
	const auto &counts  = patched.result.reciprocal_sqrt;
	Check(counts.found == 1, "patcher finds the ordinary VEX.128 VRSQRTPS");
	Check(counts.native == 0 && counts.trapped == 1, "VRSQRTPS native trampoline / trap count");
	const auto  *code = static_cast<const uint8_t *>(patched.code);
	const size_t site = FindPlainVrsqrtps(kPlainVrsqrtpsIndirectBranch);
	Check(code[site] == 0xc5 && code[site + 1] == 0xf0 && code[site + 2] == 0x52,
	      "the trap mark clears a reserved VEX.vvvv bit (c5 f8 -> c5 f0)");
	SyntheticState state;
	state.SetRip(code + site);
	const auto out = EmulateReciprocalSquareRoot(state, "TryEmulate handles the marked VRSQRTPS at the patched site");
	Check(out == reference, "patched VRSQRTPS result equals the emulated reference");
	CheckExactReciprocalSquareRoot(out);
	return patched.code;
}

// Runs the VRSQRTPS blobs natively. They are AVX code (VEX moves around the instruction, and the
// trampoline itself), so this part needs a host with AVX. The trap-marked encodings raise #UD on
// every host and go through the signal emulator; the trampoline raises no SIGILL.
void TestReciprocalSquareRootNativeExecution(const YmmWords &reference, void *trampoline_code,
                                             void *trapped_code) {
	std::printf("sse4a_emulation: running the VRSQRTPS blobs natively\n");
	std::fflush(stdout);
	const auto Run = [](void *code, YmmWords &out) {
		reinterpret_cast<LoadBlobFn>(code)(reinterpret_cast<uint64_t *>(out.data()),
		                                   reinterpret_cast<const uint64_t *>(kRsqrtInput));
	};
	YmmWords out {};
	int      before = IllegalHits();
	Run(reinterpret_cast<void *>(MapCode<LoadBlobFn>(kTrappedVrsqrtpsXmm1Xmm0)), out);
	Check(IllegalHits() > before, "the pre-marked VRSQRTPS goes through the signal emulator");
	Check(out == reference, "the pre-marked VRSQRTPS run natively equals the emulated reference");

	out    = {};
	before = IllegalHits();
	Run(trampoline_code, out);
	Check(IllegalHits() == before, "natively patched VRSQRTPS raises no SIGILL");
	Check(out == reference, "patched VRSQRTPS result equals the emulated reference");
	CheckExactReciprocalSquareRoot(out);

	out    = {};
	before = IllegalHits();
	Run(trapped_code, out);
	Check(IllegalHits() > before, "marked VRSQRTPS goes through the signal emulator");
	Check(out == reference, "patched VRSQRTPS result equals the emulated reference");
	CheckExactReciprocalSquareRoot(out);
}

} // namespace

int main() {
	Check(Common::HostException::InstallHandler(Handler), "install host exception handler");
	TestRefusals();
	TestExtrqImmediate();
	TestExtrqFullWidth();
	TestInsertqImmediate();
	TestRexHighRegisterExtrq();
	TestRexHighRegisterInsertqWithIndex();
	TestInsertqRegister();
	TestMwaitxEmulated();
	TestUnknownInstructionRefused();
	const auto rsqrt_reference = TestTrappedReciprocalSquareRoot();
	TestPatchedExtrqImmediate();
	TestPatchedExtrqRexHigh();
	TestPatchedInsertqImmediate();
	TestPatchedInsertqIndexedRexHigh();
	TestPatchedInsertqRegister();
	void *const rsqrt_trampoline = TestPatchedReciprocalSquareRootNative();
	void *const rsqrt_trapped    = TestPatchedReciprocalSquareRootTrap(rsqrt_reference);
	TestReciprocalSquareRootNativeExecution(rsqrt_reference, rsqrt_trampoline, rsqrt_trapped);
	std::printf("sse4a_emulation: all passed\n");
	return 0;
}
