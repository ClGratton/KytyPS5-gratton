#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_CSREGISTERS_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_CSREGISTERS_H_

#include "graphics/guest_gpu/hardwareContext.h"

#include <cstdint>

namespace Libs::Graphics::HW {

enum class CsRegisterResult { Applied, Ignored, Unsupported };

// Applies one COMPUTE_* SH register write (a SET_SH_REG payload word or one entry of an AGC shader
// header's register list) to `regs`. Shared by the PM4 decoder and by the shader-load hook, which
// reconstructs a compute shader's stage state from the registers its header carries before any
// command buffer mentions it. `Ignored` registers are known and do not affect translation.
CsRegisterResult ApplyComputeShaderRegister(CsStageRegisters& regs, uint32_t offset, uint32_t value);

} // namespace Libs::Graphics::HW

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_CSREGISTERS_H_
