#include "common/verifyRation.h"
#include "graphics/host_gpu/renderer/cache/knownStores.h"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace {

using namespace Libs::Graphics;
using Op = Libs::Graphics::ShaderRecompiler::IR::StoreExpr::Op;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "KnownStoresTests: failed: %s\n", message);
		std::abort();
	}
}

UniformStoreSite Site(uint32_t block, uint32_t resource, uint32_t offset, uint32_t dwords, uint8_t mask,
                      std::array<uint32_t, 4> value, bool executes = true, uint32_t index = 0) {
	UniformStoreSite s;
	s.block       = block;
	s.resource    = resource;
	s.index       = index;
	s.byte_offset = offset;
	s.dwords      = dwords;
	s.known_mask  = mask;
	s.executes    = executes;
	s.value       = value;
	return s;
}

StoreExpr Expr(Op op, uint16_t a = 0, uint16_t b = 0, uint32_t imm = 0, uint16_t c = 0) {
	StoreExpr e;
	e.op  = op;
	e.a   = a;
	e.b   = b;
	e.c   = c;
	e.imm = imm;
	return e;
}

StoreExpr LoadExpr(Op op, uint16_t resource, uint32_t offset, uint32_t index = 0) {
	StoreExpr e;
	e.op       = op;
	e.resource = resource;
	e.offset   = offset;
	e.index    = index;
	return e;
}

// One block holding all the sites: the shape of a queue-header builder.
UniformStoreProgram Straight(std::vector<UniformStoreSite> sites) {
	UniformStoreProgram p;
	for (auto& s: sites) s.block = 0;
	StoreBlock b;
	b.site_begin = 0;
	b.site_end   = static_cast<uint32_t>(sites.size());
	p.blocks     = {b};
	p.order      = {0};
	p.sites      = std::move(sites);
	return p;
}

struct Result {
	std::map<uint64_t, uint32_t> claims, kept;
	std::vector<SiteState>       states;
};

std::map<uint64_t, uint32_t> Map(const std::vector<std::pair<uint64_t, uint32_t>>& v) {
	return {v.begin(), v.end()};
}

// Memory the reader can answer from; any address not in it is not available without waiting.
using Memory = std::map<uint64_t, uint32_t>;

Result Run(const UniformStoreProgram& program, std::vector<StoreExtent> extents, const Memory& memory = {},
           std::vector<std::pair<uint64_t, uint32_t>> candidates = {}, bool single_wave = true) {
	Result r;
	EvaluateStoreFlow(program, extents, single_wave,
	                  [&](uint64_t address, uint32_t& value) {
		                  const auto it = memory.find(address);
		                  if (it == memory.end()) return false;
		                  value = it->second;
		                  return true;
	                  },
	                  r.states);
	std::vector<std::pair<uint64_t, uint32_t>> claims, kept;
	ResolveKnownStores(program, r.states, extents, candidates, claims, kept);
	r.claims = Map(claims);
	r.kept   = Map(kept);
	return r;
}

// The shape of a queue-header builder: {c0, N, 0, 0} then {0, 0, 0, N} into one 32 byte buffer and
// {3N, 1, 1, 0} into another. Only the immediates are known, N never is.
void TestHeaderBuilder() {
	const auto r = Run(Straight({Site(0, 0, 0, 4, 0b1101, {0xc0, 0, 0, 0}), Site(0, 0, 16, 4, 0b0111, {0, 0, 0, 0}),
	                            Site(0, 1, 0, 4, 0b0110, {0, 1, 1, 0})}),
	                   {{0x1000, 32, 0, true}, {0x2000, 16, 0, true}});
	const auto& known = r.claims;
	Check(known.size() == 8, "three dwords of the first store, three of the second, two of the arguments");
	Check(known.at(0x1000) == 0xc0 && known.at(0x1008) == 0 && known.at(0x100c) == 0, "header dwords 0, 2, 3");
	Check(!known.count(0x1004), "the data dependent count N is not known");
	Check(known.at(0x1010) == 0 && known.at(0x1014) == 0 && known.at(0x1018) == 0, "the zero flag dword at +0x18");
	Check(!known.count(0x101c), "the second count is not known");
	Check(known.at(0x2004) == 1 && known.at(0x2008) == 1 && !known.count(0x2000), "the arguments buffer");
}

void TestExecutionAndRange() {
	auto r = Run(Straight({Site(0, 0, 0, 1, 1, {7, 0, 0, 0}, false)}), {{0x1000, 16, 0, true}});
	Check(r.claims.empty(), "a store that may not execute claims nothing");
	r = Run(Straight({Site(0, 0, 12, 2, 0b11, {1, 2, 0, 0})}), {{0x1000, 16, 0, true}});
	Check(r.claims.size() == 1 && r.claims.at(0x100c) == 1, "the second dword is past num_records: dropped");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {7, 0, 0, 0})}), {{0, 16, 0, true}});
	Check(r.claims.empty(), "a null buffer");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {7, 0, 0, 0})}), {{0x1000, 16, 0, false}});
	Check(r.claims.empty(), "a swizzled, add-tid or unmapped descriptor claims nothing");
	r = Run(Straight({Site(0, 0, 2, 1, 1, {7, 0, 0, 0})}), {{0x1000, 16, 0, true}});
	Check(r.claims.empty(), "an unaligned address claims nothing");
	r = Run(Straight({Site(0, 3, 0, 1, 1, {7, 0, 0, 0})}), {{0x1000, 16, 0, true}});
	Check(r.claims.empty(), "a site whose resource does not exist claims nothing");
	// A strided descriptor: index * stride + offset.
	r = Run(Straight({Site(0, 0, 4, 1, 1, {7, 0, 0, 0}, true, 3)}), {{0x1000, 64, 8, true}});
	Check(r.claims.size() == 1 && r.claims.at(0x1000 + 3 * 8 + 4) == 7, "index times stride plus the offset");
	r = Run(Straight({Site(0, 0, 4, 1, 1, {7, 0, 0, 0}, true, 30)}), {{0x1000, 64, 8, true}});
	Check(r.claims.empty(), "index times stride past the extent");
}

void TestConflicts() {
	const std::vector<StoreExtent> two {{0x1000, 16, 0, true}, {0x1000, 16, 0, true}};
	auto r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 1, {6, 0, 0, 0})}), two);
	Check(r.claims.empty(), "two different constants to one dword");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 1, {5, 0, 0, 0})}), two);
	Check(r.claims.size() == 1 && r.claims.at(0x1000) == 5, "the same constant twice");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 0, {0, 0, 0, 0})}), two);
	Check(r.claims.empty(), "an unknown value stored to the same dword");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 1, {5, 0, 0, 0}, false)}), two);
	Check(r.claims.size() == 1, "an optional store of the same constant is harmless");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 1, {6, 0, 0, 0}, false)}), two);
	Check(r.claims.empty(), "an optional store of another constant");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 0, {0, 0, 0, 0})}),
	        {{0x1000, 16, 0, true}, {0x0f00, 0x200, 0, false}});
	Check(r.claims.empty(), "a strided store overlapping the address");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 0, {0, 0, 0, 0})}),
	        {{0x1000, 16, 0, true}, {0x2000, 0x200, 0, false}});
	Check(r.claims.size() == 1, "a strided store elsewhere");
	r = Run(Straight({Site(0, 0, 0, 1, 1, {5, 0, 0, 0}), Site(0, 1, 0, 1, 1, {5, 0, 0, 0})}),
	        {{0x1000, 16, 0, true}, {0x1002, 16, 0, true}});
	Check(r.claims.empty(), "skewed aliases overlap without being the same dword");
}

void TestKeptDwords() {
	// The known dwords of a bound range that the stores do not reach stay known; the reached ones do not.
	const std::vector<std::pair<uint64_t, uint32_t>> candidates {{0x1000, 9}, {0x1004, 8}, {0x1018, 0}, {0x101c, 3}};
	auto r = Run(Straight({Site(0, 0, 0, 2, 0b01, {0xc0, 0, 0, 0})}), {{0x1000, 32, 0, true}}, {}, candidates);
	Check(r.kept.size() == 2 && r.kept.count(0x1018) && r.kept.count(0x101c), "dwords outside the stores are kept");
	Check(r.claims.size() == 1 && r.claims.at(0x1000) == 0xc0, "the claimed dword has its new value");
	r = Run(Straight({Site(0, 0, 0, 1, 0, {0, 0, 0, 0})}), {{0x1000, 32, 0, false}}, {}, candidates);
	Check(r.kept.empty(), "a store through a descriptor that is not direct may reach all of it");
}

// The shape of a conditional flag initialiser: if the state bit is set and the header is not yet
// initialised, set it up. Resources: 0 state dwords, 1 bit index (scalar), 2 header, 3 flag buffer.
//   b0: cond bit == 0 ? b1 : b2      b1: header[0] = 0 (end)
//   b2: cond header[0] != 0 ? b4 : b3     b3: header[0] = 0xc0; flag, count = 1, 64      b4: end
UniformStoreProgram Initialiser() {
	UniformStoreProgram p;
	p.exprs = {
	    LoadExpr(Op::Load, 0, 0),            // 0: state dword
	    LoadExpr(Op::ScalarLoad, 1, 0),      // 1: bit index
	    Expr(Op::Imm, 0, 0, 31),             // 2
	    Expr(Op::And, 1, 2),                 // 3: index & 31
	    Expr(Op::Imm, 0, 0, 1),              // 4
	    Expr(Op::Shl, 4, 3),                 // 5: 1 << index
	    Expr(Op::And, 5, 0),                 // 6: mask & state
	    Expr(Op::Imm, 0, 0, 0),              // 7
	    Expr(Op::Uge, 7, 6),                 // 8: 0 >= x, i.e. the bit is clear
	    LoadExpr(Op::Load, 2, 0),            // 9: header[0]
	    Expr(Op::Ne, 7, 9),                  // 10: header[0] != 0
	};
	p.sites = {Site(1, 2, 0, 1, 1, {0, 0, 0, 0}), Site(3, 2, 0, 1, 1, {0xc0, 0, 0, 0}),
	           Site(3, 3, 24, 2, 0b11, {1, 64, 0, 0})};
	p.blocks.resize(5);
	p.blocks[0] = {1, 2, 8, 0, 0};
	p.blocks[1] = {StoreBlock::kNone, StoreBlock::kNone, -1, 0, 1};
	p.blocks[2] = {4, 3, 10, 1, 1};
	p.blocks[3] = {4, StoreBlock::kNone, -1, 1, 3};
	p.blocks[4] = {StoreBlock::kNone, StoreBlock::kNone, -1, 3, 3};
	p.blocks[3].succ_true = 4;
	p.order = {0, 2, 1, 3, 4};
	return p;
}

void TestConditionalInitialiser() {
	const auto p = Initialiser();
	const std::vector<StoreExtent> ext {{0x100, 16, 0, true}, {0x200, 16, 0, true}, {0x300, 32, 0, true}, {0x300, 32, 0, true}};
	const std::vector<std::pair<uint64_t, uint32_t>> candidates {{0x318, 0}, {0x31c, 7}, {0x300, 0xc0}, {0x304, 5}};
	// Header already initialised: whatever the state bit says, the init path does not run, the flag stays.
	Memory initialised {{0x300, 0xc0}};
	auto r = Run(p, ext, initialised, candidates);
	Check(r.states[2] == SiteState::Unreached && r.states[1] == SiteState::Unreached, "the init path is not reachable");
	Check(r.states[0] == SiteState::Maybe, "the clear path may run: the state bit is unknown");
	Check(r.kept.count(0x318) && r.kept.at(0x318) == 0 && r.kept.count(0x31c) && r.kept.count(0x304),
	      "the flag and the counts stay known");
	Check(!r.kept.count(0x300), "the header dword may be cleared by the clear path");
	Check(r.claims.empty(), "nothing is claimed when the clear path only may run");
	// The state bit is known too: bit clear -> the clear path surely runs.
	initialised[0x100] = 0;
	initialised[0x200] = 3;
	r = Run(p, ext, initialised, candidates);
	Check(r.states[0] == SiteState::Definite && r.claims.size() == 1 && r.claims.at(0x300) == 0,
	      "the clear path surely runs and stores a known zero");
	// Bit set, header not initialised: the init path surely runs.
	Memory fresh {{0x100, 8}, {0x200, 3}, {0x300, 0}};
	r = Run(p, ext, fresh, candidates);
	Check(r.states[0] == SiteState::Unreached && r.states[1] == SiteState::Definite && r.states[2] == SiteState::Definite,
	      "the init path surely runs");
	Check(r.claims.size() == 3 && r.claims.at(0x300) == 0xc0 && r.claims.at(0x318) == 1 && r.claims.at(0x31c) == 64,
	      "it claims what it stores");
	Check(r.kept.count(0x304) && !r.kept.count(0x318), "an untouched dword stays, the flag is claimed");
	// Header unreadable (GPU-written, unknown): everything may run, nothing is kept or claimed there.
	r = Run(p, ext, Memory {{0x100, 8}, {0x200, 3}}, candidates);
	Check(r.states[0] == SiteState::Unreached && r.states[1] == SiteState::Maybe && r.states[2] == SiteState::Maybe,
	      "an unreadable header leaves the init path undecided");
	Check(!r.kept.count(0x318) && r.claims.empty(), "so the flag is no longer known");
	// More than one wave: a load is never evaluated (another wave's store could race it).
	r = Run(p, ext, initialised, candidates, false);
	Check(r.states[1] == SiteState::Maybe && r.states[2] == SiteState::Maybe && !r.kept.count(0x318),
	      "a multi-wave dispatch decides nothing from memory");
}

void TestExpressions() {
	UniformStoreProgram p;
	p.exprs = {Expr(Op::Imm, 0, 0, 5), Expr(Op::Imm, 0, 0, 0), LoadExpr(Op::Load, 0, 0),
	           Expr(Op::And, 1, 2),                 // 3: 0 & unknown = 0
	           Expr(Op::LogicalAnd, 1, 2),          // 4: false && unknown
	           Expr(Op::LogicalOr, 0, 2),           // 5: true-ish || unknown
	           Expr(Op::Shl, 0, 0, 0),              // 6: 5 << 5 = 160
	           Expr(Op::Imm, 0, 0, 40),             // 7
	           Expr(Op::Shl, 0, 7),                 // 8: shift by 40: undefined, unknown
	           Expr(Op::Select, 2, 0, 0, 1),        // 9: select(unknown, 5, 0): arms differ
	           Expr(Op::Select, 2, 0, 0, 0),        // 10: select(unknown, 5, 5): arms agree
	           Expr(Op::Sub, 1, 0)};                // 11: 0 - 5 wraps
	StoreBlock b;
	p.blocks = {b};
	p.order  = {0};
	const std::vector<StoreExtent> ext {{0x100, 16, 0, true}};
	const detail::Evaluator ev {p, ext, true};
	auto read = [](uint64_t, uint32_t&) { return false; };
	uint32_t v = 0;
	Check(ev.Eval(3, read, v) && v == 0, "x & 0 is 0 whatever x is");
	Check(ev.Eval(4, read, v) && v == 0, "false && x is false");
	Check(ev.Eval(5, read, v) && v == 1, "true || x is true");
	Check(!ev.Eval(2, read, v), "an unreadable load is unknown");
	Check(!ev.Eval(8, read, v), "an oversized shift is unknown");
	Check(!ev.Eval(9, read, v), "a select between different arms with an unknown condition");
	Check(ev.Eval(10, read, v) && v == 5, "a select between equal arms");
	Check(ev.Eval(11, read, v) && v == 0xfffffffbu, "unsigned wrap");
	Check(!ev.Eval(99, read, v) && !ev.Eval(-1, read, v), "a node out of range");
	auto readable = [](uint64_t a, uint32_t& value) { value = static_cast<uint32_t>(a); return a == 0x100; };
	Check(ev.Eval(2, readable, v) && v == 0x100, "a load reads base + offset");
	const detail::Evaluator multi {p, ext, false};
	Check(!multi.Eval(2, readable, v), "a multi-wave load is unknown");
	const std::vector<StoreExtent> oob {{0x100, 2, 0, true}};
	const detail::Evaluator small {p, oob, true};
	Check(!small.Eval(2, readable, v), "a load past the descriptor extent is unknown");
}

void TestVerifyRation() {
	Common::VerifyRation r;
	constexpr int64_t s = 1'000'000'000;
	Check(!r.Due(5 * s, 0), "0 seconds never verifies");
	Check(r.Due(5 * s, 3), "the first call verifies");
	Check(!r.Due(5 * s + 1, 3) && !r.Due(7 * s, 3), "within the interval: no");
	Check(r.Due(8 * s, 3), "after the interval: yes");
	Check(!r.Due(8 * s + 1, 3), "and once");
	Common::VerifyRation t;
	Check(t.Due(-10 * s, 1) && !t.Due(-10 * s + 5, 1) && t.Due(-9 * s, 1), "negative clocks work");
}

} // namespace

int main() {
	TestHeaderBuilder();
	TestExecutionAndRange();
	TestConflicts();
	TestKeptDwords();
	TestConditionalInitialiser();
	TestExpressions();
	TestVerifyRation();
	std::printf("KnownStoresTests: ok\n");
	return 0;
}
