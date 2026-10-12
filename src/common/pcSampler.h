#ifndef KYTY_COMMON_PC_SAMPLER_H_
#define KYTY_COMMON_PC_SAMPLER_H_

// --debug-gpu-pc-sample <hz>: a statistical profile of the GPU thread (kyGpu) without external
// tools. A per-thread CPU-time timer (so waiting does not count) signals the thread, whose handler
// stores the interrupted instruction pointer. Every phase-stats report the counts are written to
// _gpu_pc_samples.txt as "<offset in the executable> <count>" lines, most frequent first; symbolize
// with `llvm-symbolizer -e <unstripped binary>` (a release build is stripped: relink without
// --strip-all, the code addresses stay the same). Linux only; off (hz 0) by default.

#include <cstdint>

namespace Common::PcSampler {

void Configure(uint32_t hz);          // from Config::Load()
void StartOnThisThread();             // the GPU thread, once; no-op when hz is 0
void WriteReport() noexcept;          // that thread, from the phase report

} // namespace Common::PcSampler

#endif /* KYTY_COMMON_PC_SAMPLER_H_ */
