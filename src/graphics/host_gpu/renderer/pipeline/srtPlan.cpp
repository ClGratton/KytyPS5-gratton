#include "graphics/host_gpu/renderer/pipeline/srtPlan.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>

namespace Libs::Graphics::SrtPlan {

bool Store::DeepPrefetchEnabled() {
	return Config::PlanPrefetchEnabled();
}

bool PoolEnabled() {
	return Config::PlanPoolEnabled();
}

bool Store::GuestPrefetchEnabled() {
	return Config::CpPrefetchEnabled();
}

void Store::PrefetchGuestLines(uint64_t address, uint32_t bytes) {
	Libs::LibKernel::Memory::PrefetchBacking(address, bytes);
}

namespace {

// The queue holds about two frames of a busy level (12k plans per frame); beyond that the lookahead
// is so far ahead that its oldest plans are useless anyway.
constexpr size_t kMaxPlans = 32768;
constexpr size_t kMaxBytes = 192u << 20u;

constexpr int64_t kPeriodNs = 10'000'000'000;

std::atomic<uint64_t> g_producer_events {0};
std::atomic<uint64_t> g_lookup_events {0}; // single writer (kyGpu): plain read-modify-write, relaxed
std::atomic<uint64_t> g_lookup_misses {0};
// The producer's counters are per thread (several prefetch workers produce plans at once, and one
// shared counter per plan would bounce between their caches); the summary adds the shards up. A shard is
// written by its own thread only and never freed.
struct ProducerShard {
	std::atomic<uint64_t> produced {0};
	std::atomic<uint64_t> bytes {0};
	std::atomic<uint64_t> failed {0};
	std::atomic<int64_t>  ns {0};
};
constexpr size_t              kMaxProducerShards = 64;
ProducerShard                 g_shards[kMaxProducerShards];
std::atomic<size_t>           g_shard_count {0};

ProducerShard& MyShard() {
	thread_local ProducerShard* shard = [] {
		const auto index = g_shard_count.fetch_add(1, std::memory_order_acq_rel);
		return &g_shards[std::min(index, kMaxProducerShards - 1)]; // beyond 64 threads they share the last one
	}();
	return *shard;
}

uint64_t ShardSum(std::atomic<uint64_t> ProducerShard::*field) {
	uint64_t total = 0;
	const auto n = std::min(g_shard_count.load(std::memory_order_acquire), kMaxProducerShards);
	for (size_t i = 0; i < n; ++i) total += (g_shards[i].*field).load(std::memory_order_relaxed);
	return total;
}

int64_t ShardNs() {
	int64_t total = 0;
	const auto n = std::min(g_shard_count.load(std::memory_order_acquire), kMaxProducerShards);
	for (size_t i = 0; i < n; ++i) total += g_shards[i].ns.load(std::memory_order_relaxed);
	return total;
}

struct ConsumerState {
	Stats          stats;
	AdoptTotals    adopt_window; // --srt-plans: since the last report
	AdoptTotals    adopt_total;
	std::vector<std::unique_ptr<Plan>> dead; // finished plans, handed back in batches
	int64_t        start_ns   = 0;
	int64_t        last_print = 0;
	std::FILE*     csv        = nullptr; // one row per drain interval
	bool           csv_failed = false;
	std::FILE*     wcsv       = nullptr; // one row per report window
	uint64_t       drains     = 0;
};

ConsumerState& Consumer() {
	static ConsumerState state;
	return state;
}

std::string Pct(uint64_t part, uint64_t whole) {
	return whole == 0 ? "n/a" : fmt::format("{:.1f}%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

std::string Lead(const Percentiles& p) {
	return fmt::format("{:.0f}/{:.0f}/{:.0f}", p.p10, p.p50, p.p90);
}

std::string FormatWindow(const char* title, double seconds, const Stats::Window& w, const Stats& stats,
                         bool with_intervals) {
	const auto& t = w.totals;
	const auto  outcome = [&](Outcome o) { return t.outcome[static_cast<size_t>(o)]; };
	const uint64_t ready    = outcome(Outcome::Ready);
	const uint64_t eligible = t.events - outcome(Outcome::SubDraw) - outcome(Outcome::NewSource);
	std::string line = fmt::format(
	    "SrtPlanShadow {} {:.1f}s: events={} eligible={} ready={} ({}) [gfx={} cs={}]; not_ready={} missing={} late={} "
	    "evicted={} desync={} subdraw={} new_source={}\n",
	    title, seconds, t.events, eligible, ready, Pct(ready, eligible), t.ready_graphics, t.ready_compute,
	    outcome(Outcome::NotReady), outcome(Outcome::Missing), outcome(Outcome::Late), outcome(Outcome::Evicted),
	    outcome(Outcome::Desync), outcome(Outcome::SubDraw), outcome(Outcome::NewSource));
	if (ready == 0) return line;
	line += fmt::format(
	    "SrtPlanShadow   of ready: valid={} ({}) byte_equal={} ({}) valid_and_unequal={} invalid_but_equal={}; invalid by reason:",
	    t.valid, Pct(t.valid, ready), t.equal, Pct(t.equal, ready), t.valid_unequal, t.invalid_equal);
	for (size_t i = 1; i < t.invalid.size(); ++i) {
		line += fmt::format(" {}={}", kInvalidNames[i], t.invalid[i]);
	}
	line += "; diff bits (flat,buf,img,smp,user,reads,fill,spec):";
	for (const auto v: t.diff_bits) line += fmt::format(" {}", v);
	line += "\n";
	line += fmt::format("SrtPlanShadow   lead p10/p50/p90: {} us, {} draws", Lead(w.lead_us), Lead(w.lead_draws));
	if (with_intervals) {
		line += fmt::format("; per drain interval (n={}): median lead {} us (p10 of intervals' p10 {:.0f}, p90 of p90 {:.0f}), {} draws",
		                    stats.IntervalMedianLeadUs().n, Lead(stats.IntervalMedianLeadUs()),
		                    stats.IntervalP10LeadUs().p10, stats.IntervalP90LeadUs().p90,
		                    Lead(stats.IntervalMedianLeadDraws()));
	}
	line += "\n";
	const double n = static_cast<double>(ready);
	line += fmt::format(
	    "SrtPlanShadow   cost per ready draw: check {:.0f} ns (p50 {:.0f}) vs kyGpu materialize {:.0f} ns (p50 {:.0f}) => "
	    "check/materialize {} (p50 ratio {}); store lookup {:.0f} ns/event, shadow compare {:.0f} ns; read set: {:.1f} "
	    "windows ({:.2f} clean), {:.1f} runs, {:.1f} dwords per plan\n",
	    static_cast<double>(t.check_ns) / n, w.check_ns.p50, static_cast<double>(t.materialize_ns) / n,
	    w.materialize_ns.p50,
	    t.materialize_ns != 0 ? fmt::format("{:.1f}%", 100.0 * static_cast<double>(t.check_ns) / static_cast<double>(t.materialize_ns)) : "n/a",
	    w.materialize_ns.p50 != 0 ? fmt::format("{:.1f}%", 100.0 * w.check_ns.p50 / w.materialize_ns.p50) : "n/a",
	    static_cast<double>(t.lookup_ns) / static_cast<double>(std::max<uint64_t>(t.events, 1)),
	    static_cast<double>(t.compare_ns) / n, static_cast<double>(t.windows) / n,
	    static_cast<double>(t.clean_windows) / n,
	    static_cast<double>(t.runs) / n, static_cast<double>(t.dwords) / n);
	return line;
}

double WallS() {
	return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// One row per report window (for analysis restricted to part of a run: wall_s is the end of the
// window, epoch seconds). Percentiles are of the window; sums are exact.
void WriteWindowRow(ConsumerState& c, const Stats::Window& w, double seconds) {
	if (c.wcsv == nullptr) {
		c.wcsv = std::fopen("_srt_plan_windows.csv", "w");
		if (c.wcsv == nullptr) return;
		std::fputs("wall_s,window_s,events,eligible,ready,gfx_ready,cs_ready,not_ready,missing,late,evicted,desync,"
		           "subdraw,new_source,valid,equal,valid_unequal,invalid_equal,inv_inputs,inv_changed,inv_unreadable,"
		           "inv_dirty,lead_us_p10,lead_us_p50,lead_us_p90,lead_draws_p10,lead_draws_p50,lead_draws_p90,"
		           "check_ns_sum,check_ns_p50,mat_ns_sum,mat_ns_p50,lookup_ns_sum,compare_ns_sum,windows_sum,runs_sum,"
		           "dwords_sum,store_queued,store_evictions,produced,produce_failed,produce_ns_sum,clean_windows_sum\n",
		           c.wcsv);
	}
	const auto& t = w.totals;
	const auto  o = [&](Outcome x) { return static_cast<unsigned long long>(t.outcome[static_cast<size_t>(x)]); };
	const auto  store = GetStore().GetCounters();
	const unsigned long long eligible = t.events - o(Outcome::SubDraw) - o(Outcome::NewSource);
	std::fprintf(
	    c.wcsv,
	    "%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
	    "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%lld,%.0f,%lld,%.0f,%lld,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%llu\n",
	    WallS(), seconds, static_cast<unsigned long long>(t.events), eligible, o(Outcome::Ready),
	    static_cast<unsigned long long>(t.ready_graphics), static_cast<unsigned long long>(t.ready_compute),
	    o(Outcome::NotReady), o(Outcome::Missing), o(Outcome::Late), o(Outcome::Evicted), o(Outcome::Desync),
	    o(Outcome::SubDraw), o(Outcome::NewSource), static_cast<unsigned long long>(t.valid),
	    static_cast<unsigned long long>(t.equal), static_cast<unsigned long long>(t.valid_unequal),
	    static_cast<unsigned long long>(t.invalid_equal), static_cast<unsigned long long>(t.invalid[1]),
	    static_cast<unsigned long long>(t.invalid[2]), static_cast<unsigned long long>(t.invalid[3]),
	    static_cast<unsigned long long>(t.invalid[4]), w.lead_us.p10, w.lead_us.p50, w.lead_us.p90,
	    w.lead_draws.p10, w.lead_draws.p50, w.lead_draws.p90, static_cast<long long>(t.check_ns), w.check_ns.p50,
	    static_cast<long long>(t.materialize_ns), w.materialize_ns.p50, static_cast<long long>(t.lookup_ns),
	    static_cast<long long>(t.compare_ns), static_cast<unsigned long long>(t.windows),
	    static_cast<unsigned long long>(t.runs), static_cast<unsigned long long>(t.dwords),
	    static_cast<unsigned long long>(store.queued), static_cast<unsigned long long>(store.evictions),
	    static_cast<unsigned long long>(ShardSum(&ProducerShard::produced)), static_cast<unsigned long long>(ShardSum(&ProducerShard::failed)),
	    static_cast<long long>(ShardNs()), static_cast<unsigned long long>(t.clean_windows));
	std::fflush(c.wcsv);
}

std::string FormatAdopt(const char* title, double seconds, const AdoptTotals& t) {
	const auto outcome = [&](Outcome o) { return t.outcome[static_cast<size_t>(o)]; };
	const uint64_t eligible = t.events - outcome(Outcome::SubDraw) - t.new_source - t.call_source;
	std::string line = fmt::format(
	    "SrtPlans {} {:.1f}s: draws={} eligible={} adopted={} ({}); plan found={} not_ready={} missing={} late={} evicted={} "
	    "desync={} subdraw={} new_source={} call_source={}; unusable plans:",
	    title, seconds, t.events, eligible, t.adopted, Pct(t.adopted, eligible), outcome(Outcome::Ready),
	    outcome(Outcome::NotReady), outcome(Outcome::Missing), outcome(Outcome::Late), outcome(Outcome::Evicted),
	    outcome(Outcome::Desync), outcome(Outcome::SubDraw), t.new_source, t.call_source);
	for (size_t i = 1; i < t.invalid.size(); ++i) line += fmt::format(" {}={}", kInvalidNames[i], t.invalid[i]);
	if (t.verified != 0) {
		line += fmt::format("; verified against kyGpu's own result: {} mismatched={} diff bits (flat,buf,img,smp,user,reads,fill,spec):",
		                    t.verified, t.mismatched);
		for (const auto v: t.diff_bits) line += fmt::format(" {}", v);
	}
	return line + "\n";
}

std::string PoolText() {
	const auto pool = GetPlanPool().GetCounters();
	if (pool.acquired == 0 && pool.given == 0 && pool.freed == 0) return {};
	return fmt::format("SrtPlan pool: acquired={} (new={}, {}% reused) given back={} freed={} pooled now={}\n", pool.acquired,
	                   pool.created, pool.acquired != 0 ? 100 - 100 * pool.created / pool.acquired : 0, pool.given,
	                   pool.freed, GetPlanPool().Pooled());
}

std::string ProducerText() {
	auto counters = GetStore().GetCounters();
	return fmt::format(
	    "SrtPlan producer: events={} plans={} ({:.1f} MB total) failed={} produce_cost={:.0f} ns/plan; queue: put={} "
	    "evicted={} queued={} ({:.1f} MB)\n",
	    g_producer_events.load(), ShardSum(&ProducerShard::produced), static_cast<double>(ShardSum(&ProducerShard::bytes)) / 1048576.0,
	    ShardSum(&ProducerShard::failed),
	    ShardSum(&ProducerShard::produced) != 0 ? static_cast<double>(ShardNs()) / static_cast<double>(ShardSum(&ProducerShard::produced)) : 0.0,
	    counters.put, counters.evictions, counters.queued, static_cast<double>(counters.queued_bytes) / 1048576.0) +
	    PoolText();
}

} // namespace

bool ShadowEnabled() {
	return Config::SrtPlanShadowEnabled();
}

bool AdoptEnabled() {
	return Config::SrtPlansEnabled();
}

bool Enabled() {
	return ShadowEnabled() || AdoptEnabled();
}

AdoptTotals& Adopting() {
	return Consumer().adopt_window;
}

void NoteLookup(Outcome outcome) {
	if (outcome == Outcome::SubDraw || outcome == Outcome::NewSource) return;
	g_lookup_events.store(g_lookup_events.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	if (outcome == Outcome::NotReady || outcome == Outcome::Late || outcome == Outcome::Missing) {
		g_lookup_misses.store(g_lookup_misses.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}
}

LookupCounts LookupSnapshot() {
	return {g_lookup_events.load(std::memory_order_relaxed), g_lookup_misses.load(std::memory_order_relaxed)};
}

void RecyclePlan(std::unique_ptr<Plan> plan) {
	auto& dead = Consumer().dead;
	dead.push_back(std::move(plan));
	if (dead.size() >= 128) GetStore().Recycle(std::move(dead));
}

Store& GetStore() {
	static Store store(kMaxPlans, kMaxBytes);
	return store;
}

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

void NoteProducerEvent() {
	g_producer_events.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ProducerEvents() {
	return g_producer_events.load(std::memory_order_relaxed);
}

void NoteProduced(uint64_t bytes) {
	auto& shard = MyShard();
	shard.produced.fetch_add(1, std::memory_order_relaxed);
	shard.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteProduceFailed() {
	MyShard().failed.fetch_add(1, std::memory_order_relaxed);
}

void NoteProduceCost(int64_t ns) {
	MyShard().ns.fetch_add(ns, std::memory_order_relaxed);
}

void Record(const Sample& sample) {
	Consumer().stats.Add(sample);
}

void NoteDrain() {
	auto& c = Consumer();
	const auto record = c.stats.EndInterval();
	++c.drains;
	if (record.events == 0) return;
	if (c.csv == nullptr && c.start_ns != 0 && !c.csv_failed) {
		c.csv = std::fopen("_srt_plan.csv", "w");
		if (c.csv == nullptr) {
			c.csv_failed = true; // say so once instead of every drain
			::Log::WriteToConsoleAndLog(fmt::format("SrtPlan: cannot create _srt_plan.csv: {}\n", std::strerror(errno)));
		} else {
			std::fputs("wall_s,events,ready,valid,equal,lead_us_p10,lead_us_p50,lead_us_p90,lead_draws_p10,lead_draws_p50,"
			           "lead_draws_p90\n",
			           c.csv);
		}
	}
	if (c.csv != nullptr) {
		std::fprintf(c.csv, "%.3f,%llu,%llu,%llu,%llu,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",
		             WallS(), static_cast<unsigned long long>(record.events),
		             static_cast<unsigned long long>(record.ready), static_cast<unsigned long long>(record.valid),
		             static_cast<unsigned long long>(record.equal), record.lead_us.p10, record.lead_us.p50,
		             record.lead_us.p90, record.lead_draws.p10, record.lead_draws.p50, record.lead_draws.p90);
	}
}

void Poll() {
	auto&      c   = Consumer();
	const auto now = NowNs();
	if (c.start_ns == 0) {
		c.start_ns   = now;
		c.last_print = now;
		return;
	}
	if (now - c.last_print < kPeriodNs) return;
	const double seconds = static_cast<double>(now - c.last_print) / 1e9;
	c.last_print         = now;
	if (c.adopt_window.events != 0) {
		const auto window = c.adopt_window;
		c.adopt_total.Add(window);
		c.adopt_window = {};
		::Log::WriteToConsoleAndLog(FormatAdopt("window", seconds, window) + ProducerText());
	}
	const auto window    = c.stats.TakeWindow();
	if (window.totals.events == 0) return;
	WriteWindowRow(c, window, seconds);
	::Log::WriteToConsoleAndLog(FormatWindow("window", seconds, window, c.stats, false) + ProducerText());
	if (c.csv != nullptr) std::fflush(c.csv);
}

std::string SummaryText() {
	auto& c = Consumer();
	const auto tail = c.stats.TakeWindow(); // absorb what is left of the last window
	if (tail.totals.events != 0) {
		WriteWindowRow(c, tail, c.start_ns != 0 ? static_cast<double>(NowNs() - c.last_print) / 1e9 : 0.0);
	}
	const auto total = c.stats.TotalWindow();
	const double seconds = c.start_ns != 0 ? static_cast<double>(NowNs() - c.start_ns) / 1e9 : 0.0;
	c.adopt_total.Add(c.adopt_window);
	c.adopt_window = {};
	std::string text;
	if (c.adopt_total.events != 0) text += FormatAdopt("total", seconds, c.adopt_total);
	if (ShadowEnabled() && !AdoptEnabled()) {
		text += fmt::format("drain intervals: {}\n", c.drains);
		text += FormatWindow("total", seconds, total, c.stats, true);
	}
	text += ProducerText();
	if (c.csv != nullptr) std::fflush(c.csv);
	return text;
}

} // namespace Libs::Graphics::SrtPlan
