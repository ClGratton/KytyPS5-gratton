#ifndef KYTY_COMMON_MEASURE_HACK_H_
#define KYTY_COMMON_MEASURE_HACK_H_

// MEASUREMENT HACK (branch readback-ceiling, local, never merged): stale values for GPU-dependent
// readbacks, to find the frame rate that is left when the GPU thread no longer waits for them.
// --debug-skip-srt-drain / --debug-skip-meta-drain: the dependent read (the SRT walk's strict read
// of GPU-dirty guest bytes / the render-target clear code) uses the guest's stale copy instead of
// draining the GPU. Every --debug-skip-drain-verify'th such read drains as usual and compares the
// stale and the real bytes; mismatches and a running summary go to _skip_drain.log.

#include <cstdint>

namespace Common::MeasureHack {

enum class Kind : uint8_t { Srt, Meta, Count };

// True: skip this drain (use the stale bytes). False: drain, then call NoteVerify.
[[nodiscard]] bool SkipNow(Kind kind) noexcept;
void               NoteVerify(Kind kind, uint64_t address, const void* stale, const void* now,
                              uint64_t size) noexcept;

} // namespace Common::MeasureHack

#endif /* KYTY_COMMON_MEASURE_HACK_H_ */
