#include "common/hleTrace.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(__linux__) && defined(__x86_64__)
#define KYTY_HLE_TRACE_SUPPORTED 1
#include <pthread.h>
#include <sys/mman.h>
#endif

namespace Common::HleTrace {

bool detail::g_enabled = false;

void Enable(bool enabled) noexcept {
	detail::g_enabled = enabled;
}

#if defined(KYTY_HLE_TRACE_SUPPORTED)

namespace {

// Layout is known to the trampoline: `jmp *8(%r11)`.
struct Record {
	const char* name;
	uint64_t    target;
};
static_assert(offsetof(Record, target) == 8);

constexpr int64_t  NO_RESULT     = INT64_MIN;
constexpr unsigned RING_ENTRIES  = 256; // power of two
constexpr unsigned MAX_RINGS     = 2048;
constexpr size_t   NOTE_CAPACITY = 60;

struct Entry {
	uint64_t      ns;
	const Record* record;
	uint64_t      arg[4];
	uint64_t      caller;
	int64_t       result;
	uint32_t      flags;
	char          note[NOTE_CAPACITY];
};
static_assert(sizeof(Entry) == 128);

struct Ring {
	std::atomic<uint64_t> head {0};
	pthread_t             thread {};
	char                  name[24] {};
	Entry                 entries[RING_ENTRIES] {};
};

std::atomic<Ring*>    g_rings[MAX_RINGS];
std::atomic<uint32_t> g_ring_count {0};
std::atomic<uint64_t> g_call_count {0};
thread_local Ring*    t_ring        = nullptr;
thread_local bool     t_ring_failed = false;
uint64_t              g_start_ns    = 0;

uint64_t NowNs() noexcept {
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

Ring* GetRing() noexcept {
	if (t_ring != nullptr) {
		return t_ring;
	}
	if (t_ring_failed) {
		return nullptr;
	}
	const auto slot = g_ring_count.fetch_add(1, std::memory_order_relaxed);
	if (slot >= MAX_RINGS) {
		t_ring_failed = true;
		return nullptr;
	}
	auto* ring = new (std::nothrow) Ring;
	if (ring == nullptr) {
		t_ring_failed = true;
		return nullptr;
	}
	ring->thread = pthread_self();
	pthread_getname_np(ring->thread, ring->name, sizeof(ring->name));
	t_ring = ring;
	g_rings[slot].store(ring, std::memory_order_release);
	return ring;
}

} // namespace

extern "C" void KytyHleTraceLog(const Record* record, const uint64_t* saved) noexcept {
	Ring* ring = GetRing();
	if (ring == nullptr) {
		return;
	}
	const auto head = ring->head.load(std::memory_order_relaxed);
	if ((head & 255u) == 0) { // the guest names its threads after they start
		pthread_getname_np(ring->thread, ring->name, sizeof(ring->name));
	}
	auto& e  = ring->entries[head & (RING_ENTRIES - 1)];
	e.ns     = NowNs();
	e.record = record;
	// saved[]: r11, rax, r9, r8, rcx, rdx, rsi, rdi, return address
	e.arg[0]   = saved[7];
	e.arg[1]   = saved[6];
	e.arg[2]   = saved[5];
	e.arg[3]   = saved[4];
	e.caller   = saved[8];
	e.result   = NO_RESULT;
	e.flags    = 0;
	e.note[0]  = '\0';
	ring->head.store(head + 1, std::memory_order_release);
	g_call_count.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void KytyHleTraceTrampoline();

// Entered by `jmp` from a per-function thunk with r11 = Record*, and the caller's return address on
// top of the stack. Everything the callee can observe is restored before the tail jump.
asm(R"(
.text
.globl KytyHleTraceTrampoline
.type KytyHleTraceTrampoline,@function
.p2align 4
KytyHleTraceTrampoline:
	pushq %rdi
	pushq %rsi
	pushq %rdx
	pushq %rcx
	pushq %r8
	pushq %r9
	pushq %rax
	pushq %r11
	subq $136, %rsp
	movups %xmm0, 0(%rsp)
	movups %xmm1, 16(%rsp)
	movups %xmm2, 32(%rsp)
	movups %xmm3, 48(%rsp)
	movups %xmm4, 64(%rsp)
	movups %xmm5, 80(%rsp)
	movups %xmm6, 96(%rsp)
	movups %xmm7, 112(%rsp)
	movq %r11, %rdi
	leaq 136(%rsp), %rsi
	call KytyHleTraceLog@PLT
	movups 0(%rsp), %xmm0
	movups 16(%rsp), %xmm1
	movups 32(%rsp), %xmm2
	movups 48(%rsp), %xmm3
	movups 64(%rsp), %xmm4
	movups 80(%rsp), %xmm5
	movups 96(%rsp), %xmm6
	movups 112(%rsp), %xmm7
	addq $136, %rsp
	popq %r11
	popq %rax
	popq %r9
	popq %r8
	popq %rcx
	popq %rdx
	popq %rsi
	popq %rdi
	jmp *8(%r11)
.size KytyHleTraceTrampoline, .-KytyHleTraceTrampoline
)");

namespace {

uint8_t* g_thunk_chunk    = nullptr;
size_t   g_thunk_used     = 0;
size_t   g_thunk_capacity = 0;

constexpr size_t THUNK_SIZE  = 32;
constexpr size_t CHUNK_BYTES = size_t {1} << 20;

Entry* CurrentEntry() noexcept {
	Ring* ring = t_ring;
	if (ring == nullptr) {
		return nullptr;
	}
	const auto head = ring->head.load(std::memory_order_relaxed);
	if (head == 0) {
		return nullptr;
	}
	return &ring->entries[(head - 1) & (RING_ENTRIES - 1)];
}

void CopyNote(Entry* e, const char* text) noexcept {
	if (text == nullptr) {
		e->note[0] = '\0';
		return;
	}
	size_t i = 0;
	for (; i + 1 < NOTE_CAPACITY && text[i] != '\0'; i++) {
		e->note[i] = text[i];
	}
	e->note[i] = '\0';
}

} // namespace

void detail::NoteStringSlow(const char* text) noexcept {
	if (auto* e = CurrentEntry()) {
		CopyNote(e, text);
	}
}

void detail::NoteResultSlow(int64_t result) noexcept {
	if (auto* e = CurrentEntry()) {
		e->result = result;
	}
}

void detail::NoteResultStringSlow(int64_t result, const char* text) noexcept {
	if (auto* e = CurrentEntry()) {
		e->result = result;
		CopyNote(e, text);
	}
}

uint64_t Wrap(const char* name, uint64_t target) {
	if (!detail::g_enabled || target == 0) {
		return target;
	}
	if (g_start_ns == 0) {
		g_start_ns = NowNs();
	}
	if (g_thunk_chunk == nullptr || g_thunk_used + THUNK_SIZE > g_thunk_capacity) {
		void* p = mmap(nullptr, CHUNK_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			return target;
		}
		g_thunk_chunk    = static_cast<uint8_t*>(p);
		g_thunk_used     = 0;
		g_thunk_capacity = CHUNK_BYTES;
	}
	// The record and the name live for the whole run.
	auto* record   = new Record {strdup(name), target};
	uint8_t* code  = g_thunk_chunk + g_thunk_used;
	g_thunk_used  += THUNK_SIZE;

	const auto record_address = reinterpret_cast<uint64_t>(record);
	const auto trampoline     = reinterpret_cast<uint64_t>(&KytyHleTraceTrampoline);
	uint8_t    bytes[THUNK_SIZE] = {};
	bytes[0] = 0x49; // mov r11, imm64
	bytes[1] = 0xbb;
	std::memcpy(bytes + 2, &record_address, 8);
	bytes[10] = 0xff; // jmp [rip+0]
	bytes[11] = 0x25;
	bytes[12] = bytes[13] = bytes[14] = bytes[15] = 0;
	std::memcpy(bytes + 16, &trampoline, 8);
	std::memcpy(code, bytes, THUNK_SIZE);
	return reinterpret_cast<uint64_t>(code);
}

uint64_t CallCount() noexcept {
	return g_call_count.load(std::memory_order_relaxed);
}

namespace {

void PrintRing(FILE* out, const Ring* ring, unsigned count, uint64_t now_ns, bool is_current,
               LocateFn locate) noexcept {
	const auto head = ring->head.load(std::memory_order_acquire);
	if (head == 0) {
		return;
	}
	const auto available = std::min<uint64_t>(head, RING_ENTRIES);
	const auto n         = std::min<uint64_t>(count, available);
	std::fprintf(out, "-- thread %s%s: %" PRIu64 " HLE calls, last %" PRIu64 " (ms = before the dump) --\n",
	             ring->name[0] != '\0' ? ring->name : "(unnamed)", is_current ? " [faulting]" : "",
	             head, n);
	for (uint64_t i = head - n; i < head; i++) {
		const Entry e = ring->entries[i & (RING_ENTRIES - 1)]; // copy: the owner may still write
		char          where[96] = {};
		if (locate != nullptr && e.caller != 0) {
			locate(e.caller, where, sizeof(where));
		}
		std::fprintf(out, "  %8.1f ms  %s(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")",
		             -static_cast<double>(now_ns - std::min(now_ns, e.ns)) / 1.0e6,
		             e.record != nullptr ? e.record->name : "?", e.arg[0], e.arg[1], e.arg[2],
		             e.arg[3]);
		if (e.result != NO_RESULT) {
			std::fprintf(out, " = %" PRId64 " (0x%" PRIx64 ")", e.result,
			             static_cast<uint64_t>(e.result));
		} else {
			std::fprintf(out, " = ?");
		}
		if (e.note[0] != '\0') {
			std::fprintf(out, "  \"%.60s\"", e.note);
		}
		if (where[0] != '\0') {
			std::fprintf(out, "  from%s", where);
		}
		std::fprintf(out, "\n");
	}
}

} // namespace

void Dump(FILE* out, unsigned own_entries, unsigned other_entries, LocateFn locate) noexcept {
	if (!detail::g_enabled || out == nullptr) {
		return;
	}
	const auto now = NowNs();
	std::fprintf(out, "--- HLE call trace (--debug-hle-trace), %" PRIu64 " calls recorded ---\n",
	             g_call_count.load(std::memory_order_relaxed));
	const Ring* own = t_ring;
	if (own != nullptr) {
		PrintRing(out, own, own_entries, now, true, locate);
	}
	const auto count = std::min<uint32_t>(g_ring_count.load(std::memory_order_acquire), MAX_RINGS);
	for (uint32_t i = 0; i < count; i++) {
		const Ring* ring = g_rings[i].load(std::memory_order_acquire);
		if (ring != nullptr && ring != own) {
			PrintRing(out, ring, other_entries, now, false, locate);
		}
	}
	std::fflush(out);
}

#else // !KYTY_HLE_TRACE_SUPPORTED

void detail::NoteStringSlow(const char*) noexcept {}
void detail::NoteResultSlow(int64_t) noexcept {}
void detail::NoteResultStringSlow(int64_t, const char*) noexcept {}

uint64_t Wrap(const char*, uint64_t target) {
	return target;
}

void Dump(FILE*, unsigned, unsigned, LocateFn) noexcept {}

uint64_t CallCount() noexcept {
	return 0;
}

#endif

} // namespace Common::HleTrace
