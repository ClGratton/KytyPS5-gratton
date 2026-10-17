#include "common/gpuPhaseStats.h"

#include "common/benchInstrument.h"
#include "common/logging/log.h"
#include "common/phaseTimers.h"
#include "common/shardedCounter.h"

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

#if defined(__linux__)
#include <dlfcn.h>
#include <execinfo.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fmt/format.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace Common::GpuPhase {

namespace {

constexpr size_t kPhases    = static_cast<size_t>(Phase::Count);
constexpr size_t kCoalesce  = static_cast<size_t>(Coalesce::Count);

constexpr std::array<const char*, kPhases> kNames = {
    "idle", "pm4", "draw", "pipeline", "pipeline_job", "srt", "descriptors", "vkcmd", "vkalloc",
    "submit", "drain_tick", "drain_flush", "drain_finish", "gc", "pipe_prepare", "pipe_programkey",
    "pipe_materialize", "pipe_keybuild", "pipe_find", "pipe_entrylock", "pipe_srtread",
    "pipe_permsearch", "desc_buffers", "desc_upload", "desc_images", "desc_resolve", "desc_findtex",
    "desc_commit", "desc_hazard", "desc_writes", "desc_transit", "desc_push", "srt_shadow", "srt_adopt",
    "draw_checks", "draw_targets", "draw_vertex", "draw_dynamic", "srt_reuse", "srt_resolve", "srt_samplers",
    "srt_buffers", "draw_pop", "draw_describe", "draw_record"};

// Tracy zone per phase (--profile): named "gpu:<phase>" and emitted only on the GPU thread, so
// `tracy-csvexport -e -f gpu:` yields the exclusive time per phase of that thread alone.
constexpr size_t kMaxZoneDepth = 24;
const std::array<std::string, kPhases> kZoneNames = [] {
	std::array<std::string, kPhases> names;
	for (size_t i = 0; i < kPhases; ++i) {
		names[i] = std::string("gpu:") + kNames[i];
	}
	return names;
}();
const std::array<___tracy_source_location_data, kPhases> kZoneLocations = [] {
	std::array<___tracy_source_location_data, kPhases> locations {};
	for (size_t i = 0; i < kPhases; ++i) {
		locations[i] = {kZoneNames[i].c_str(), "GpuPhase", __FILE__, 0, 0};
	}
	return locations;
}();

std::atomic<uint32_t> g_interval_s {0};
std::atomic<uint64_t> g_cut_requests {0};
uint64_t              g_cuts_seen = 0; // GPU thread only
std::atomic<bool>     g_phases {true};
ShardedCounter        g_submits;
std::atomic<uint64_t> g_frames {0};
ShardedCounter        g_barriers_emitted;
ShardedCounter        g_barriers_elided;
ShardedCounter        g_qr_calls;
ShardedCounter        g_qr_flushed;
ShardedCounter        g_qr_lag;
ShardedCounter        g_clean_reads;
ShardedCounter        g_direct_reads;
std::array<ShardedCounter, static_cast<size_t>(KnownFill::Count)> g_known_fills {};
std::array<ShardedCounter, static_cast<size_t>(KnownStore::Count)> g_known_stores {};
std::array<ShardedCounter, static_cast<size_t>(KnownCopy::Count)> g_known_copies {};
std::array<ShardedCounter, kCoalesce> g_coalesce {};
std::array<std::atomic<uint64_t>, static_cast<size_t>(Eager::Count)> g_eager {};
std::array<std::atomic<uint64_t>, static_cast<size_t>(Future::Count)> g_future {};
std::atomic<uint64_t> g_future_wait_ns {0};
std::array<ShardedCounter, static_cast<size_t>(IndirectDraw::Route::Count)>
    g_indirect_routes {};
std::atomic<uint32_t> g_indirect_logged {0};

int64_t WallNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

bool IsDrain(Phase phase) {
	return phase == Phase::DrainTick || phase == Phase::DrainFlush || phase == Phase::DrainFinish;
}

} // namespace

namespace detail {

struct ThreadState {
	PhaseTimers::ExclusiveClock<kPhases> clock;
	uint32_t                             current_opcode = 0;
	std::array<uint64_t, 256>            packet_ticks {};
	std::array<uint64_t, 256>            packet_count {};
	std::array<uint64_t, 256>            drain_ticks_by_packet {};
	DrainCauses::Table                   drain_causes;      // DrainTick waits, by cause
	DrainCauses::Table                   window_drain_causes;
	struct Readback {
		uint64_t   calls = 0, bytes = 0, ticks = 0;
		DrainCause cause = DrainCause::Other; // of the latest readback of this region
	};
	std::unordered_map<uint64_t, Readback> readbacks; // by guest address of the first copy
	struct FaultSite {
		uint64_t              hits = 0;
		std::vector<uint64_t> offsets; // return addresses relative to the main binary
	};
	std::unordered_map<uint64_t, FaultSite> fault_sites; // by hash of the stack
	std::unordered_map<uint64_t, uint64_t>  fault_reported; // hits already listed

	// Window start (previous report) and what the counters read then.
	int64_t  window_wall_ns = 0;
	uint64_t window_tsc     = 0;
	int64_t  first_wall_ns  = 0;
	uint64_t window_submits = 0;
	uint64_t window_frames  = 0;
	uint64_t window_barriers_emitted = 0;
	uint64_t window_barriers_elided  = 0;
	std::array<uint64_t, kPhases>   window_total {};
	std::array<uint64_t, kPhases>   window_calls {};
	std::array<uint64_t, 256>       window_packet_ticks {};
	std::array<uint64_t, 256>       window_packet_count {};
	std::array<uint64_t, 256>       window_drain_by_packet {};
	std::array<uint64_t, kCoalesce> window_coalesce {};
	bool                            phases = true;
	bool                            zones  = false; // also emit Tracy zones
	std::array<TracyCZoneCtx, kMaxZoneDepth> zone_stack {};
	size_t                          zone_depth = 0;
	std::FILE*                      csv    = nullptr;
};

thread_local ThreadState* t_state = nullptr;
thread_local DrainCause   t_drain_cause = DrainCause::Other;
thread_local bool         t_drain_full  = false;
thread_local uint64_t     t_readback_key = 0;

uint64_t Now() noexcept {
#if defined(__x86_64__) || defined(__i386__)
	return __builtin_ia32_rdtsc();
#else
	return static_cast<uint64_t>(WallNs());
#endif
}

void Enter(ThreadState* state, Phase phase) noexcept {
	if (state->zones && state->zone_depth < kMaxZoneDepth) {
		state->zone_stack[state->zone_depth] =
		    ___tracy_emit_zone_begin(&kZoneLocations[static_cast<size_t>(phase)], 1);
	}
	++state->zone_depth;
	state->clock.Enter(static_cast<size_t>(phase), Now());
}

void Exit(ThreadState* state, Phase phase, uint64_t start) noexcept {
	const auto now = Now();
	state->clock.Exit(now);
	if (state->zone_depth != 0) {
		--state->zone_depth;
		if (state->zones && state->zone_depth < kMaxZoneDepth) {
			___tracy_emit_zone_end(state->zone_stack[state->zone_depth]);
		}
	}
	if (IsDrain(phase) && now > start) {
		state->drain_ticks_by_packet[state->current_opcode & 0xffu] += now - start;
		if (phase == Phase::DrainTick) {
			state->drain_causes.Add(t_drain_cause, t_drain_full, now - start);
			if (t_readback_key != 0) {
				state->readbacks[t_readback_key].ticks += now - start;
				t_readback_key = 0;
			}
		}
	}
}

void NoteReadbackImpl(ThreadState* state, uint64_t address, uint64_t bytes) noexcept {
	// Keyed by region and cause (cause in the top byte; guest addresses use 48 bits).
	const uint64_t key = address | (static_cast<uint64_t>(t_drain_cause) << 56);
	if (state->readbacks.size() < 8192 || state->readbacks.contains(key)) {
		auto& r = state->readbacks[key];
		++r.calls;
		r.bytes += bytes;
		r.cause = t_drain_cause;
	}
	t_readback_key = key;
}

void SetPacket(ThreadState* state, uint32_t opcode) noexcept {
	state->current_opcode = opcode;
}

void PacketEnd(ThreadState* state, uint32_t opcode, uint64_t start) noexcept {
	const auto now = Now();
	if (now > start) {
		state->packet_ticks[opcode & 0xffu] += now - start;
	}
	++state->packet_count[opcode & 0xffu];
}

} // namespace detail

namespace {
// The reporting state of the attached thread. t_state (what the scopes see) is the same object
// only when phases are timed (--gpu-phase-stats); --submit-stats reports counters only.
thread_local detail::ThreadState* t_report = nullptr;
} // namespace

namespace {
// Cost of one Scope (rdtsc, Enter, rdtsc, Exit) measured in a tight loop on a throw-away state.
// A lower bound: the real scopes run with colder caches between them.
double g_scope_ns = 0.0;

double MeasureScopeNs() {
	auto* state = new detail::ThreadState();
	state->zones = false;
	constexpr int kRounds = 200000;
	const auto    t0      = WallNs();
	for (int i = 0; i < kRounds; ++i) {
		const auto start = detail::Now();
		detail::Enter(state, Phase::Pm4);
		detail::Exit(state, Phase::Pm4, start);
	}
	const auto t1 = WallNs();
	delete state;
	return static_cast<double>(t1 - t0) / kRounds;
}
} // namespace

void Configure(uint32_t interval_seconds, bool phases) {
	g_phases.store(phases, std::memory_order_relaxed);
	g_interval_s.store(interval_seconds, std::memory_order_relaxed);
}

bool Enabled() noexcept {
	return g_interval_s.load(std::memory_order_relaxed) != 0;
}

void NoteGpuThreadFault() noexcept {
#if defined(__linux__)
	auto* state = detail::t_state;
	if (state == nullptr) {
		return;
	}
	void*     frames[24];
	const int n = backtrace(frames, 24);
	uint64_t  hash = 1469598103934665603ull;
	std::vector<uint64_t> offsets;
	Dl_info               info {};
	for (int i = 0; i < n; ++i) {
		const auto address = reinterpret_cast<uint64_t>(frames[i]);
		uint64_t   offset  = address;
		if (dladdr(frames[i], &info) != 0 && info.dli_fbase != nullptr) {
			offset = address - reinterpret_cast<uint64_t>(info.dli_fbase);
		}
		hash = (hash ^ offset) * 1099511628211ull;
		offsets.push_back(offset);
	}
	if (state->fault_sites.size() >= 64 && !state->fault_sites.contains(hash)) {
		return;
	}
	auto& site = state->fault_sites[hash];
	++site.hits;
	if (site.offsets.empty()) {
		site.offsets = std::move(offsets);
	}
#endif
}

void NoteReadback(uint64_t address, uint64_t bytes) noexcept {
	if (detail::t_state != nullptr) {
		detail::NoteReadbackImpl(detail::t_state, address, bytes);
	}
}

void NoteSubmit() noexcept {
	g_submits.fetch_add(1, std::memory_order_relaxed);
}

void NoteBarrier(bool elided) noexcept {
	(elided ? g_barriers_elided : g_barriers_emitted).fetch_add(1, std::memory_order_relaxed);
}

void NoteKnownFill(KnownFill kind) noexcept {
	g_known_fills[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
}

void NoteKnownCopy(KnownCopy kind, uint64_t n) noexcept {
	g_known_copies[static_cast<size_t>(kind)].fetch_add(n, std::memory_order_relaxed);
}

void NoteKnownStore(KnownStore kind, uint64_t n) noexcept {
	g_known_stores[static_cast<size_t>(kind)].fetch_add(n, std::memory_order_relaxed);
}

void NoteIndirectDraw(IndirectDraw::Route route, uint32_t draws) noexcept {
	const auto index = static_cast<size_t>(route);
	g_indirect_routes[index].fetch_add(draws, std::memory_order_relaxed);
	if (IndirectDraw::IsFallbackWorthLogging(route) && (g_indirect_logged.load(std::memory_order_relaxed) & (1u << index)) == 0 &&
	    (g_indirect_logged.fetch_or(1u << index, std::memory_order_relaxed) & (1u << index)) == 0) {
		std::printf("gpu-indirect-draws: %s (further draws of this kind are only counted)\n",
		            IndirectDraw::kRouteNames[index]);
		std::fflush(stdout);
	}
}

void NoteCleanRead(bool clean) noexcept {
	(clean ? g_clean_reads : g_direct_reads).fetch_add(1, std::memory_order_relaxed);
}

void NoteQueueReadback(bool flushed, uint64_t lag_ticks) noexcept {
	g_qr_calls.fetch_add(1, std::memory_order_relaxed);
	g_qr_flushed.fetch_add(flushed ? 1 : 0, std::memory_order_relaxed);
	g_qr_lag.fetch_add(lag_ticks, std::memory_order_relaxed);
}

void NoteEager(Eager kind) noexcept {
	g_eager[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
}

void NoteFuture(Future kind, uint64_t wait_ns) noexcept {
	g_future[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
	if (wait_ns != 0) {
		g_future_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
	}
}

uint64_t FutureCount(Future kind) noexcept {
	return g_future[static_cast<size_t>(kind)].load(std::memory_order_relaxed);
}

void NoteFrame() noexcept {
	g_frames.fetch_add(1, std::memory_order_relaxed);
}

void NoteCoalesce(Coalesce kind, uint64_t count) noexcept {
	g_coalesce[static_cast<size_t>(kind)].fetch_add(count, std::memory_order_relaxed);
}

uint64_t Submits() noexcept {
	return g_submits.load(std::memory_order_relaxed);
}

uint64_t Frames() noexcept {
	return g_frames.load(std::memory_order_relaxed);
}

void AttachGpuThread() {
	const bool stats = Enabled();
	const bool zones = tracy::ProfilerAvailable();
	if ((!stats && !zones) || t_report != nullptr) {
		return;
	}
	// Intentionally leaked: the thread can be torn down by quick_exit().
	auto* state           = new detail::ThreadState();
	state->phases         = stats ? g_phases.load(std::memory_order_relaxed) : true;
	state->zones          = zones;
	state->window_wall_ns = WallNs();
	state->first_wall_ns  = state->window_wall_ns;
	state->window_tsc     = detail::Now();
	state->window_submits = Submits();
	state->window_frames  = Frames();
	state->window_barriers_emitted = g_barriers_emitted.load(std::memory_order_relaxed);
	state->window_barriers_elided  = g_barriers_elided.load(std::memory_order_relaxed);
	for (size_t i = 0; i < kCoalesce; ++i) {
		state->window_coalesce[i] = g_coalesce[i].load(std::memory_order_relaxed);
	}
	state->csv = stats ? std::fopen(state->phases ? "_gpu_phase.csv" : "_submit_stats.csv", "w")
	                   : nullptr;
	if (state->csv != nullptr) {
		std::string header = "t_s,window_s,frames,submits,coalesce_deferred,coalesce_batch,"
		                     "coalesce_deadline,coalesce_forced,end_ns";
		if (state->phases) {
			for (const auto* name: kNames) {
				header += fmt::format(",{}_pct,{}_calls", name, name);
			}
			header += ",unscoped_pct";
			for (const auto* name: DrainCauses::kCauseNames) {
				header += fmt::format(",drain_{}_ms,drain_{}_calls", name, name);
			}
		}
		header += "\n";
		std::fputs(header.c_str(), state->csv);
		std::fflush(state->csv);
	}
	t_report = state;
	if (state->phases) {
		if (stats) g_scope_ns = MeasureScopeNs();
		detail::t_state = state; // --gpu-phase-stats or --profile: the scopes are live
	}
}

namespace {

void Report(detail::ThreadState& s, bool final_report) {
	const auto   wall_now = WallNs();
	const auto   tsc_now  = detail::Now();
	const double wall_ns  = static_cast<double>(wall_now - s.window_wall_ns);
	const double tsc_d    = static_cast<double>(tsc_now - s.window_tsc);
	if (wall_ns <= 0.0 || tsc_d <= 0.0) {
		return;
	}
	// TSC ticks to nanoseconds from the window itself (no calibration step).
	const double ns_per_tick = wall_ns / tsc_d;
	const auto   frames      = Frames() - s.window_frames;
	const auto   submits     = Submits() - s.window_submits;
	const auto   barriers_emitted = g_barriers_emitted.load(std::memory_order_relaxed);
	const auto   barriers_elided  = g_barriers_elided.load(std::memory_order_relaxed);
	const auto   barrier_emit_d   = barriers_emitted - s.window_barriers_emitted;
	const auto   barrier_elide_d  = barriers_elided - s.window_barriers_elided;
	s.window_barriers_emitted     = barriers_emitted;
	s.window_barriers_elided      = barriers_elided;
	std::array<uint64_t, kCoalesce> coalesce {};
	for (size_t i = 0; i < kCoalesce; ++i) {
		const auto now = g_coalesce[i].load(std::memory_order_relaxed);
		coalesce[i]    = now - s.window_coalesce[i];
		s.window_coalesce[i] = now;
	}
	const double t_s = static_cast<double>(wall_now - s.first_wall_ns) / 1e9;

	std::string line = fmt::format(
	    "{}: t={:.0f}s window={:.2f}s frames={} submits={} ({:.1f}/frame) coalesce: deferred={} "
	    "batch={} deadline={} forced={}\n",
	    s.phases ? (final_report ? "GpuPhase final" : "GpuPhase")
	             : (final_report ? "SubmitStats final" : "SubmitStats"),
	    t_s, wall_ns / 1e9, frames, submits,
	    frames != 0 ? static_cast<double>(submits) / static_cast<double>(frames) : 0.0,
	    coalesce[0], coalesce[1], coalesce[2], coalesce[3]);

	{
		std::array<uint64_t, static_cast<size_t>(KnownFill::Count)> now {};
		bool                                                        any = false;
		for (size_t i = 0; i < now.size(); ++i) {
			now[i] = g_known_fills[i].load(std::memory_order_relaxed);
			any    = any || now[i] != 0;
		}
		if (any) {
			line += fmt::format(
			    "known fills (total): hit={} readback={} discarded={} discard_refused={}\n", now[0],
			    now[1], now[2], now[3]);
		}
	}
	{
		std::array<uint64_t, static_cast<size_t>(KnownStore::Count)> now {};
		bool                                                          any = false;
		for (size_t i = 0; i < now.size(); ++i) {
			now[i] = g_known_stores[i].load(std::memory_order_relaxed);
			any    = any || now[i] != 0;
		}
		if (any) {
			line += fmt::format("known stores (total): recorded={} hit={} partial={} verified={} "
			                    "mismatch={}\n",
			                    now[0], now[1], now[2], now[3], now[4]);
		}
	}
	{
		std::array<uint64_t, static_cast<size_t>(KnownCopy::Count)> now {};
		bool                                                         any = false;
		for (size_t i = 0; i < now.size(); ++i) {
			now[i] = g_known_copies[i].load(std::memory_order_relaxed);
			any    = any || now[i] != 0;
		}
		if (any) {
			line += fmt::format("known copies (total): recorded={} hit={} partial={} verified={} "
			                    "mismatch={}\n",
			                    now[0], now[1], now[2], now[3], now[4]);
		}
	}
	{
		std::array<uint64_t, g_indirect_routes.size()> routes {};
		uint64_t                                       total = 0;
		for (size_t i = 0; i < routes.size(); ++i) {
			routes[i] = g_indirect_routes[i].load(std::memory_order_relaxed);
			total += routes[i];
		}
		if (total != 0) {
			std::string text;
			for (size_t i = 0; i < routes.size(); ++i) {
				if (routes[i] != 0) {
					text += fmt::format(" [{}]={}", IndirectDraw::kRouteNames[i], routes[i]);
				}
			}
			line += fmt::format("indirect draws (total):{}\n", text);
		}
	}
	if (g_clean_reads.load(std::memory_order_relaxed) + g_direct_reads.load(std::memory_order_relaxed) != 0) {
		line += fmt::format("clean guest reads (total): clean={} direct={}\n",
		                    g_clean_reads.load(std::memory_order_relaxed),
		                    g_direct_reads.load(std::memory_order_relaxed));
	}
	if (g_eager[static_cast<size_t>(Eager::Scheduled)].load(std::memory_order_relaxed) +
	        g_eager[static_cast<size_t>(Eager::SkippedNoSpace)].load(std::memory_order_relaxed) != 0) {
		const auto get = [](Eager kind) {
			return g_eager[static_cast<size_t>(kind)].load(std::memory_order_relaxed);
		};
		line += fmt::format("eager readback (total): scheduled={} retired={} skipped_no_space={} "
		                    "verify_checks={} verify_mismatches={}\n",
		                    get(Eager::Scheduled), get(Eager::Retired), get(Eager::SkippedNoSpace),
		                    get(Eager::VerifyChecks), get(Eager::VerifyMismatches));
	}
	if (g_future[static_cast<size_t>(Future::Started)].load(std::memory_order_relaxed) +
	        g_future[static_cast<size_t>(Future::Sync)].load(std::memory_order_relaxed) +
	        g_future[static_cast<size_t>(Future::Contended)].load(std::memory_order_relaxed) +
	        g_future[static_cast<size_t>(Future::Waited)].load(std::memory_order_relaxed) != 0) {
		const auto get = [](Future kind) {
			return g_future[static_cast<size_t>(kind)].load(std::memory_order_relaxed);
		};
		line += fmt::format("readback futures (total): started={} retried={} sync={} exhausted={} contended={} "
		                    "waited={} guest_wait_ms={:.1f}\n",
		                    get(Future::Started), get(Future::Retried), get(Future::Sync),
		                    get(Future::Exhausted), get(Future::Contended), get(Future::Waited),
		                    static_cast<double>(g_future_wait_ns.load(std::memory_order_relaxed)) / 1e6);
	}
	if (g_qr_calls.load(std::memory_order_relaxed) != 0) {
		const auto calls = g_qr_calls.load(std::memory_order_relaxed);
		line += fmt::format("queue readbacks (total): {} flushed_first={} mean_producer_lag={:.1f} ticks\n",
		                    calls, g_qr_flushed.load(std::memory_order_relaxed),
		                    static_cast<double>(g_qr_lag.load(std::memory_order_relaxed)) /
		                        static_cast<double>(calls));
	}
	line += fmt::format("guest barriers: emitted={} ({:.1f}/frame) elided={} ({:.1f}/frame)\n",
	                    barrier_emit_d,
	                    frames != 0 ? static_cast<double>(barrier_emit_d) / static_cast<double>(frames) : 0.0,
	                    barrier_elide_d,
	                    frames != 0 ? static_cast<double>(barrier_elide_d) / static_cast<double>(frames) : 0.0);

	std::array<double, kPhases>   pct {};
	std::array<uint64_t, kPhases> calls {};
	std::array<double, DrainCauses::kCauseNames.size()>   cause_ms {};
	std::array<uint64_t, DrainCauses::kCauseNames.size()> cause_calls {};
	double                        scoped = 0.0;
	if (s.phases) {
		for (size_t i = 0; i < kPhases; ++i) {
			const auto total = s.clock.Total(i) - s.window_total[i];
			pct[i]           = static_cast<double>(total) * ns_per_tick * 100.0 / wall_ns;
			calls[i]         = s.clock.Calls(i) - s.window_calls[i];
			scoped += pct[i];
			s.window_total[i] = s.clock.Total(i);
			s.window_calls[i] = s.clock.Calls(i);
		}
		line += "GpuPhase shares:";
		for (size_t i = 0; i < kPhases; ++i) {
			line += fmt::format(" {}={:.1f}%", kNames[i], pct[i]);
		}
		line += fmt::format(" unscoped={:.1f}%\n", std::max(0.0, 100.0 - scoped));
		{
			// Descriptor family (the former single "descriptors" phase) and its parts.
			constexpr std::array<Phase, 10> kDesc = {Phase::DescBuffers,     Phase::DescUpload,  Phase::DescImages,
			                                         Phase::DescResolve,     Phase::DescFindTexture,
			                                         Phase::DescCommit,      Phase::DescHazard,  Phase::DescWrites,
			                                         Phase::DescTransit,     Phase::DescPush};
			double total = pct[static_cast<size_t>(Phase::Descriptors)];
			for (const auto phase: kDesc) total += pct[static_cast<size_t>(phase)];
			line += fmt::format("GpuPhase descriptors_total={:.1f}% [", total);
			for (const auto phase: kDesc) {
				const auto i = static_cast<size_t>(phase);
				line += fmt::format(" {}={:.1f}% ({}x{})", kNames[i], pct[i], calls[i],
				                    frames != 0 ? fmt::format(", {:.0f}/frame", static_cast<double>(calls[i]) / static_cast<double>(frames)) : std::string());
			}
			line += " ]\n";
			uint64_t scopes = 0;
			for (const auto c: calls) scopes += c;
			if (g_scope_ns > 0.0) {
				line += fmt::format(
				    "GpuPhase timer overhead: {:.1f} ns/scope (tight-loop microbench) x {} scopes = {:.2f}% of wall ({:.0f} scopes/frame)\n",
				    g_scope_ns, scopes, static_cast<double>(scopes) * g_scope_ns * 100.0 / wall_ns,
				    frames != 0 ? static_cast<double>(scopes) / static_cast<double>(frames) : 0.0);
			}
		}

		// Drains: count by cause (the phases are exclusive, so the shares above are pure wait
		// time), and the PM4 packets that were current when they happened.
		line += fmt::format("GpuPhase drains: tick={}x flush={}x finish={}x",
		                    calls[static_cast<size_t>(Phase::DrainTick)],
		                    calls[static_cast<size_t>(Phase::DrainFlush)],
		                    calls[static_cast<size_t>(Phase::DrainFinish)]);
		auto top = [&](const std::array<uint64_t, 256>& now, std::array<uint64_t, 256>& base) {
			std::array<std::pair<uint64_t, size_t>, 256> order {};
			for (size_t i = 0; i < 256; ++i) {
				order[i] = {now[i] - base[i], i};
				base[i]  = now[i];
			}
			std::partial_sort(order.begin(), order.begin() + 4, order.end(),
			                  [](const auto& a, const auto& b) { return a.first > b.first; });
			return order;
		};
		const auto drain_top = top(s.drain_ticks_by_packet, s.window_drain_by_packet);
		line += " by_packet:";
		for (size_t i = 0; i < 4 && drain_top[i].first != 0; ++i) {
			line += fmt::format(" op0x{:02x}={:.1f}ms", drain_top[i].second,
			                    static_cast<double>(drain_top[i].first) * ns_per_tick / 1e6);
		}
		line += "\n";
		{
			const auto causes = s.drain_causes.Since(s.window_drain_causes);
			s.window_drain_causes = s.drain_causes;
			line += "GpuPhase drain causes (calls/full/ms):";
			for (size_t i = 0; i < DrainCauses::kCauseNames.size(); ++i) {
				const auto& e = causes.Get(static_cast<DrainCause>(i));
				if (e.calls != 0) {
					line += fmt::format(" {}={}/{}/{:.1f}", DrainCauses::kCauseNames[i], e.calls,
					                    e.full_calls,
					                    static_cast<double>(e.ticks) * ns_per_tick / 1e6);
				}
			}
			line += "\n";
			if (!s.readbacks.empty()) {
				std::vector<std::pair<uint64_t, detail::ThreadState::Readback>> order(
				    s.readbacks.begin(), s.readbacks.end());
				const size_t keep = std::min<size_t>(5, order.size());
				std::partial_sort(order.begin(), order.begin() + keep, order.end(),
				                  [](const auto& a, const auto& b) {
					                  return a.second.ticks > b.second.ticks;
				                  });
				line += fmt::format("GpuPhase readback regions: {} distinct; top (addr calls KiB/call ms):",
				                    order.size());
				for (size_t i = 0; i < keep; ++i) {
					const auto& r = order[i].second;
					line += fmt::format(" 0x{:x}[{}] {}x {:.1f}KiB {:.1f}ms",
					                    order[i].first & ((1ull << 56) - 1),
					                    DrainCauses::kCauseNames[static_cast<size_t>(r.cause)], r.calls,
					                    r.calls != 0 ? static_cast<double>(r.bytes) / 1024.0 /
					                                       static_cast<double>(r.calls)
					                                 : 0.0,
					                    static_cast<double>(r.ticks) * ns_per_tick / 1e6);
				}
				line += "\n";
				s.readbacks.clear();
			}
			if (!s.fault_sites.empty()) {
				std::vector<std::pair<uint64_t, uint64_t>> order; // hits, hash
				for (const auto& [hash, site]: s.fault_sites) {
					const auto before = s.fault_reported[hash];
					if (site.hits > before) {
						order.emplace_back(site.hits - before, hash);
					}
					s.fault_reported[hash] = site.hits;
				}
				std::sort(order.rbegin(), order.rend());
				for (size_t i = 0; i < std::min<size_t>(order.size(), 4); ++i) {
					const auto& site = s.fault_sites[order[i].second];
					line += fmt::format("GpuPhase GPU-thread fault site {:016x} hits={} frames:",
					                    order[i].second, order[i].first);
					for (size_t f = 2; f < std::min<size_t>(site.offsets.size(), 14); ++f) {
						line += fmt::format(" +0x{:x}", site.offsets[f]);
					}
					line += "\n";
				}
			}
			for (size_t i = 0; i < DrainCauses::kCauseNames.size(); ++i) {
				const auto& e = causes.Get(static_cast<DrainCause>(i));
				cause_ms[i]    = static_cast<double>(e.ticks) * ns_per_tick / 1e6;
				cause_calls[i] = e.calls;
			}
		}
		const auto                count_base = s.window_packet_count;
		const auto                packet_top = top(s.packet_ticks, s.window_packet_ticks);
		line += "GpuPhase packets (inclusive):";
		for (size_t i = 0; i < 4 && packet_top[i].first != 0; ++i) {
			const auto op = packet_top[i].second;
			line += fmt::format(" op0x{:02x}={:.1f}ms/{}x", op,
			                    static_cast<double>(packet_top[i].first) * ns_per_tick / 1e6,
			                    s.packet_count[op] - count_base[op]);
		}
		line += "\n";
		s.window_packet_count = s.packet_count;
	}
	::Log::WriteToConsoleAndLog(line);

	if (s.csv != nullptr) {
		std::string row = fmt::format("{:.1f},{:.3f},{},{},{},{},{},{},{}", t_s, wall_ns / 1e9, frames,
		                              submits, coalesce[0], coalesce[1], coalesce[2], coalesce[3],
		                              Bench::NowNs());
		if (s.phases) {
			for (size_t i = 0; i < kPhases; ++i) {
				row += fmt::format(",{:.2f},{}", pct[i], calls[i]);
			}
			row += fmt::format(",{:.2f}", std::max(0.0, 100.0 - scoped));
			for (size_t i = 0; i < cause_ms.size(); ++i) {
				row += fmt::format(",{:.2f},{}", cause_ms[i], cause_calls[i]);
			}
		}
		row += "\n";
		std::fputs(row.c_str(), s.csv);
		std::fflush(s.csv);
	}

	s.window_wall_ns = wall_now;
	s.window_tsc     = tsc_now;
	s.window_submits += submits;
	s.window_frames += frames;
}

} // namespace

void Poll() {
	auto* s = t_report;
	if (s == nullptr) {
		return;
	}
	const auto interval =
	    static_cast<int64_t>(g_interval_s.load(std::memory_order_relaxed)) * 1'000'000'000;
	const auto cuts = g_cut_requests.load(std::memory_order_relaxed);
	if (interval != 0 && (WallNs() - s->window_wall_ns >= interval || cuts != g_cuts_seen)) {
		g_cuts_seen = cuts;
		Report(*s, false);
	}
}

void RequestCut() noexcept {
	g_cut_requests.fetch_add(1, std::memory_order_relaxed);
}

void Finish() {
	auto* s = t_report;
	if (s == nullptr || !Enabled()) {
		return;
	}
	Report(*s, true);
}

} // namespace Common::GpuPhase
