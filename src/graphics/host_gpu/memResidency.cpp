#include "graphics/host_gpu/memResidency.h"

#include "common/gpuPhaseStats.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace Libs::Graphics::MemResidency {
namespace {

constexpr uint64_t PAGE         = 4096;
constexpr auto     SAMPLE_EVERY = std::chrono::seconds(10);

template <size_t N>
struct Matrix {
	std::array<std::array<std::atomic<uint64_t>, ACTOR_COUNT>, N> cell {};
};

std::atomic<bool>     g_enabled {false};
std::atomic<uint64_t> g_probe_armed {0};
Matrix<FAULT_CLASS_COUNT> g_probe_read;
Matrix<FAULT_CLASS_COUNT> g_probe_write;
Matrix<FAULT_CLASS_COUNT> g_write_fault;
std::array<std::atomic<uint64_t>, ACTOR_COUNT> g_read_fault {};

std::chrono::steady_clock::time_point g_start;
std::atomic<int64_t>                  g_next_sample_ns {0};

std::mutex                 g_mutex;
std::condition_variable    g_cv;
std::deque<Snapshot>       g_queue;
bool                       g_stop = false;
std::thread                g_thread;

uint64_t Load(const std::atomic<uint64_t>& v) {
	return v.load(std::memory_order_relaxed);
}

uint64_t Row(const Matrix<FAULT_CLASS_COUNT>& m, FaultClass cls) {
	uint64_t sum = 0;
	for (const auto& a: m.cell[static_cast<size_t>(cls)]) {
		sum += Load(a);
	}
	return sum;
}

uint64_t Col(const Matrix<FAULT_CLASS_COUNT>& m, Actor actor) {
	uint64_t sum = 0;
	for (const auto& row: m.cell) {
		sum += Load(row[static_cast<size_t>(actor)]);
	}
	return sum;
}

const char* RangeClassName(size_t i) {
	static constexpr const char* names[] = {"tex_clean", "tex_other", "rt", "depth", "buffer"};
	return names[i];
}

uint64_t ResidentOf(const RangeList& list) {
	uint64_t sum = 0;
	for (const auto& [address, size]: list) {
		sum += LibKernel::Memory::KernelGuestResidentBytes(address, size);
	}
	return sum;
}

double GiB(uint64_t bytes) {
	return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

void Report(const Snapshot& snapshot) {
	const auto seconds = std::chrono::duration_cast<std::chrono::milliseconds>(
	                         std::chrono::steady_clock::now() - g_start)
	                         .count() / 1000.0;
	// Merge each class, then give every page to the first class that claims it (render targets and
	// depth, then clean textures, other textures, buffers) so the classes add up.
	std::array<RangeList, RANGE_CLASS_COUNT> merged;
	for (size_t i = 0; i < RANGE_CLASS_COUNT; i++) {
		merged[i] = Merge(snapshot.ranges[i], PAGE);
	}
	constexpr std::array<RangeClass, RANGE_CLASS_COUNT> order = {
	    RangeClass::RenderTarget, RangeClass::Depth, RangeClass::TextureClean,
	    RangeClass::TextureOther, RangeClass::Buffer};
	std::array<RangeList, RANGE_CLASS_COUNT> exclusive;
	RangeList                                claimed;
	for (const auto cls: order) {
		const auto i  = static_cast<size_t>(cls);
		exclusive[i]  = Subtract(merged[i], claimed);
		claimed       = Union(claimed, exclusive[i]);
	}
	const uint64_t total_resident = LibKernel::Memory::KernelGuestResidentTotalBytes();
	char           text[2048];
	int            n = std::snprintf(text, sizeof(text), "[mem-residency t=%.1fs frame=%llu] guest_resident=%.3f GiB",
	                                 seconds, static_cast<unsigned long long>(snapshot.frame),
	                                 GiB(total_resident));
	uint64_t       classified = 0;
	for (size_t i = 0; i < RANGE_CLASS_COUNT && n > 0 && static_cast<size_t>(n) < sizeof(text); i++) {
		const uint64_t bytes    = TotalBytes(exclusive[i]);
		const uint64_t resident = ResidentOf(exclusive[i]);
		classified += resident;
		n += std::snprintf(text + n, sizeof(text) - n, " | %s mapped=%.3f resident=%.3f GiB",
		                   RangeClassName(i), GiB(bytes), GiB(resident));
	}
	// Pages that a buffer and a clean texture both claim (a second GPU copy of the same bytes).
	const auto overlap =
	    Intersect(merged[static_cast<size_t>(RangeClass::TextureClean)],
	              merged[static_cast<size_t>(RangeClass::Buffer)]);
	const uint64_t other = total_resident > classified ? total_resident - classified : 0;
	n += std::snprintf(text + n, sizeof(text) - n,
	                   " | tex_clean&buffer overlap=%.3f GiB (resident %.3f) | other_guest resident=%.3f GiB",
	                   GiB(TotalBytes(overlap)), GiB(ResidentOf(overlap)), GiB(other));
	Log::WriteToConsoleAndLog(std::string(text) + "\n");

	// CPU faults and probes (cumulative).
	const auto& probes = snapshot.probes;
	uint64_t    rd = 0, wr = 0;
	for (size_t c = 0; c < FAULT_CLASS_COUNT; c++) {
		rd += Row(g_probe_read, static_cast<FaultClass>(c));
		wr += Row(g_probe_write, static_cast<FaultClass>(c));
	}
	const auto arm = probes.armed;
	n = std::snprintf(
	    text, sizeof(text),
	    "[mem-residency-faults t=%.1fs] probes armed=%llu pages (%.3f GiB) read=%llu written=%llu"
	    " dropped=%llu live=%llu | read-after-upload %.2f%% of armed pages"
	    " | probe reads guest=%llu gpu=%llu host=%llu | probe reads by class tex=%llu rt=%llu depth=%llu other=%llu"
	    " | write faults tex=%llu rt=%llu depth=%llu non-image=%llu (guest=%llu gpu=%llu host=%llu)"
	    " | gpu-dirty read faults guest=%llu gpu=%llu host=%llu\n",
	    seconds, static_cast<unsigned long long>(arm), GiB(arm * PAGE),
	    static_cast<unsigned long long>(rd), static_cast<unsigned long long>(wr),
	    static_cast<unsigned long long>(probes.dropped),
	    static_cast<unsigned long long>(arm - std::min(arm, probes.consumed + probes.dropped)),
	    arm != 0 ? 100.0 * static_cast<double>(rd) / static_cast<double>(arm) : 0.0,
	    static_cast<unsigned long long>(Col(g_probe_read, Actor::Guest)),
	    static_cast<unsigned long long>(Col(g_probe_read, Actor::Gpu)),
	    static_cast<unsigned long long>(Col(g_probe_read, Actor::Host)),
	    static_cast<unsigned long long>(Row(g_probe_read, FaultClass::Texture)),
	    static_cast<unsigned long long>(Row(g_probe_read, FaultClass::RenderTarget)),
	    static_cast<unsigned long long>(Row(g_probe_read, FaultClass::Depth)),
	    static_cast<unsigned long long>(Row(g_probe_read, FaultClass::NonImage)),
	    static_cast<unsigned long long>(Row(g_write_fault, FaultClass::Texture)),
	    static_cast<unsigned long long>(Row(g_write_fault, FaultClass::RenderTarget)),
	    static_cast<unsigned long long>(Row(g_write_fault, FaultClass::Depth)),
	    static_cast<unsigned long long>(Row(g_write_fault, FaultClass::NonImage)),
	    static_cast<unsigned long long>(Col(g_write_fault, Actor::Guest)),
	    static_cast<unsigned long long>(Col(g_write_fault, Actor::Gpu)),
	    static_cast<unsigned long long>(Col(g_write_fault, Actor::Host)),
	    static_cast<unsigned long long>(Load(g_read_fault[0])),
	    static_cast<unsigned long long>(Load(g_read_fault[1])),
	    static_cast<unsigned long long>(Load(g_read_fault[2])));
	(void)n;
	Log::WriteToConsoleAndLog(text);

	// Bytes Kyty itself read from guest memory that was not resident (reads of >= 16 KiB).
	const auto reads = LibKernel::Memory::KernelBackingReadStats();
	const auto line  = [&](const char* name, const LibKernel::Memory::BackingReadStats::Counts& c) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "%s reads=%llu bytes=%.3f GiB non-resident=%.3f GiB (never written=%.3f GiB, swapped=%.3f GiB)"
                      " pread=%.3f GiB",
                      name, static_cast<unsigned long long>(c.reads), GiB(c.bytes),
                      GiB(c.nonresident), GiB(c.hole),
                      GiB(c.nonresident > c.hole ? c.nonresident - c.hole : 0), GiB(c.pread_bytes));
        return std::string(buf);
	};
	Log::WriteToConsoleAndLog("[mem-residency-reads t=" + std::to_string(static_cast<int>(seconds)) +
	                          "s] image: " + line("image", reads.image) + " | other: " +
	                          line("other", reads.other) + " | pread failures " +
	                          std::to_string(reads.pread_failures) + "\n");
}

void WorkerMain() {
	KYTY_PROFILER_THREAD("kyMemResidency");
	std::unique_lock lock(g_mutex);
	for (;;) {
		g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
		if (g_queue.empty()) {
			return; // stopped
		}
		auto snapshot = std::move(g_queue.front());
		g_queue.pop_front();
		lock.unlock();
		Report(snapshot);
		lock.lock();
	}
}

} // namespace

void Enable(bool on) {
	if (on == g_enabled.load()) {
		return;
	}
	if (on) {
		g_start = std::chrono::steady_clock::now();
		g_next_sample_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(SAMPLE_EVERY).count());
		g_stop = false;
		g_enabled.store(true);
		g_thread = std::thread(WorkerMain);
		return;
	}
	g_enabled.store(false);
	{
		std::lock_guard lock(g_mutex);
		g_stop = true;
	}
	g_cv.notify_all();
	if (g_thread.joinable()) {
		g_thread.join();
	}
}

bool Enabled() noexcept {
	return g_enabled.load(std::memory_order_relaxed);
}

Actor CurrentActor() noexcept {
	if (GuestGpu::IsGpuThread()) {
		return Actor::Gpu;
	}
	return LibKernel::PthreadSelfOrNull() != nullptr ? Actor::Guest : Actor::Host;
}

bool SampleDue() noexcept {
	if (!Enabled()) {
		return false;
	}
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                     std::chrono::steady_clock::now() - g_start)
	                     .count();
	auto next = g_next_sample_ns.load(std::memory_order_relaxed);
	if (now < next) {
		return false;
	}
	return g_next_sample_ns.compare_exchange_strong(
	    next, now + std::chrono::duration_cast<std::chrono::nanoseconds>(SAMPLE_EVERY).count());
}

void Submit(Snapshot&& snapshot) {
	{
		std::lock_guard lock(g_mutex);
		if (g_queue.size() >= 4) {
			return; // the worker is behind; drop this sample
		}
		g_queue.push_back(std::move(snapshot));
	}
	g_cv.notify_one();
}

void NoteProbeArmed(uint64_t pages) noexcept {
	g_probe_armed.fetch_add(pages, std::memory_order_relaxed);
}

void NoteProbeRead(FaultClass cls, Actor actor) noexcept {
	g_probe_read.cell[static_cast<size_t>(cls)][static_cast<size_t>(actor)].fetch_add(
	    1, std::memory_order_relaxed);
}

void NoteProbeWrite(FaultClass cls, Actor actor) noexcept {
	g_probe_write.cell[static_cast<size_t>(cls)][static_cast<size_t>(actor)].fetch_add(
	    1, std::memory_order_relaxed);
}

void NoteWriteFault(FaultClass cls, Actor actor) noexcept {
	g_write_fault.cell[static_cast<size_t>(cls)][static_cast<size_t>(actor)].fetch_add(
	    1, std::memory_order_relaxed);
}

void NoteReadFault(Actor actor) noexcept {
	g_read_fault[static_cast<size_t>(actor)].fetch_add(1, std::memory_order_relaxed);
}

} // namespace Libs::Graphics::MemResidency
