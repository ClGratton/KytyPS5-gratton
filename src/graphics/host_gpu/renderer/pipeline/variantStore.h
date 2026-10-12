#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_VARIANTSTORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_VARIANTSTORE_H_

// Persisted shader variants (--pipeline-variants-replay, docs/shader-load-notes.md).
//
// A "variant" is the key of one translated compute program as the game used it: which shader binary
// (code hash and size), the static state the translator was given (workgroup size, LDS, wave size,
// the dispatch flags that change translation) and the resource specialization (buffer strides and
// formats, image classes) that the draw path derived from the descriptors. It holds no pointers
// and no code: the next session registers the same binary (AgcCreateShader), looks the hash up here
// and queues one program and one pipeline build per stored variant on the worker pool, seconds
// before the first dispatch. The file is generic run data (it lives in the run's _PipelineCache
// directory next to the driver cache), never part of the source tree.
//
// This header deliberately depends on the standard library only so it can be unit-tested alone.
//
// File format (little endian):
//   "KYTYVAR1" | u32 schema | records...
//   record = u32 payload_size | payload | u32 fnv1a32(payload)
//   payload = u8 kind (1 = compute, 2 = graphics) | fields (see Encode) | optional u32 first_use_ms
// A record that fails its checksum, has an unknown kind, or is cut short (a crash while appending)
// ends the readable part of the file; Load() reports how many bytes were good and OpenAppend()
// truncates the rest, so appends never land behind garbage.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::Variants {

// Bump when the meaning of a stored field changes. A file with another schema is ignored (and
// replaced by the next append): variants are only an accelerator, never needed for correctness.
inline constexpr uint32_t kSchema = 3;
// Upper bound on the buffers or images of one stored specialization (a corrupt record cannot ask for
// a huge allocation).
inline constexpr uint32_t kMaxResources = 256;

struct BufferSpec {
	uint32_t packed_stride            = 0;
	uint32_t descriptor_format        = 0;
	uint32_t descriptor_swizzle       = 0;
	uint32_t zero_stride_oob          = 0;
	uint32_t indirect_root            = 0xffffffffu;
	uint32_t indirect_mapping_offset  = 0;
	uint32_t indirect_search_iterations = 0;
	bool     operator==(const BufferSpec&) const = default;
};

struct ImageSpec {
	uint32_t numeric_class              = 0;
	uint32_t dimension                  = 0;
	uint32_t mip_count                  = 1;
	uint32_t conversion_format          = 0;
	uint32_t shader_swizzle             = 0;
	uint32_t indirect_root              = 0xffffffffu;
	uint32_t indirect_mapping_offset    = 0;
	uint32_t indirect_search_iterations = 0;
	uint32_t flags                      = 0; // bit 0 cube, bit 1 fmask
	bool     operator==(const ImageSpec&) const = default;
};

struct SpecRecord {
	std::vector<BufferSpec> buffers;
	std::vector<ImageSpec>  images;
	bool                    operator==(const SpecRecord&) const = default;
};

// The ShaderComputeInputInfo fields that BuildStageStaticKey() puts in the program key (and the
// user data count of ProgramKey), by value.
struct ComputeStatic {
	uint32_t user_data_count     = 0;
	uint32_t workgroup_register  = 0;
	uint32_t wave_size           = 64;
	uint32_t float_mode          = 0;
	uint32_t host_subgroup_size  = 64;
	uint32_t thread_ids_num      = 0;
	uint32_t lds_size_dwords     = 0;
	uint32_t scratch_size_dwords = 0;
	uint32_t threads_num[3]      = {0, 0, 0};
	// bit 0 dispatch_thread_dimensions, bit 1 async_compute, bit 2 tg_size_en, bits 3..5 group_id.
	uint32_t flags               = 0;
	bool     operator==(const ComputeStatic&) const = default;
};

struct ComputeVariant {
	// `first_use_ms` is when the first dispatch used the variant, in ms since the pipeline cache
	// was created: the replay builds the earliest-needed variants first. It is not part of the
	// variant's identity (equality, de-duplication) and old files without it read as "unknown".
	static constexpr uint32_t kUnknownFirstUse = 0xffffffffu;

	uint64_t       code_hash        = 0;
	uint32_t       code_size_dwords = 0;
	ComputeStatic  state;
	SpecRecord     spec;
	uint32_t       first_use_ms     = kUnknownFirstUse;
	bool           operator==(const ComputeVariant& other) const {
		return code_hash == other.code_hash && code_size_dwords == other.code_size_dwords &&
		       state == other.state && spec == other.spec;
	}

	// Hash of the shader binary only: the lookup key of the store.
	[[nodiscard]] static uint64_t BinaryKey(uint64_t code_hash, uint32_t code_size_dwords) {
		return code_hash ^ (static_cast<uint64_t>(code_size_dwords) * 0x9e3779b97f4a7c15ull);
	}
	[[nodiscard]] uint64_t BinaryKey() const { return BinaryKey(code_hash, code_size_dwords); }
};

// One shader stage program of a graphics pipeline as a draw used it: the program key (stage, code
// hash and size, user SGPR count, the static state words BuildStageStaticKey() produced), the push
// data cursor its layout was allocated at, the specialization, and the stage's input info as raw
// bytes (`input`, with its runtime pointers cleared; `input_layout` fingerprints the struct it came
// from, a reader with another layout ignores the record). The input bytes are payload, not identity.
struct ProgramVariant {
	uint8_t               stage            = 0; // ShaderType
	uint64_t              code_hash        = 0;
	uint32_t              code_size_dwords = 0;
	uint32_t              user_data_count  = 0;
	uint32_t              push_data_cursor = 0;
	std::vector<uint32_t> static_state;
	SpecRecord            spec;
	std::vector<uint8_t>  input;
	uint32_t              input_layout = 0;
	// A separately compiled back half (an NGG vertex shader with its GS back): hash and size of
	// its binary, 0 when the stage has none. The replay needs that binary registered as well.
	uint64_t              back_hash        = 0;
	uint32_t              back_size_dwords = 0;
	// With a back half `code_hash` is the program key's combined hash of both binaries; this is the
	// hash of the front binary itself, which is what the guest registers. 0 without a back half.
	uint64_t              front_hash       = 0;

	// The hash of the binary the guest registers for this stage's (front) code.
	[[nodiscard]] uint64_t RegistrationHash() const { return front_hash != 0 ? front_hash : code_hash; }

	bool operator==(const ProgramVariant& other) const {
		return stage == other.stage && code_hash == other.code_hash && back_hash == other.back_hash &&
		       front_hash == other.front_hash &&
		       back_size_dwords == other.back_size_dwords &&
		       code_size_dwords == other.code_size_dwords && user_data_count == other.user_data_count &&
		       push_data_cursor == other.push_data_cursor && static_state == other.static_state &&
		       spec == other.spec;
	}
};

// A graphics pipeline: its vertex-side and optional pixel program and the pipeline key (render
// formats, vertex input, fixed-function state) in a canonical byte encoding of the caller's
// choosing. The shader ids that the in-memory key holds are process-local and are not stored.
struct GraphicsVariant {
	ProgramVariant       vs;
	ProgramVariant       ps;
	bool                 has_ps = false;
	std::vector<uint8_t> key;
	uint32_t             first_use_ms = ComputeVariant::kUnknownFirstUse;

	bool operator==(const GraphicsVariant& other) const {
		return has_ps == other.has_ps && vs == other.vs && (!has_ps || ps == other.ps) && key == other.key;
	}
};

std::vector<uint8_t> Encode(const ComputeVariant& variant);
std::vector<uint8_t> Encode(const GraphicsVariant& variant);
// False for a malformed payload (unknown kind, short or oversized fields).
bool Decode(std::span<const uint8_t> payload, GraphicsVariant& variant);
bool Decode(std::span<const uint8_t> payload, ComputeVariant& variant);
uint32_t Checksum(std::span<const uint8_t> bytes);

class VariantStore {
public:
	struct LoadResult {
		size_t records     = 0; // records read (before de-duplication)
		size_t added       = 0; // new variants
		size_t good_bytes  = 0; // length of the readable prefix of the file
		bool   schema_ok   = true;
		bool   file_found  = false;
		bool   torn_tail   = false; // bytes after the last good record
	};

	VariantStore() = default;
	~VariantStore();
	VariantStore(const VariantStore&)            = delete;
	VariantStore& operator=(const VariantStore&) = delete;

	// Reads `path` into memory. Safe to call with a missing file.
	LoadResult Load(const std::filesystem::path& path);
	// Starts appending new variants to `path` (creating it with a header; truncating a torn tail or
	// a file with another schema). Without it Add() only records in memory.
	bool OpenAppend(const std::filesystem::path& path);
	void Close();

	// Adds a variant; false when an equal one is already stored. The record is appended (and
	// flushed) right away, so a crash keeps everything seen before it.
	bool Add(const ComputeVariant& variant);
	[[nodiscard]] bool Contains(const ComputeVariant& variant) const;
	// Every stored variant of one shader binary.
	[[nodiscard]] std::vector<ComputeVariant> Find(uint64_t code_hash, uint32_t code_size_dwords) const;
	[[nodiscard]] size_t Size() const;
	// A store never grows past this many variants (a changing game or translator would otherwise
	// keep adding stale records for ever); further Add() calls return false and are counted.
	static constexpr size_t kDefaultLimit = 50000;
	void                    SetLimit(size_t limit);
	[[nodiscard]] size_t    Dropped() const;

	// Graphics pipelines (same file, same rules).
	bool Add(const GraphicsVariant& variant);
	[[nodiscard]] bool   Contains(const GraphicsVariant& variant) const;
	[[nodiscard]] std::vector<GraphicsVariant> Graphics() const;
	[[nodiscard]] size_t GraphicsSize() const;

private:
	bool AddLocked(const ComputeVariant& variant);
	bool AddLocked(const GraphicsVariant& variant);
	bool AppendLocked(const std::vector<uint8_t>& payload);

	mutable std::mutex                                           m_mutex;
	std::unordered_map<uint64_t, std::vector<ComputeVariant>>    m_by_binary;
	size_t                                                       m_size = 0;
	size_t                                                       m_limit   = kDefaultLimit;
	size_t                                                       m_dropped = 0;
	std::vector<GraphicsVariant>                                 m_graphics;
	std::filesystem::path                                        m_path;
	std::FILE*                                                   m_file = nullptr;
	size_t                                                       m_good_bytes = 0;
	bool                                                         m_schema_ok  = true;
};

} // namespace Libs::Graphics::Variants

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_VARIANTSTORE_H_
