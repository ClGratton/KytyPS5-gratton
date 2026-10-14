#include "common/drawRepeat.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace Common::DrawRepeat {

namespace {

struct Sig {
	uint64_t ctx = 0, sh = 0, args = 0, reads = 0;
	bool     has_plan = false;
};
// Differing groups of a matched draw: bit 0 ctx, 1 sh, 2 args, 3 reads (4: plan present in one frame only).
constexpr unsigned kMasks = 32;

struct State {
	bool                               enabled = false;
	std::FILE*                         csv     = nullptr;
	std::unordered_map<uint64_t, Sig>  prev, cur;
	std::unordered_map<uint64_t, uint32_t> occurrence; // per submission: program -> count
	uint32_t                           item_ordinal = 0;
	uint64_t                           frame        = 0;
	uint64_t                           reads_acc    = 0;
	bool                               reads_seen   = false;
	uint64_t                           items        = 0;
	std::array<uint64_t, 2>            draws {}, matched {};
	std::array<std::array<uint64_t, kMasks>, 2> masks {};
};

State& S() {
	static State state = [] {
		State s;
		if (const char* path = std::getenv("KYTY_DRAW_REPEAT"); path != nullptr && path[0] != '\0') {
			s.csv = std::fopen(path, "w");
			if (s.csv != nullptr) {
				s.enabled = true;
				std::fprintf(s.csv, "frame,items,draws,draws_matched,dispatches,dispatches_matched");
				for (unsigned m = 0; m < kMasks; m++) {
					std::fprintf(s.csv, ",dm%02u", m);
				}
				for (unsigned m = 0; m < kMasks; m++) {
					std::fprintf(s.csv, ",cm%02u", m);
				}
				std::fprintf(s.csv, "\n");
				std::fprintf(stderr, "DrawRepeat: logging to %s\n", path);
			}
		}
		return s;
	}();
	return state;
}

void EndFrame(State& s) {
	if (s.frame != 0 && s.csv != nullptr) {
		std::fprintf(s.csv, "%llu,%llu,%llu,%llu,%llu,%llu", static_cast<unsigned long long>(s.frame),
		             static_cast<unsigned long long>(s.items), static_cast<unsigned long long>(s.draws[0]),
		             static_cast<unsigned long long>(s.matched[0]), static_cast<unsigned long long>(s.draws[1]),
		             static_cast<unsigned long long>(s.matched[1]));
		for (unsigned k = 0; k < 2; k++) {
			for (unsigned m = 0; m < kMasks; m++) {
				std::fprintf(s.csv, ",%llu", static_cast<unsigned long long>(s.masks[k][m]));
			}
		}
		std::fprintf(s.csv, "\n");
		std::fflush(s.csv);
	}
	s.prev = std::move(s.cur);
	s.cur.clear();
	s.occurrence.clear();
	s.item_ordinal = 0;
	s.items        = 0;
	s.draws        = {};
	s.matched      = {};
	for (auto& a: s.masks) {
		a.fill(0);
	}
	s.frame++;
}

} // namespace

bool Enabled() noexcept {
	static const bool enabled = S().enabled;
	return enabled;
}

uint64_t HashBytes(const void* data, size_t size, uint64_t seed) noexcept {
	const auto*    p = static_cast<const unsigned char*>(data);
	uint64_t       h = seed ^ 0xcbf29ce484222325ull;
	size_t         i = 0;
	for (; i + 8 <= size; i += 8) {
		uint64_t w;
		std::memcpy(&w, p + i, 8);
		h = (h ^ w) * 0x9e3779b97f4a7c15ull;
		h ^= h >> 29;
	}
	if (i < size) {
		uint64_t w = 0;
		std::memcpy(&w, p + i, size - i);
		h = (h ^ w) * 0x9e3779b97f4a7c15ull;
		h ^= h >> 29;
	}
	return h;
}

void BeginItem(bool reset) noexcept {
	auto& s = S();
	if (!s.enabled) {
		return;
	}
	if (reset) {
		EndFrame(s);
		return;
	}
	s.item_ordinal++;
	s.items++;
	s.occurrence.clear();
	s.reads_acc  = 0;
	s.reads_seen = false;
}

void NoteReads(uint64_t hash) noexcept {
	auto& s = S();
	if (!s.enabled) {
		return;
	}
	s.reads_acc  = Mix(s.reads_acc, hash);
	s.reads_seen = true;
}

void NoteDraw(const Inputs& in) noexcept {
	auto& s = S();
	if (!s.enabled) {
		return;
	}
	const uint64_t occ = s.occurrence[Mix(in.program, in.kind)]++;
	uint64_t       key = Mix(Mix(Mix(in.program, in.kind), s.item_ordinal), occ);
	Sig            sig;
	sig.ctx      = in.ctx;
	sig.sh       = in.sh;
	sig.args     = in.args;
	sig.reads    = s.reads_acc;
	sig.has_plan = s.reads_seen;
	s.reads_acc  = 0;
	s.reads_seen = false;
	s.draws[in.kind]++;
	if (const auto it = s.prev.find(key); it != s.prev.end()) {
		const Sig& p = it->second;
		unsigned   m = (p.ctx != sig.ctx ? 1u : 0u) | (p.sh != sig.sh ? 2u : 0u) | (p.args != sig.args ? 4u : 0u) |
		             (p.reads != sig.reads ? 8u : 0u) | (p.has_plan != sig.has_plan ? 16u : 0u);
		s.matched[in.kind]++;
		s.masks[in.kind][m]++;
	}
	s.cur[key] = sig;
}

} // namespace Common::DrawRepeat
