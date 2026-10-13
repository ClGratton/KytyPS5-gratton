#include "graphics/host_gpu/renderer/skipDepsHooks.h"

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <sys/prctl.h>

#include <algorithm>
#include <array>
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
	              "id=%u skips=%u dep_visual=%u dep_data=%u dep_table=%u age_ms=%lld reads=%s "
	              "transitive=%s",
	              root.id, root.skips, root.dependent[0], root.dependent[1], root.dependent[2],
	              static_cast<long long>(((root.ready_ns != 0 ? root.ready_ns : now_ns) - root.first_ns) /
	                                     1000000),
	              FormatReads(root.reads).c_str(), FormatReads(root.transitive_reads).c_str());
	return text;
}

void Row(const char* event, uint64_t vs, uint64_t ps, const char* rule, const char* draw,
         const std::string& targets) {
	char head[160];
	std::snprintf(head, sizeof(head), "%s,%016" PRIx64 ",%016" PRIx64 ",%s,%s,", event, vs, ps, rule,
	              draw);
	Bench::LogSkipRow(std::string(head) + targets);
}

void DumpAtExit() {
	DumpOpenRoots("exit");
}

} // namespace

// Hooks the recompiler's descriptor diagnostics up to the skip log (the hook itself checks that the
// log is on).
const bool g_diag_registered = [] {
	ShaderRecompiler::IR::SetDescriptorDiagHook(&NoteBadDescriptor);
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
	if (!Global().NoteRead(range, kind, shader_hash, hits)) return false;
	for (auto i = before; i < hits.size(); i++) {
		const auto& hit = hits[i];
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

void NoteScalarRead(uint64_t address, uint64_t bytes) {
	if (!Bench::SkipLogEnabled()) return;
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

void NoteUntracked(uint64_t shader_hash, const char* op, const char* reason, bool reads_poison) {
	static std::mutex                           mutex;
	static std::set<std::pair<uint64_t, bool>> seen;
	{
		std::lock_guard lock(mutex);
		if (!seen.emplace(shader_hash, reads_poison).second) return;
	}
	Row("untracked", shader_hash, 0, reason, op, reads_poison ? "reads_poison=1" : "reads_poison=0");
}

void NoteBadDescriptor(uint64_t shader_hash, const char* what, uint32_t slot, const uint32_t* words,
                       uint32_t word_count) {
	if (!Bench::SkipLogEnabled()) return;
	std::string text = std::string("what=") + what + " slot=" + std::to_string(slot) +
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

void DumpOpenRoots(const char* why) {
	const auto now   = Bench::NowNs();
	const auto roots = Global().OpenRoots();
	for (const auto& root : roots) {
		Row("open", root.vs, root.ps, why, "-", FormatRoot(root, now));
	}
	const auto totals = Global().GetTotals();
	char       text[256];
	std::snprintf(text, sizeof(text),
	              "skip_events=%" PRIu64 " roots=%" PRIu64 " resolved=%" PRIu64 " dropped=%" PRIu64
	              " dep_visual=%" PRIu64 " dep_data=%" PRIu64 " dep_table=%" PRIu64 " live=%zu",
	              totals.skip_events, totals.entries, totals.resolved, totals.dropped_entries,
	              totals.dependent[0], totals.dependent[1], totals.dependent[2],
	              Global().EntryCount());
	Row("summary", 0, 0, why, "-", text);
}

} // namespace Libs::Graphics::SkipDeps
