#include "graphics/guest_gpu/csRegisters.h"

#include "graphics/guest_gpu/pm4.h"

namespace Libs::Graphics::HW {

CsRegisterResult ApplyComputeShaderRegister(CsStageRegisters& regs, uint32_t offset, uint32_t value) {
	switch (offset) {
		case Pm4::COMPUTE_PGM_LO:
			regs.data_addr &= 0xFFFFFF00000000FFull;
			regs.data_addr |= static_cast<uint64_t>(value) << 8u;
			break;
		case Pm4::COMPUTE_PGM_HI:
			regs.data_addr &= 0xFFFF00FFFFFFFFFFull;
			regs.data_addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			break;
		case Pm4::COMPUTE_PGM_RSRC1:
			regs.vgprs =
			    (value >> Pm4::COMPUTE_PGM_RSRC1_VGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_VGPRS_MASK;
			regs.priority = (value >> Pm4::COMPUTE_PGM_RSRC1_PRIORITY_SHIFT) &
			                   Pm4::COMPUTE_PGM_RSRC1_PRIORITY_MASK;
			regs.float_mode    = (value >> Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_SHIFT) &
			                        Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_MASK;
			regs.dx10_clamp    = ((value >> Pm4::COMPUTE_PGM_RSRC1_DX10_CLAMP_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_DX10_CLAMP_MASK) != 0u;
			regs.debug_mode    = ((value >> Pm4::COMPUTE_PGM_RSRC1_DEBUG_MODE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_DEBUG_MODE_MASK) != 0u;
			regs.ieee_mode     = ((value >> Pm4::COMPUTE_PGM_RSRC1_IEEE_MODE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_IEEE_MODE_MASK) != 0u;
			regs.fp16_overflow = ((value >> Pm4::COMPUTE_PGM_RSRC1_FP16_OVFL_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_FP16_OVFL_MASK) != 0u;
			regs.threadgroup_configuration = ((value >> Pm4::COMPUTE_PGM_RSRC1_WGP_MODE_SHIFT) &
			                                     Pm4::COMPUTE_PGM_RSRC1_WGP_MODE_MASK) != 0u;
			regs.require_forward_progress =
			    ((value >> Pm4::COMPUTE_PGM_RSRC1_FWD_PROGRESS_SHIFT) &
			     Pm4::COMPUTE_PGM_RSRC1_FWD_PROGRESS_MASK) != 0u;
			break;
		case Pm4::COMPUTE_PGM_RSRC2:
			regs.scratch_en     = ((value >> Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_MASK) != 0u;
			regs.user_sgpr      = (value >> Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_MASK;
			regs.tgid_x_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_MASK) != 0u;
			regs.tgid_y_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_MASK) != 0u;
			regs.tgid_z_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_MASK) != 0u;
			regs.tg_size_en     = ((value >> Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_MASK) != 0u;
			regs.tidig_comp_cnt = (value >> Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_MASK;
			regs.lds_size       = (value >> Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_MASK;
			break;
		case Pm4::COMPUTE_PGM_RSRC3:
			regs.shared_vgprs =
			    (value >> Pm4::COMPUTE_PGM_RSRC3_SHARED_VGPRS_SHIFT) &
			    Pm4::COMPUTE_PGM_RSRC3_SHARED_VGPRS_MASK;
			break;
		case Pm4::COMPUTE_NUM_THREAD_X: regs.num_thread_x = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Y: regs.num_thread_y = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Z: regs.num_thread_z = value; break;
		case Pm4::COMPUTE_START_X:
		case Pm4::COMPUTE_START_Y:
		case Pm4::COMPUTE_START_Z:
		case Pm4::COMPUTE_RESOURCE_LIMITS:
		case Pm4::COMPUTE_TMPRING_SIZE:
		case Pm4::COMPUTE_PACE_ID: return CsRegisterResult::Ignored;
		default: return CsRegisterResult::Unsupported;
	}
	return CsRegisterResult::Applied;
}

} // namespace Libs::Graphics::HW
