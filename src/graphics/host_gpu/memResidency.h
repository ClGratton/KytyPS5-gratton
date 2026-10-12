#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMRESIDENCY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMRESIDENCY_H_

#include "common/common.h"
#include "graphics/host_gpu/memResidencyRanges.h"
#include "graphics/host_gpu/pageManager.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

// --mem-residency: log-only host RAM diagnostics for GPU-registered guest memory. Nothing here
// changes emulation; the one active part is the one-shot read probe (PageManager::ArmReadProbes).
namespace Libs::Graphics::MemResidency {

// How the cache classes a guest range it holds a GPU copy of.
enum class RangeClass : uint8_t {
	TextureClean, // sampled image, tracked, CPU-clean and not GPU-modified: guest copy == VRAM copy
	TextureOther, // sampled image that is CPU-dirty, GPU-modified or untracked
	RenderTarget,
	Depth,
	Buffer,
	Count
};
inline constexpr size_t RANGE_CLASS_COUNT = static_cast<size_t>(RangeClass::Count);

// What a CPU fault hit.
enum class FaultClass : uint8_t { Texture, RenderTarget, Depth, NonImage, Count };
inline constexpr size_t FAULT_CLASS_COUNT = static_cast<size_t>(FaultClass::Count);

// Who took the fault.
enum class Actor : uint8_t { Guest, Gpu, Host, Count };
inline constexpr size_t ACTOR_COUNT = static_cast<size_t>(Actor::Count);

struct Snapshot {
	std::array<std::vector<Range>, RANGE_CLASS_COUNT> ranges;
	uint64_t                                          frame = 0;
	PageManager::ProbeStats                           probes;
};

// Starts (on) or stops (off) the worker that turns snapshots into log lines.
void Enable(bool on);
[[nodiscard]] bool Enabled() noexcept;
[[nodiscard]] Actor CurrentActor() noexcept;
// True once per sampling interval (10 s); GPU thread.
[[nodiscard]] bool SampleDue() noexcept;
void               Submit(Snapshot&& snapshot);

void NoteProbeArmed(uint64_t pages) noexcept;
void NoteProbeRead(FaultClass cls, Actor actor) noexcept;  // first CPU read of a probed page
void NoteProbeWrite(FaultClass cls, Actor actor) noexcept; // first CPU write of a probed page
void NoteWriteFault(FaultClass cls, Actor actor) noexcept; // write to a write-watched page
void NoteReadFault(Actor actor) noexcept;                  // read of a GPU-dirty (read-watched) page

} // namespace Libs::Graphics::MemResidency

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMRESIDENCY_H_
