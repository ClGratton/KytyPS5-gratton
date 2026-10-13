#include "graphics/host_gpu/renderer/skipDepsHooks.h"

#include "common/logging/log.h"
#include "common/speculation.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <sys/prctl.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics::SkipDeps {

namespace {

thread_local uint64_t    t_reader_hash = 0;
thread_local const char* t_reader_op   = "unscoped";

struct RecentRead {
	uint64_t address = 0;
	uint32_t bytes   = 0;
	uint32_t first   = 0;
};
constexpr size_t kRecentReads = 24;
thread_local std::array<RecentRead, kRecentReads> t_recent;
thread_local size_t                               t_recent_count = 0;

std::atomic<int64_t> g_last_dump_ns {0};
std::atomic<int64_t> g_last_guard_summary_ns {0};
std::atomic<uint64_t> g_guard_waits_program {0};
std::atomic<uint64_t> g_guard_waits_pipeline {0};
std::atomic<uint64_t> g_other_waits {0};
std::atomic<uint64_t> g_guard_unqueried {0};
std::atomic<uint64_t> g_guard_queries {0};
std::atomic<int64_t>  g_read_check_ns {0};
thread_local GuardQuery* t_guard_query = nullptr;
std::array<std::atomic<uint64_t>, kReadKinds> g_checked {};
std::array<std::atomic<uint64_t>, kReadKinds> g_hit {};
std::atomic<int64_t> g_last_la_dump_ns {0};
std::atomic<bool>    g_atexit_registered {false};

const char* ThreadName() {
	thread_local char name[24] = {};
	if (name[0] == 0) {
		prctl(PR_GET_NAME, name, 0, 0, 0);
	}
	return name;
}

std::string FormatReads(const std::array<uint32_t, kReadKinds>& reads) {
	std::string text;
	for (size_t k = 0; k < kReadKinds; k++) {
		if (reads[k] == 0) continue;
		char part[48];
		std::snprintf(part, sizeof(part), "%s%s:%u", text.empty() ? "" : "/",
		              ReadKindName(static_cast<ReadKind>(k)), reads[k]);
		text += part;
	}
	return text.empty() ? "none" : text;
}

std::string FormatRoot(const RootStats& root, int64_t now_ns) {
	char text[512];
	std::snprintf(text, sizeof(text),
	              "id=%u skips=%u dep_visual=%u dep_data=%u dep_cpu=%u dep_table=%u misses=%u age_ms=%lld "
	              "reads=%s transitive=%s",
	              root.id, root.skips, root.dependent[0], root.dependent[1], root.dependent[2],
	              root.dependent[3], root.misses,
	              static_cast<long long>(((root.ready_ns != 0 ? root.ready_ns : now_ns) - root.first_ns) /
	                                     1000000),
	              FormatReads(root.reads).c_str(), FormatReads(root.transitive_reads).c_str());
	return text;
}

void Row(const char* event, uint64_t vs, uint64_t ps, const char* rule, const char* draw,
         const std::string& targets) {
	if (!Bench::SkipLogEnabled()) return; // --skip-guard alone feeds the tracker but writes no CSV
	char head[160];
	std::snprintf(head, sizeof(head), "%s,%016" PRIx64 ",%016" PRIx64 ",%s,%s,", event, vs, ps, rule,
	              draw);
	Bench::LogSkipRow(std::string(head) + targets);
}

void DumpAtExit() {
	DumpOpenRoots("exit");
}

void GuardLog(const std::string& text) {
	Log::WriteToConsoleAndLog("SkipGuard: " + text + "\n");
}

} // namespace

// Hooks the recompiler's descriptor diagnostics up to the skip log (the hook itself checks that the
// log is on).
const bool g_diag_registered = [] {
	ShaderRecompiler::IR::SetDescriptorDiagHook(&NoteBadDescriptor);
	return true;
}();

LookaheadProgress& Progress() {
	static LookaheadProgress progress;
	return progress;
}

// A speculative abort ends the lookahead: write when and where to the skip log.
void OnSpeculativeAbort(const char* file, int line, std::string_view text) {
	if (!Bench::SkipLogEnabled()) return;
	std::string row = "spec_abort,0000000000000000,0000000000000000,none,-,";
	row += std::string(file) + ":" + std::to_string(line) + " ";
	for (const char c: text) row += (c == ',' || c == '\n' || c == '\r') ? ' ' : c;
	Bench::LogSkipRow(row);
}

const bool g_observer_registered = [] {
	Common::Speculation::SetAbortObserver(&OnSpeculativeAbort);
	return true;
}();

Tracker& Global() {
	static Tracker tracker;
	return tracker;
}

ReaderScope::ReaderScope(uint64_t shader_hash, const char* op)
    : m_prev_hash(t_reader_hash), m_prev_op(t_reader_op) {
	t_reader_hash = shader_hash;
	t_reader_op   = op;
}

ReaderScope::~ReaderScope() {
	t_reader_hash = m_prev_hash;
	t_reader_op   = m_prev_op;
}

namespace {
std::string FormatHex(uint64_t value) {
	char text[24];
	std::snprintf(text, sizeof(text), "0x%016" PRIx64, value);
	return text;
}
} // namespace

std::string FormatRange(const Range& range) {
	char text[64];
	std::snprintf(text, sizeof(text), "0x%" PRIx64 "+0x%" PRIx64, range.address, range.size);
	return text;
}

void NoteSkippedDraw(const char* event, uint64_t vs, uint64_t ps, const char* draw_name,
                     std::span<const Range> writes, const char* stores, const std::string& targets) {
	if (!g_atexit_registered.exchange(true)) std::atexit(DumpAtExit);
	const auto now = Bench::NowNs();
	const auto key = vs * 0x9e3779b97f4a7c15ull ^ ps;
	const auto id  = Global().NoteSkip(key, vs, ps, writes, now);
	Row(event, vs, ps, stores, draw_name, "id=" + std::to_string(id) + " " + targets);
	auto last = g_last_dump_ns.load(std::memory_order_relaxed);
	if (now - last > 5'000'000'000ll && g_last_dump_ns.compare_exchange_strong(last, now)) {
		DumpOpenRoots("periodic");
	}
}

void NoteDrawReady(uint64_t vs, uint64_t ps) {
	if (!Global().Active()) return;
	const auto now  = Bench::NowNs();
	const auto key  = vs * 0x9e3779b97f4a7c15ull ^ ps;
	const auto done = Global().NoteReady(key, now);
	for (const auto& root: done) {
		Row("ready", root.vs, root.ps, "none", "-", FormatRoot(root, now));
	}
}

bool NoteRead(const Range& range, ReadKind kind, uint64_t shader_hash, const char* op,
              std::vector<Hit>& hits) {
	const auto before = hits.size();
	const bool guard  = GuardEnabled();
	const auto t0     = guard ? Bench::NowNs() : 0;
	// Positive control for the "0 readers of this class" results: how many reads of each kind were
	// checked against live poison, and how many hit.
	g_checked[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
	const bool any = Global().NoteRead(range, kind, shader_hash, hits);
	if (guard) g_read_check_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
	if (!any) return false;
	g_hit[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
	for (auto i = before; i < hits.size(); i++) {
		const auto& hit = hits[i];
		if (hit.newly_unsafe || hit.new_misses != 0) {
			// The loud line: the first hard-class read of a skipped draw's output. It makes the output
			// unsafe (later draws writing it wait) and every skip event of that root up to now was read
			// by a hard reader, a correctness miss.
			std::string roots;
			for (uint32_t r = 0; r < hit.root_count; r++) {
				roots += (r ? "/" : "") + std::to_string(hit.roots[r]);
			}
			char text[640];
			std::snprintf(text, sizeof(text),
			              "hard-class read (%s) of a skipped draw's output: %u range(s) now unsafe, first %s; "
			              "%u skipped draw(s) were read afterwards (correctness miss) | roots=%s derived=%d "
			              "depth=%u root_vs=%016" PRIx64 " root_ps=%016" PRIx64 " reader=%016" PRIx64
			              " op=%s read=%s thread=%s",
			              ReadKindName(kind), hit.unsafe_added,
			              hit.unsafe_added ? FormatRange(hit.unsafe_first).c_str() : "-", hit.new_misses,
			              roots.c_str(), hit.derived ? 1 : 0, hit.depth, hit.root_vs, hit.root_ps,
			              shader_hash, op, FormatRange(range).c_str(), ThreadName());
			GuardLog(text);
		}
		if (!hit.first) continue;
		std::string roots;
		for (uint32_t r = 0; r < hit.root_count; r++) {
			roots += (r ? "/" : "") + std::to_string(hit.roots[r]);
		}
		char text[512];
		std::snprintf(text, sizeof(text),
		              "sink=%s hit=%u derived=%d depth=%u roots=%s root_vs=%016" PRIx64
		              " root_ps=%016" PRIx64 " range=%s thread=%s",
		              SinkName(SinkOf(kind)), hit.entry, hit.derived ? 1 : 0, hit.depth,
		              roots.c_str(), hit.root_vs, hit.root_ps, FormatRange(range).c_str(),
		              ThreadName());
		Row("reader", shader_hash, 0, ReadKindName(kind), op, text);
	}
	return true;
}

void NoteScalarReadSlow(uint64_t address, uint64_t bytes) {
	// Remember the last reads of this thread so a bad descriptor can name where it came from.
	auto& slot = t_recent[t_recent_count++ % kRecentReads];
	slot.address = address;
	slot.bytes   = static_cast<uint32_t>(bytes);
	slot.first   = 0;
	if (!Global().Active()) return;
	std::vector<Hit> hits;
	NoteRead({address, bytes}, ReadKind::ScalarWalk, t_reader_hash, t_reader_op, hits);
}

std::vector<Hit>& PendingHits() {
	thread_local std::vector<Hit> hits;
	return hits;
}

void ConfigureGuard(bool enabled, bool bda) {
	Global().SetGuard(enabled, bda);
	detail::g_guard_enabled.store(enabled, std::memory_order_relaxed);
	if (enabled && !g_atexit_registered.exchange(true)) std::atexit(DumpAtExit);
	if (enabled) {
		GuardLog(std::string("on (hard-class readers: srt walk, indirect args, dma, cpu readback, ") +
		         "index/vertex data; 64-bit-load shaders " + (bda ? "conservative" : "not handled") +
		         "); skip itself is a bridge, a miss is a correctness bug");
	}
}

bool GuardQuery::MustWait() {
	if (m_state < 0) {
		g_guard_queries.fetch_add(1, std::memory_order_relaxed);
		std::vector<Range> writes;
		m_fn(m_context, writes);
		m_state = Global().MustWait(writes) ? 1 : 0;
	}
	return m_state == 1;
}

GuardScope::GuardScope(GuardQuery* query): m_prev(t_guard_query) {
	t_guard_query = query;
}

GuardScope::~GuardScope() {
	t_guard_query = m_prev;
}

bool GuardVetoesSkip() {
	if (!GuardEnabled()) return false;
	if (t_guard_query == nullptr) {
		g_guard_unqueried.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	return t_guard_query->MustWait();
}

void NoteGuardedWait(bool pipeline_stage) {
	(pipeline_stage ? g_guard_waits_pipeline : g_guard_waits_program).fetch_add(1, std::memory_order_relaxed);
}

void NoteOtherWait() {
	g_other_waits.fetch_add(1, std::memory_order_relaxed);
}

void NoteBdaSuspect(uint64_t shader_hash, const char* op, const char* reason,
                    std::span<const Hit> hits) {
	if (!GuardEnabled() || hits.empty()) return;
	const auto added = Global().NoteBdaSuspect(hits);
	if (added.empty()) return;
	std::string ranges;
	for (size_t i = 0; i < added.size() && i < 4; i++) ranges += (i ? " " : "") + FormatRange(added[i]);
	GuardLog("shader " + FormatHex(shader_hash) + " (" + reason + ", op=" + op +
	         ") reads a skipped draw's output and loads through 64-bit addresses (blind class): " +
	         std::to_string(added.size()) + " range(s) now unsafe: " + ranges);
}

void GuardSummary(const char* why) {
	const auto totals = Global().GetTotals();
	const auto counts = Global().GetGuardCounts();
	char       text[900];
	std::snprintf(text, sizeof(text),
	              "summary (%s): skipped=%" PRIu64 " guarded_waits=%" PRIu64 " (program %" PRIu64
	              ", pipeline %" PRIu64 ") other_waits=%" PRIu64 " unqueried_vetoes=%" PRIu64
	              " queries=%" PRIu64 " | unsafe_ranges=%zu%s by_hard_reader=%" PRIu64
	              " by_64bit_loads=%" PRIu64 " | CORRECTNESS MISSES=%" PRIu64 " (through taint %" PRIu64
	              ", roots %" PRIu64 ") hard hits srt=%" PRIu64 " indirect=%" PRIu64 " geometry=%" PRIu64
	              " dma=%" PRIu64 " cpu=%" PRIu64 " | tracker_read_checks_ms=%lld",
	              why, totals.skip_events,
	              g_guard_waits_program.load() + g_guard_waits_pipeline.load(), g_guard_waits_program.load(),
	              g_guard_waits_pipeline.load(), g_other_waits.load(), g_guard_unqueried.load(),
	              g_guard_queries.load(), Global().UnsafeRangeCount(),
	              Global().UnsafeSaturated() ? " SATURATED (everything waits)" : "", counts.hard_ranges,
	              counts.bda_ranges, counts.misses, counts.misses_derived, counts.hard_roots,
	              counts.hard_hits[static_cast<size_t>(ReadKind::ScalarWalk)],
	              counts.hard_hits[static_cast<size_t>(ReadKind::IndirectArgs)],
	              counts.hard_hits[static_cast<size_t>(ReadKind::IndexOrVertex)],
	              counts.hard_hits[static_cast<size_t>(ReadKind::Dma)],
	              counts.hard_hits[static_cast<size_t>(ReadKind::CpuRead)],
	              static_cast<long long>(g_read_check_ns.load() / 1000000));
	GuardLog(text);
}

void NoteUntracked(uint64_t shader_hash, const char* op, const char* reason, bool reads_poison) {
	static std::mutex                           mutex;
	static std::set<std::pair<uint64_t, bool>> seen;
	{
		std::lock_guard lock(mutex);
		if (!seen.emplace(shader_hash, reads_poison).second) return;
	}
	Row("untracked", shader_hash, 0, reason, op, reads_poison ? "reads_poison=1" : "reads_poison=0");
}

void NoteLookaheadCall(const char* what, int64_t start_ns, int64_t duration_ns) {
	if (!Bench::SkipLogEnabled() || duration_ns < 20'000'000) return;
	auto& p = Progress();
	p.slow_calls.fetch_add(1, std::memory_order_relaxed);
	p.slow_total_ns.fetch_add(duration_ns, std::memory_order_relaxed);
	p.last_slow_ns.store(duration_ns, std::memory_order_relaxed);
	p.last_slow_end_ns.store(start_ns + duration_ns, std::memory_order_relaxed);
	const auto la_seq  = p.item_seq.load(std::memory_order_relaxed);
	const auto started = p.gpu_started.load(std::memory_order_relaxed);
	char       text[256];
	std::snprintf(text, sizeof(text),
	              "what=%s duration_ms=%lld la_seq=%" PRIu64 " gpu_started=%" PRIu64
	              " gpu_minus_la=%lld item_age_ms=%lld",
	              what, static_cast<long long>(duration_ns / 1000000), la_seq, started,
	              static_cast<long long>(static_cast<int64_t>(started - la_seq)),
	              static_cast<long long>((start_ns + duration_ns - p.item_start_ns.load()) / 1000000));
	Row("slow_prefetch", 0, 0, "none", "-", text);
}

void NoteLookaheadPeriodic() {
	const auto now  = Bench::NowNs();
	auto       last = g_last_la_dump_ns.load(std::memory_order_relaxed);
	if (now - last > 5'000'000'000ll && g_last_la_dump_ns.compare_exchange_strong(last, now)) {
		DumpLookahead("periodic");
	}
}

void NoteBadDescriptor(uint64_t shader_hash, const char* what, uint32_t slot, const uint32_t* words,
                       uint32_t word_count) {
	if (!Bench::SkipLogEnabled()) return;
	// Rate limit per (thread, what): the first 200 rows of each.
	{
		static std::mutex                                      mutex;
		static std::map<std::pair<std::string, std::string>, uint32_t> counts;
		std::lock_guard lock(mutex);
		if (++counts[{ThreadName(), what}] > 200) return;
	}
	const auto& progress = Progress();
	const auto  la_seq   = progress.item_seq.load(std::memory_order_relaxed);
	const auto  started  = progress.gpu_started.load(std::memory_order_relaxed);
	std::string text = "la_seq=" + std::to_string(la_seq) + " gpu_started=" + std::to_string(started) +
	                   " gpu_minus_la=" + std::to_string(static_cast<int64_t>(started - la_seq)) +
	                   " item_age_ms=" + std::to_string((Bench::NowNs() - progress.item_start_ns.load()) / 1000000) +
	                   " last_slow_ms=" + std::to_string(progress.last_slow_ns.load() / 1000000) +
	                   " last_slow_ended_ms_ago=" +
	                   std::to_string((Bench::NowNs() - progress.last_slow_end_ns.load()) / 1000000) +
	                   " what=" + what + " slot=" + std::to_string(slot) +
	                   " thread=" + ThreadName() + " op=" + t_reader_op +
	                   " open_entries=" + std::to_string(Global().EntryCount()) + " words=";
	for (uint32_t i = 0; i < word_count; i++) {
		char word[16];
		std::snprintf(word, sizeof(word), "%s%08x", i ? ":" : "", words[i]);
		text += word;
	}
	text += " recent_reads=";
	const size_t count = std::min(t_recent_count, kRecentReads);
	for (size_t i = 0; i < count; i++) {
		const auto& read = t_recent[(t_recent_count - count + i) % kRecentReads];
		char        part[48];
		std::snprintf(part, sizeof(part), "%s0x%" PRIx64 "+%u", i ? ":" : "", read.address,
		              read.bytes);
		text += part;
	}
	Row("bad_descriptor", shader_hash, 0, "none", t_reader_op, text);
}

void DumpLookahead(const char* why) {
	const auto& p = Progress();
	char        la[480];
	std::snprintf(la, sizeof(la),
	              "items=%" PRIu64 " items_gpu_started_at_start=%" PRIu64 " draws=%" PRIu64
	              " draws_gpu_started=%" PRIu64 " draws_gpu_passed=%" PRIu64 " gpu_started=%" PRIu64
	              " la_seq=%" PRIu64 " gpu_minus_la=%lld slow_calls=%" PRIu64 " slow_total_ms=%lld",
	              p.items.load(), p.items_gpu_started_at_start.load(), p.draws.load(),
	              p.draws_gpu_started.load(), p.draws_gpu_passed.load(), p.gpu_started.load(),
	              p.item_seq.load(),
	              static_cast<long long>(static_cast<int64_t>(p.gpu_started.load() - p.item_seq.load())),
	              p.slow_calls.load(), static_cast<long long>(p.slow_total_ns.load() / 1000000));
	Row("lookahead", 0, 0, why, "-", la);
}

void DumpOpenRoots(const char* why) {
	const auto now   = Bench::NowNs();
	if (GuardEnabled()) {
		auto last = g_last_guard_summary_ns.load(std::memory_order_relaxed);
		if (why[0] == 'e' || (now - last > 30'000'000'000ll &&
		                      g_last_guard_summary_ns.compare_exchange_strong(last, now))) {
			GuardSummary(why);
		}
	}
	const auto roots = Global().OpenRoots();
	for (const auto& root : roots) {
		Row("open", root.vs, root.ps, why, "-", FormatRoot(root, now));
	}
	DumpLookahead(why);
	const auto totals = Global().GetTotals();
	char       text[256];
	std::snprintf(text, sizeof(text),
	              "skip_events=%" PRIu64 " roots=%" PRIu64 " resolved=%" PRIu64 " dropped=%" PRIu64
	              " dep_visual=%" PRIu64 " dep_data=%" PRIu64 " dep_cpu=%" PRIu64 " dep_table=%" PRIu64
	              " live=%zu",
	              totals.skip_events, totals.entries, totals.resolved, totals.dropped_entries,
	              totals.dependent[0], totals.dependent[1], totals.dependent[2], totals.dependent[3],
	              Global().EntryCount());
	Row("summary", 0, 0, why, "-", text);
	std::string checks;
	for (size_t k = 0; k < kReadKinds; k++) {
		char part[96];
		std::snprintf(part, sizeof(part), "%s%s:%" PRIu64 "/%" PRIu64, k ? " " : "",
		              ReadKindName(static_cast<ReadKind>(k)), g_checked[k].load(), g_hit[k].load());
		checks += part;
	}
	Row("read_checks", 0, 0, why, "-", "checked/hit " + checks);
}

} // namespace Libs::Graphics::SkipDeps
