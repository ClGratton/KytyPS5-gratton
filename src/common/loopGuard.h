#ifndef KYTY_COMMON_LOOPGUARD_H_
#define KYTY_COMMON_LOOPGUARD_H_

// The shader loop guard (--shader-loop-limit, on by default).
//
// Every shader invocation gets a budget of loop-header executions, shared by all of its loops. When it
// is used up the loops are left, so a loop that cannot end (a linked list with a cycle, a garbage trip
// count) costs a bounded time instead of hanging the GPU (NVRM Xid 109). A trip always means an
// emulation bug or corrupt guest data, so it is never silent: shaders report how much of the budget an
// invocation used into a host-only tail of the GDS buffer (one dword per shader slot, atomic max), and
// Monitor turns that into log lines (a trip: loud, once per shader; high-water marks: informative).
//
// Pure logic, unit tested in tests/LoopGuardTests.cpp. The emitter side is in
// graphics/shader/recompiler/backend/spirv (EmitLoopBudgetCheck, EmitLoopGuardReport).

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace Common::LoopGuard {

// Loop-header executions per invocation (all loops together) when --shader-loop-limit is not given.
constexpr uint32_t DefaultLimit = 1u << 20;

// The guest's GDS is 64 KiB; the host buffer has a tail after it that no guest address reaches.
constexpr uint32_t GuestGdsBytes = 64u * 1024u;
constexpr uint32_t Slots         = 1024;
constexpr uint32_t HostTailBytes = Slots * sizeof(uint32_t);
constexpr uint32_t FirstDword    = GuestGdsBytes / sizeof(uint32_t);

[[nodiscard]] constexpr uint32_t SlotFor(uint64_t shader_hash) {
	return static_cast<uint32_t>((shader_hash ^ (shader_hash >> 29) ^ (shader_hash >> 47)) % Slots);
}

// An invocation reports when it used at least this many iterations (a trip reports the whole limit).
[[nodiscard]] constexpr uint32_t ReportThreshold(uint32_t limit) {
	return std::max<uint32_t>(16u, limit / 64u);
}

class Monitor {
public:
	void Register(uint64_t shader_hash, const char* stage) {
		std::lock_guard lock(m_mutex);
		auto& slot = m_slots[SlotFor(shader_hash)];
		for (const auto& entry: slot.shaders) {
			if (entry.hash == shader_hash) {
				return;
			}
		}
		slot.shaders.push_back({shader_hash, stage});
	}

	// `tail` is the host tail of the GDS buffer (Slots dwords). Returns the lines to print for what
	// is new since the last call.
	[[nodiscard]] std::vector<std::string> Observe(std::span<const uint32_t> tail, uint32_t limit) {
		std::vector<std::string> lines;
		std::lock_guard          lock(m_mutex);
		const uint32_t           threshold = ReportThreshold(limit);
		for (uint32_t index = 0; index < std::min<size_t>(tail.size(), Slots); index++) {
			const uint32_t used = tail[index];
			if (used < threshold) {
				continue;
			}
			auto& slot = m_slots[index];
			if (used >= limit && !slot.tripped) {
				slot.tripped = true;
				lines.push_back("LOOP GUARD TRIPPED: " + Describe(slot) + " used its whole budget of " +
				                std::to_string(limit) +
				                " loop iterations in one invocation and its loops were cut short. "
				                "This is an emulation bug (a mistranslated shader, a pass that left "
				                "corrupt data) or a hang the guard prevented: please report it with "
				                "the shader hash.\n");
			}
			if (used > slot.reported && (slot.reported == 0 || used / 2 >= slot.reported)) {
				slot.reported = used;
				if (used < limit) {
					lines.push_back("loop guard: " + Describe(slot) + " used up to " +
					                std::to_string(used) + " of " + std::to_string(limit) +
					                " loop iterations in one invocation\n");
				}
			}
			slot.max = std::max(slot.max, used);
		}
		return lines;
	}

	// The highest use seen for each reporting slot, for a summary at exit.
	[[nodiscard]] std::vector<std::string> Summary(uint32_t limit) {
		std::vector<std::string> lines;
		std::lock_guard          lock(m_mutex);
		for (const auto& [index, slot]: m_slots) {
			if (slot.max >= ReportThreshold(limit)) {
				lines.push_back("loop guard summary: " + Describe(slot) + " max " +
				                std::to_string(slot.max) + " of " + std::to_string(limit) + "\n");
			}
		}
		return lines;
	}

private:
	struct Shader {
		uint64_t    hash;
		std::string stage;
	};
	struct Slot {
		std::vector<Shader> shaders;
		uint32_t            reported = 0;
		uint32_t            max      = 0;
		bool                tripped  = false;
	};
	static std::string Describe(const Slot& slot) {
		std::string text = "shader";
		for (size_t i = 0; i < slot.shaders.size() && i < 4; i++) {
			char hash[24];
			std::snprintf(hash, sizeof(hash), "%016" PRIx64, slot.shaders[i].hash);
			text += (i == 0 ? " " : " or ") + slot.shaders[i].stage + " " + hash;
		}
		if (slot.shaders.empty()) {
			text += " (unregistered)";
		}
		return text;
	}

	std::mutex                  m_mutex;
	std::map<uint32_t, Slot>    m_slots;
};

// The process-wide monitor (shaders register when compiled, the buffer cache observes the GDS tail).
[[nodiscard]] inline Monitor& GetMonitor() {
	static Monitor monitor;
	return monitor;
}

} // namespace Common::LoopGuard

#endif // KYTY_COMMON_LOOPGUARD_H_
