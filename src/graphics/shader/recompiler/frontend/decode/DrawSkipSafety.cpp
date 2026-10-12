#include "graphics/shader/recompiler/frontend/decode/DrawSkipSafety.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

bool InstructionWritesPersistentMemory(const Instruction& inst) {
	switch (inst.opcode) {
		case Opcode::BUFFER_STORE_FORMAT_X:
		case Opcode::BUFFER_STORE_FORMAT_XY:
		case Opcode::BUFFER_STORE_FORMAT_XYZ:
		case Opcode::BUFFER_STORE_FORMAT_XYZW:
		case Opcode::BUFFER_STORE_FORMAT_D16_X:
		case Opcode::BUFFER_STORE_BYTE:
		case Opcode::BUFFER_STORE_SHORT:
		case Opcode::BUFFER_STORE_DWORD:
		case Opcode::BUFFER_STORE_DWORDX2:
		case Opcode::BUFFER_STORE_DWORDX3:
		case Opcode::BUFFER_STORE_DWORDX4:
		case Opcode::TBUFFER_STORE_FORMAT_X:
		case Opcode::TBUFFER_STORE_FORMAT_XY:
		case Opcode::TBUFFER_STORE_FORMAT_XYZ:
		case Opcode::TBUFFER_STORE_FORMAT_XYZW:
		case Opcode::FLAT_STORE_BYTE:
		case Opcode::FLAT_STORE_SHORT:
		case Opcode::FLAT_STORE_DWORD:
		case Opcode::FLAT_STORE_DWORDX2:
		case Opcode::FLAT_STORE_DWORDX3:
		case Opcode::FLAT_STORE_DWORDX4:
		case Opcode::IMAGE_STORE:
		case Opcode::IMAGE_STORE_MIP:
		case Opcode::IMAGE_ATOMIC_SWAP:
		case Opcode::IMAGE_ATOMIC_CMPSWAP:
		case Opcode::IMAGE_ATOMIC_ADD:
		case Opcode::IMAGE_ATOMIC_SMIN:
		case Opcode::IMAGE_ATOMIC_UMIN:
		case Opcode::IMAGE_ATOMIC_SMAX:
		case Opcode::IMAGE_ATOMIC_UMAX:
		case Opcode::IMAGE_ATOMIC_AND:
		case Opcode::IMAGE_ATOMIC_OR:
		case Opcode::IMAGE_ATOMIC_XOR:
		case Opcode::IMAGE_ATOMIC_FMIN:
		case Opcode::IMAGE_ATOMIC_FMAX:
		case Opcode::DS_ORDERED_COUNT: return true;
		case Opcode::UNKNOWN:
		case Opcode::UNSUPPORTED: return true; // cannot tell: wait
		default: break;
	}
	// LDS accesses are private to the workgroup; the same instructions on GDS are not.
	return inst.family == Family::DS && inst.gds;
}

bool ProgramMayBeSkipped(const Program& program) {
	for (const auto& inst: program.instructions) {
		if (InstructionWritesPersistentMemory(inst)) {
			return false;
		}
	}
	return true;
}

bool CodeMayBeSkipped(std::span<const uint32_t> code, std::span<const uint32_t> back_code,
                      bool front_only) {
	if (!back_code.empty()) {
		return ProgramMayBeSkipped(DecodeFrontProgram(code)) && CodeMayBeSkipped(back_code);
	}
	if (front_only) {
		return ProgramMayBeSkipped(DecodeFrontProgram(code));
	}
	Program program;
	DecodeProgram(code, program);
	return ProgramMayBeSkipped(program);
}

bool DrawSkipSafetyCache::MayBeSkipped(std::span<const uint32_t> code, uint64_t hash,
                                       std::span<const uint32_t> back_code, bool front_only,
                                       bool* newly_unsafe) {
	const uint64_t key = hash ^ (static_cast<uint64_t>(code.size()) * 0x9e3779b97f4a7c15ull) ^
	                     (static_cast<uint64_t>(back_code.size()) * 0xc2b2ae3d27d4eb4full) ^
	                     (front_only ? 0x165667b19e3779f9ull : 0ull);
	{
		std::lock_guard lock(m_mutex);
		if (const auto it = m_verdicts.find(key); it != m_verdicts.end()) {
			return it->second;
		}
	}
	const bool safe = CodeMayBeSkipped(code, back_code, front_only);
	std::lock_guard lock(m_mutex);
	const bool inserted = m_verdicts.try_emplace(key, safe).second;
	if (newly_unsafe != nullptr) {
		*newly_unsafe = inserted && !safe;
	}
	return safe;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
