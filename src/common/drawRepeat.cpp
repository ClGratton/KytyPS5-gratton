#include "common/drawRepeat.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace Common::DrawRepeat {

namespace {

struct Sig {
	uint64_t h[ComponentCount] {};
	bool     has_plan = false;
};

// Joint equalities that matter (see the notes): bit i of `equal` = component i equal.
constexpr unsigned kJoint = 5;
// J0 = state equal (ctx, user config, draw args); J1 = content equal (shader regs without pointers, reads values);
// J2 = J0 & J1; J3 = J2 with the pointer-carrying components too (strictly identical);
// J4 = state + shader regs (everything but the reads and the pointers)
const char* const kJointNames[kJoint] = {"state", "content", "state_content", "strict", "no_reads"};

struct State {
	bool                               enabled = false;
	std::FILE*                         csv     = nullptr;
	std::unordered_map<uint64_t, Sig>  prev, cur;
	std::unordered_map<uint64_t, uint32_t> occurrence; // per submission: program -> count
	uint32_t                           item_ordinal = 0;
	uint64_t                           frame        = 0;
	uint64_t                           reads_values = 0, reads_inputs = 0;
	bool                               reads_seen   = false;
	uint64_t                           items        = 0;
	std::array<uint64_t, 2>            draws {}, matched {};
	std::array<std::array<uint64_t, ComponentCount>, 2> equal {};
	std::array<std::array<uint64_t, kJoint>, 2>         joint {};
};

State& S() {
	static State state = [] {
		State s;
		if (const char* path = std::getenv("KYTY_DRAW_REPEAT"); path != nullptr && path[0] != '\0') {
			s.csv = std::fopen(path, "w");
			if (s.csv != nullptr) {
				s.enabled = true;
				std::fprintf(s.csv, "frame,items");
				for (const char* k: {"d", "c"}) {
					std::fprintf(s.csv, ",%s_n,%s_matched", k, k);
					for (const char* c: kComponentNames) {
						std::fprintf(s.csv, ",%s_eq_%s", k, c);
					}
					for (const char* j: kJointNames) {
						std::fprintf(s.csv, ",%s_j_%s", k, j);
					}
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
		std::fprintf(s.csv, "%llu,%llu", static_cast<unsigned long long>(s.frame),
		             static_cast<unsigned long long>(s.items));
		for (unsigned k = 0; k < 2; k++) {
			std::fprintf(s.csv, ",%llu,%llu", static_cast<unsigned long long>(s.draws[k]),
			             static_cast<unsigned long long>(s.matched[k]));
			for (unsigned c = 0; c < ComponentCount; c++) {
				std::fprintf(s.csv, ",%llu", static_cast<unsigned long long>(s.equal[k][c]));
			}
			for (unsigned j = 0; j < kJoint; j++) {
				std::fprintf(s.csv, ",%llu", static_cast<unsigned long long>(s.joint[k][j]));
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
	for (auto& a: s.equal) {
		a.fill(0);
	}
	for (auto& a: s.joint) {
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
	s.reads_values = s.reads_inputs = 0;
	s.reads_seen = false;
}

void NoteReads(uint64_t values_hash, uint64_t inputs_hash) noexcept {
	auto& s = S();
	if (!s.enabled) {
		return;
	}
	s.reads_values = Mix(s.reads_values, values_hash);
	s.reads_inputs = Mix(s.reads_inputs, inputs_hash);
	s.reads_seen   = true;
}

void NoteDraw(const Inputs& in) noexcept {
	auto& s = S();
	if (!s.enabled) {
		return;
	}
	const uint64_t occ = s.occurrence[Mix(in.program, in.kind)]++;
	const uint64_t key = Mix(Mix(Mix(in.program, in.kind), s.item_ordinal), occ);
	Sig            sig;
	for (unsigned c = 0; c < ComponentCount; c++) {
		sig.h[c] = in.h[c];
	}
	sig.h[ReadsValues] = s.reads_values;
	sig.h[ReadsInputs] = s.reads_inputs;
	sig.has_plan       = s.reads_seen;
	s.reads_values = s.reads_inputs = 0;
	s.reads_seen = false;
	s.draws[in.kind]++;
	if (const auto it = s.prev.find(key); it != s.prev.end()) {
		const Sig& p = it->second;
		bool       eq[ComponentCount];
		for (unsigned c = 0; c < ComponentCount; c++) {
			eq[c] = p.h[c] == sig.h[c];
			s.equal[in.kind][c] += eq[c] ? 1 : 0;
		}
		const bool plan_same = p.has_plan == sig.has_plan;
		const bool state     = eq[CtxAll] && eq[Ucfg] && eq[DrawArgs];
		const bool shaders   = eq[ShVsBase] && eq[ShPsRegs] && eq[ShRsrcVs] && eq[ShSgprMeta];
		const bool content   = shaders && eq[ReadsValues] && plan_same;
		s.matched[in.kind]++;
		s.joint[in.kind][0] += state ? 1 : 0;
		s.joint[in.kind][1] += content ? 1 : 0;
		s.joint[in.kind][2] += (state && content) ? 1 : 0;
		s.joint[in.kind][3] += (state && content && eq[ShUser] && eq[ReadsInputs]) ? 1 : 0;
		s.joint[in.kind][4] += (state && shaders) ? 1 : 0;
	}
	s.cur[key] = sig;
}

} // namespace Common::DrawRepeat
