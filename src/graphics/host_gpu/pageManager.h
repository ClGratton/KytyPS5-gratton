#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;

	// Restores the watchers' protection after the host protection of watched pages was changed
	// behind the tracker's back (a guest mprotect). Unwatched pages keep theirs.
	void ReapplyProtection(uint64_t vaddr, uint64_t size);

	// --mem-residency: arms one-shot read probes. Every page of [vaddr, vaddr + size) that is write
	// watched (a clean copy of GPU data) and not read watched is made inaccessible; the first CPU
	// access to it faults and ConsumeReadProbe gives the page its tracker protection back. Returns
	// the number of pages armed. A probe is dropped, uncounted by the fault path, when the page
	// loses its last write watcher or gains a read watcher (counted in ProbeStats::dropped).
	uint64_t ArmReadProbes(uint64_t vaddr, uint64_t size);
	// True (and the probe is removed) when the page of `vaddr` held an armed probe.
	[[nodiscard]] bool ConsumeReadProbe(uint64_t vaddr);

	struct ProbeStats {
		uint64_t armed    = 0; // pages armed, cumulative
		uint64_t consumed = 0; // probes removed by an access
		uint64_t dropped  = 0; // probes removed by the tracker (page released or read watched)
	};
	[[nodiscard]] ProbeStats GetProbeStats() const noexcept;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
