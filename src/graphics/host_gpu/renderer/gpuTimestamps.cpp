#include "graphics/host_gpu/renderer/gpuTimestamps.h"

#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/gpuClock.h"
#include "graphics/host_gpu/renderer/sync.h"

namespace Libs::Graphics::GpuTimestamps {

namespace {

// Time scale in the parse-time domain: values are the reference clock at the moment the emulator
// parses the packet, boundaries are the parse-time flips.
GpuClock::TimeScaler& ParseTimeScaler() {
	static GpuClock::TimeScaler scaler(Config::GetGpuTimeScale());
	return scaler;
}

} // namespace

uint64_t SampleGuestClock(CommandBuffer& command, void* dst, uint32_t size, Point point) {
	(void)command;
	(void)dst;
	(void)size;
	(void)point;
	return ParseTimeScaler().Apply(Sync::ReadReferenceClock());
}

void OnGuestFlip(CommandBuffer& command) {
	(void)command;
	ParseTimeScaler().AddBoundary(Sync::ReadReferenceClock());
}

} // namespace Libs::Graphics::GpuTimestamps
