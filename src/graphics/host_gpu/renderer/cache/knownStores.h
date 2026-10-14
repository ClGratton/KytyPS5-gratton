#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNSTORES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNSTORES_H_

// --known-stores: which guest dwords a compute dispatch is proven to leave with a constant value,
// and which dwords of the ranges it binds for writing it is proven not to touch. Pure logic, unit
// tested in tests/KnownStoresTests.cpp. The shader side is ShaderRecompiler::IR::UniformStoreProgram
// (every memory write of the shader is a store whose address the descriptor alone decides, the
// control flow between them is acyclic, and its branch conditions are small expressions); the
// renderer resolves the descriptors, evaluates the program with the memory it can read without
// waiting for the GPU, and records the result in the buffer cache's known-store table (KnownFills).
// The GPU thread consults that table instead of draining the GPU when it must read a GPU-written
// dword itself (the SRT walk).
//
// Exactness. A dword is claimed only when every writer of its address is accounted for:
//   * the store is a direct raw store (no swizzle, no add-tid, fully mapped) in range of the
//     descriptor (a store beyond num_records is dropped on the GPU and is not claimed),
//   * its block provably runs (every branch to it was decided from values known exactly) and its
//     guard executes on at least one lane, and its dword is an immediate,
//   * no other store that may run can reach that address. A store through a descriptor that is not
//     direct is taken to reach its whole buffer range; a store in a block that may run still reaches
//     its address; stores of the same immediate to one address are fine (the result is the same
//     whichever lane, wave or store runs last).
// A store in a block that provably does not run reaches nothing. A dword that was known before the
// dispatch and that no store which may run reaches is still known afterwards (the dispatch's
// binding forgot it, because the cache cannot tell which bytes of a bound range a shader writes).
// A branch condition is evaluated only from user-data derived descriptors, immediates and dwords
// whose current value the GPU thread has without waiting (clean guest memory, or a known store); a
// load in a dispatch of more than one wave is never evaluated (another wave's store could race it).
// Anything else about the shader (a write the analysis does not model, a loop, an indirect
// descriptor) makes it ineligible at the analysis, so no claim is ever made for a shader that can
// write something this code does not see.

#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

using ShaderRecompiler::IR::StoreBlock;
using ShaderRecompiler::IR::StoreExpr;
using ShaderRecompiler::IR::UniformStoreProgram;
using ShaderRecompiler::IR::UniformStoreSite;

// Where a buffer resource of the dispatch lives, as the descriptor and the mapped-memory map say.
struct StoreExtent {
	uint64_t base   = 0; // 0: a null buffer, nothing is read or written
	uint64_t size   = 0; // the descriptor's byte extent
	uint32_t stride = 0;
	bool     direct = false; // no swizzle, no add-tid, fully mapped: index * stride + offset is the address
};

enum class SiteState : uint8_t { Unreached, Maybe, Definite };

namespace detail {

struct Evaluator {
	const UniformStoreProgram&   program;
	std::span<const StoreExtent> extents;
	bool                         single_wave;
	// Reader(address, &value): the current value of the dword when it is available without waiting
	// for the GPU.
	template <class Reader>
	bool Eval(int32_t node, Reader& read, uint32_t& out, uint32_t depth = 0) const {
		using Op = StoreExpr::Op;
		if (node < 0 || static_cast<size_t>(node) >= program.exprs.size() || depth > 32) return false;
		const auto& e = program.exprs[static_cast<size_t>(node)];
		uint32_t    a = 0, b = 0, c = 0;
		switch (e.op) {
			case Op::Unknown: return false;
			case Op::Imm: out = e.imm; return true;
			case Op::Load:
			case Op::ScalarLoad: {
				if (!single_wave || e.resource >= extents.size()) return false;
				const auto& ext = extents[e.resource];
				if (ext.base == 0 || !ext.direct) return false;
				const uint64_t rel = (e.op == Op::Load ? static_cast<uint64_t>(e.index) * ext.stride : 0ull) + e.offset;
				if (rel + 4 > ext.size || ((ext.base + rel) & 3u) != 0) return false;
				return read(ext.base + rel, out);
			}
			case Op::Not: if (!Eval(e.a, read, a, depth + 1)) return false; out = a == 0 ? 1u : 0u; return true;
			case Op::Select: {
				if (Eval(e.a, read, a, depth + 1)) return Eval(a != 0 ? e.b : e.c, read, out, depth + 1);
				// An undecided condition still gives a value when both arms agree.
				return Eval(e.b, read, b, depth + 1) && Eval(e.c, read, c, depth + 1) && b == c && ((out = b), true);
			}
			default: break;
		}
		const bool ka = Eval(e.a, read, a, depth + 1);
		const bool kb = Eval(e.b, read, b, depth + 1);
		switch (e.op) {
			case Op::And: if ((ka && a == 0) || (kb && b == 0)) { out = 0; return true; } break;
			case Op::LogicalAnd: if ((ka && a == 0) || (kb && b == 0)) { out = 0; return true; } break;
			case Op::LogicalOr: if ((ka && a != 0) || (kb && b != 0)) { out = 1; return true; } break;
			default: break;
		}
		if (!ka || !kb) return false;
		switch (e.op) {
			case Op::And: out = a & b; return true;
			case Op::Or: out = a | b; return true;
			case Op::Xor: out = a ^ b; return true;
			case Op::Shl: if (b >= 32) return false; out = a << b; return true;
			case Op::Shr: if (b >= 32) return false; out = a >> b; return true;
			case Op::Add: out = a + b; return true;
			case Op::Sub: out = a - b; return true;
			case Op::Eq: out = a == b; return true;
			case Op::Ne: out = a != b; return true;
			case Op::Ult: out = a < b; return true;
			case Op::Ule: out = a <= b; return true;
			case Op::Ugt: out = a > b; return true;
			case Op::Uge: out = a >= b; return true;
			case Op::LogicalAnd: out = (a != 0) && (b != 0); return true;
			case Op::LogicalOr: out = (a != 0) || (b != 0); return true;
			default: return false;
		}
	}
};

} // namespace detail

// Which sites can run in this dispatch. `states` gets one entry per site. A branch whose condition
// cannot be decided makes both successors Maybe; a block reached by a decided edge from a block that
// surely runs surely runs.
template <class Reader>
void EvaluateStoreFlow(const UniformStoreProgram& program, std::span<const StoreExtent> extents,
                       bool single_wave, Reader&& read, std::vector<SiteState>& states) {
	states.assign(program.sites.size(), SiteState::Unreached);
	if (program.order.empty()) {
		return;
	}
	std::vector<SiteState> block_state(program.blocks.size(), SiteState::Unreached);
	block_state[program.order.front()] = SiteState::Definite;
	const detail::Evaluator eval {program, extents, single_wave};
	const auto raise = [&](uint32_t block, SiteState state) {
		if (block != StoreBlock::kNone && block < block_state.size() && state > block_state[block]) {
			block_state[block] = state;
		}
	};
	for (const auto index: program.order) {
		const auto state = block_state[index];
		if (state == SiteState::Unreached) {
			continue;
		}
		const auto& block = program.blocks[index];
		for (uint32_t s = block.site_begin; s < block.site_end && s < states.size(); ++s) {
			states[s] = state;
		}
		if (block.cond < 0) {
			raise(block.succ_true, state);
			continue;
		}
		uint32_t value = 0;
		if (eval.Eval(block.cond, read, value)) {
			// Decided: the other way is not taken from here. Surely, if this block surely runs.
			raise(value != 0 ? block.succ_true : block.succ_false, state);
		} else {
			// Undecided: either way may run, neither surely.
			raise(block.succ_true, SiteState::Maybe);
			raise(block.succ_false, SiteState::Maybe);
		}
	}
}

namespace detail {
struct Cover {
	uint64_t begin = 0, end = 0;
	bool     claim = false; // proven to run and to store `value`
	bool     known = false; // would store the immediate `value` if it runs
	uint32_t value = 0;
};

inline bool BuildCovers(const UniformStoreProgram& program, std::span<const SiteState> states,
                        std::span<const StoreExtent> extents, std::vector<Cover>& covers) {
	covers.clear();
	constexpr size_t kMaxCovers = 128;
	for (uint32_t s = 0; s < program.sites.size(); ++s) {
		const auto& site = program.sites[s];
		if (site.resource >= extents.size() || site.dwords == 0 || site.dwords > 4 || s >= states.size()) {
			return false; // the plan does not match the dispatch
		}
		if (states[s] == SiteState::Unreached) {
			continue; // provably not run: reaches nothing
		}
		const auto& ext = extents[site.resource];
		if (ext.base == 0 || ext.size == 0) {
			continue; // null buffer: the stores are dropped
		}
		if (!ext.direct) {
			covers.push_back({ext.base, ext.base + ext.size, false, false, 0});
			continue;
		}
		for (uint32_t d = 0; d < site.dwords; ++d) {
			const uint64_t offset = static_cast<uint64_t>(site.index) * ext.stride + site.byte_offset + 4ull * d;
			if (offset + 4 > ext.size) {
				continue; // out of range of the descriptor: dropped, writes nothing we account for
			}
			const uint64_t address = ext.base + offset;
			const bool     known   = ((site.known_mask >> d) & 1u) != 0 && (address & 3u) == 0;
			const bool     claim   = known && site.executes && states[s] == SiteState::Definite;
			covers.push_back({address, address + 4, claim, known, site.value[d]});
		}
		if (covers.size() > kMaxCovers) {
			return false;
		}
	}
	return true;
}
} // namespace detail

// The dwords this dispatch leaves with a constant value (`claims`) and the candidates (dwords that
// were known before the dispatch, inside a range it binds for writing) that it provably does not
// touch (`kept`).
inline void ResolveKnownStores(const UniformStoreProgram& program, std::span<const SiteState> states,
                               std::span<const StoreExtent>                        extents,
                               std::span<const std::pair<uint64_t, uint32_t>>      candidates,
                               std::vector<std::pair<uint64_t, uint32_t>>&          claims,
                               std::vector<std::pair<uint64_t, uint32_t>>&          kept) {
	claims.clear();
	kept.clear();
	std::vector<detail::Cover> covers;
	if (!detail::BuildCovers(program, states, extents, covers)) {
		return;
	}
	for (const auto& c: covers) {
		if (!c.claim) {
			continue;
		}
		bool exact = true;
		for (const auto& o: covers) {
			if (&o == &c || o.end <= c.begin || o.begin >= c.end) {
				continue;
			}
			if (!(o.known && o.begin == c.begin && o.end == c.end && o.value == c.value)) {
				exact = false;
				break;
			}
		}
		if (exact) {
			bool seen = false;
			for (const auto& [a, v]: claims) {
				seen |= a == c.begin;
			}
			if (!seen) {
				claims.emplace_back(c.begin, c.value);
			}
		}
	}
	for (const auto& [address, value]: candidates) {
		bool touched = false;
		for (const auto& o: covers) {
			touched |= o.begin < address + 4 && address < o.end;
		}
		if (!touched) {
			kept.emplace_back(address, value);
		}
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNSTORES_H_
