#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

// --push-descriptor-delta. A pushed descriptor set keeps the bindings a later push does not write, until the set is disturbed
// (a different pipeline layout at the bind point, a bound descriptor set, a new command buffer, another component's graphics
// pipeline). This holds the content of every binding written since the last disturbance, so a push can leave out the
// bindings whose content has not changed: the driver's cost of a push is per written descriptor.
//
// Pure data, no Vulkan types: the caller turns a write into a list of words (type, count, then the handles and ranges).
// GPU thread only; one instance per command scheduler, invalidated whenever a recording starts or another component binds a
// graphics pipeline.
class PushDescriptorCache {
public:
	struct Stats {
		uint64_t pushes         = 0; // CommitBindings calls that went through the cache
		uint64_t writes_seen    = 0;
		uint64_t writes_pushed  = 0;
		uint64_t pushes_elided  = 0; // calls where every write was unchanged
		uint64_t disturbed      = 0; // times the held content was discarded
	};

	// Whatever the set holds is no longer known.
	void Invalidate() noexcept {
		if (m_layout != 0) m_stats.disturbed++;
		m_layout = 0;
	}

	// A push is about to be made at `layout` (the pipeline layout handle, nonzero). A layout other than the last one's
	// discards the held content.
	void Begin(uint64_t layout) {
		m_stats.pushes++;
		if (layout != m_layout) {
			if (m_layout != 0) m_stats.disturbed++;
			for (auto& slot: m_slots) slot.valid = false;
			m_layout = layout;
		}
	}

	// True when the binding has to be written: its content differs from what the set holds (or nothing is held). The new
	// content is remembered either way.
	[[nodiscard]] bool Changed(uint32_t binding, std::span<const uint64_t> content) {
		m_stats.writes_seen++;
		if (binding >= m_slots.size()) m_slots.resize(binding + 1);
		auto& slot = m_slots[binding];
		if (slot.valid && slot.content.size() == content.size() &&
		    std::equal(content.begin(), content.end(), slot.content.begin())) {
			return false;
		}
		slot.content.assign(content.begin(), content.end());
		slot.valid = true;
		m_stats.writes_pushed++;
		return true;
	}

	// A write that cannot be compared is pushed and must not be compared against older content afterwards.
	void Forget(uint32_t binding) {
		m_stats.writes_seen++;
		m_stats.writes_pushed++;
		if (binding < m_slots.size()) m_slots[binding].valid = false;
	}

	void NoteElided() noexcept { m_stats.pushes_elided++; }

	[[nodiscard]] const Stats& GetStats() const noexcept { return m_stats; }

private:
	struct Slot {
		std::vector<uint64_t> content;
		bool                  valid = false;
	};

	std::vector<Slot> m_slots;
	uint64_t          m_layout = 0;
	Stats             m_stats;
};

} // namespace Libs::Graphics
