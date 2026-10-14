#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct DescriptorValue {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	bool operator==(const DescriptorValue& other) const {
		return dword_count == other.dword_count && dwords == other.dwords;
	}
};

enum class UniformFillKind { None, Buffer, Image };

struct UniformFill {
	UniformFillKind          kind         = UniformFillKind::None;
	uint32_t                 resource     = 0;
	std::array<uint32_t, 3> group_stride {};
	uint32_t                 words        = 0;
	uint32_t                 value        = 0;

	bool operator==(const UniformFill&) const = default;
};

// A buffer store of a compute shader whose address is decided by the descriptor alone: the index,
// the vector offset and the scalar offset are immediates, so every lane writes the same bytes. The
// stored dwords that are immediates are the same for every lane and wave. `executes`: the store runs
// on at least one lane whenever its block runs (its exec guard is true or only bounds the invocation
// id by the dispatch extent).
struct UniformStoreSite {
	uint32_t                resource    = 0; // index into ResourceSnapshot::buffers
	uint32_t                index       = 0; // the immediate index operand (times the descriptor stride)
	uint32_t                byte_offset = 0; // voffset + soffset + the instruction offset
	uint32_t                block       = 0; // index into UniformStoreProgram::blocks
	uint32_t                dwords      = 1; // 1..4
	uint8_t                 known_mask  = 0; // bit i: dword i is the immediate value[i]
	bool                    executes    = false;
	std::array<uint32_t, 4> value {};

	bool operator==(const UniformStoreSite&) const = default;
};

// A branch condition (or a load feeding one) of a shader with UniformStoreSites, as a tiny expression
// DAG the renderer can evaluate on the CPU from the dispatch's descriptors and memory. Anything the
// translator could not express is Unknown, and Unknown makes the branch undecided (both ways may run).
struct StoreExpr {
	enum class Op : uint8_t {
		Unknown, Imm, Load /* raw buffer dword */, ScalarLoad /* scalar buffer dword */,
		And, Or, Xor, Shl, Shr, Add, Sub, Eq, Ne, Ult, Ule, Ugt, Uge, Not, LogicalAnd, LogicalOr, Select
	};
	Op       op       = Op::Unknown;
	uint16_t a        = 0, b = 0, c = 0; // operand nodes (Select: condition a, then b, else c)
	uint16_t resource = 0;                // Load / ScalarLoad: buffer resource
	uint32_t imm      = 0;                // Imm: the value
	uint32_t index    = 0;                // Load: the immediate index operand (times the descriptor stride)
	uint32_t offset   = 0;                // Load / ScalarLoad: byte offset (voffset + soffset + instruction offset)

	bool operator==(const StoreExpr&) const = default;
};

// One basic block of the shader's control flow, for the stores it holds.
struct StoreBlock {
	static constexpr uint32_t kNone = UINT32_MAX;
	uint32_t succ_true  = kNone; // Branch: the only successor; ConditionalBranch: taken when cond != 0
	uint32_t succ_false = kNone;
	int32_t  cond       = -1;    // StoreExpr index, -1: no condition (Branch or Return)
	uint32_t site_begin = 0, site_end = 0;

	bool operator==(const StoreBlock&) const = default;
};

// Every memory write of a compute shader is one of `sites`, the control flow between them is acyclic
// and `blocks` (entry first, in an order where predecessors come first) say which can run. Built by
// ExtractResourcePlan (AnalyzeUniformStores), shared by every snapshot of the shader. The renderer
// evaluates it per dispatch (graphics/host_gpu/renderer/cache/knownStores.h).
struct UniformStoreProgram {
	std::vector<UniformStoreSite> sites;
	std::vector<StoreBlock>       blocks;
	std::vector<StoreExpr>        exprs;
	std::vector<uint32_t>         order; // block indices, predecessors first, reachable only

	bool operator==(const UniformStoreProgram&) const = default;
};

// A compute shader that copies records from one buffer to another: one formatted store of the four
// components of one formatted load, both at the same index (local id + group stride * workgroup id),
// nothing else written. Whether it is an exact dword copy depends on the descriptors (formats,
// strides, extent); the renderer checks them (graphics/host_gpu/renderer/renderCompute.cpp).
struct UniformCopy {
	bool     valid        = false;
	uint32_t src_resource = 0;
	uint32_t dst_resource = 0;
	uint32_t group_stride = 0;

	bool operator==(const UniformCopy&) const = default;
};

struct ResourceSnapshot {
	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
	std::vector<std::pair<uint64_t, uint64_t>> specialization_reads;
	UniformFill                 uniform_fill;
	UniformCopy                 uniform_copy;
	// Null unless every memory write of the (compute) shader is a UniformStoreSite.
	std::shared_ptr<const UniformStoreProgram> uniform_stores;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
