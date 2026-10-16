#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_NULLPAGEREAD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_NULLPAGEREAD_H_

#include "common/guestAddressSpace.h"

#include <cstdint>

namespace Libs::Graphics {

/// No guest memory is mapped below the start of the guest address space (see
/// Common::kGuestAddressSpaceStart: the shader resource walk uses the same boundary).
inline constexpr uint64_t GuestNullPageEnd = Common::kGuestAddressSpaceStart;

/// Satisfies a shader table read that goes through a null pointer with zeros; see
/// Common::ReadGuestNullPage.
using Common::ReadGuestNullPage;

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_NULLPAGEREAD_H_
