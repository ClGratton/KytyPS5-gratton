#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   zero_stride_oob                 = false;
		uint32_t               indirect_root                   = BufferResource::NoIndirectBuffer;
		uint32_t               indirect_mapping_offset         = 0;
		uint32_t               indirect_search_iterations      = 0;
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          fmask                      = false;
		// Generic only: the first member of a numeric-class group names itself, the others name
		// the first one; `class_mask` (first member only) has bit `1 << class` for every class the
		// group covers. The members follow the originals and are the same image in another class.
		uint32_t                      class_root                 = ImageResource::NoClassRoot;
		uint32_t                      class_mask                 = 0;
		bool                          operator==(const Image&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;
	// A generic variant (MakeGenericSpecialization): the fields below are read at run time, from
	// the dispatch's own specialization, and the entries here are only the hard fields.
	bool generic = false;

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Refreshes cached resources and specialization in place. A failed refresh must not be used.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization);

// The most likely specialization of a program before any descriptor exists (shader-load
// precompile): what MaterializeResources would produce if every descriptor had the commonest
// shape of its class (16-byte structured buffers, float images of the instruction's dimension). Returns false when the program cannot be guessed at all: it needs guest
// memory to specialize, uses bindless tables, or has typed buffers whose format only the
// descriptor knows. The guess is used for a speculative build and never for drawing.
bool GuessSpecialization(const ResourcePlan& program, ResourceSpecialization& specialization);

// ---- generic compute variants (--generic-compute) -----------------------------------------------
//
// A program is specialized on what the guest's descriptors say, and the descriptors do not exist
// when the shader is registered. Most of that is address arithmetic or selects, though, so a generic
// variant keeps only the fields that decide the *shape* of the SPIR-V (the hard signature) and reads
// the rest from the shader data:
//
//   hard (in the key):   image dimension (the instruction's), mip count, FMASK, conversion format,
//                        indirect tables, and the numeric class of an image whose instruction names
//                        it (sampling is float, atomics are unsigned)
//   run time:            buffer stride, swizzle, index stride and out-of-bounds mode; image cube flag
//                        and swizzle; the numeric class of an image that is only loaded or stored
//                        (a three-way switch over the float, unsigned and signed bindings)
//
// The variant is valid for a dispatch exactly when MatchGenericSpecialization says so; any other
// dispatch uses the specialized path unchanged.

enum class GenericRefusal : uint8_t {
	None,
	Incomplete,        // resource tracking did not finish
	NotCompute,
	NeedsGuestMemory,  // the descriptors are found by reading guest memory
	IndirectBuffer,
	IndirectImage,
	FormattedBuffer,   // an untyped formatted access: the descriptor's format decides the element
	ImageClass,        // no resource class, or the class has no binding
	ImageDimension,
	ImageBinding,      // a group member that has no binding (for instance an MSAA storage image)
	TooManyImages,
	CallSource,        // an S_SWAP_PC program: its callee is linked at run time (set by the cache)
	Unsupported,       // the translator does not implement the binary (set by the cache)
	BelowMinSize,      // shorter than --generic-compute-min-dwords: no variant is built (set by the cache)
	Count,
};

[[nodiscard]] const char* GenericRefusalName(GenericRefusal refusal);

/// The generic variant of a plan: false (with the reason) when its hard fields cannot be known
/// without a descriptor. `out` is only meaningful when the result is None.
[[nodiscard]] GenericRefusal MakeGenericSpecialization(const ResourcePlan&      plan,
                                                       ResourceSpecialization& out);

enum class GenericMismatch : uint8_t {
	None,
	NotGeneric,
	BufferCount,
	ImageCount,
	BufferIndirect,
	BufferAddTid,
	BufferSwizzle,
	ImageIndirect,
	ImageDimension,
	ImageClass,
	ImageMips,
	ImageConversion,
	ImageFmask,
	Count,
};

[[nodiscard]] const char* GenericMismatchName(GenericMismatch mismatch);

/// Whether a dispatch whose descriptors specialize to `live` may run the generic variant: its hard
/// signature has to equal the variant's. Cheap (one pass over the resources); the dispatch computes
/// `live` anyway to look up a permutation.
[[nodiscard]] GenericMismatch MatchGenericSpecialization(const ResourceSpecialization& generic,
                                                         const ResourceSpecialization& live);

/// Dynamic data word of a buffer / image (the layout the emitter decodes).
[[nodiscard]] uint32_t GenericBufferWord(const ResourceSpecialization::Buffer& live);
[[nodiscard]] uint32_t GenericImageWord(const ResourceSpecialization::Image& live);
constexpr uint32_t GenericBufferZeroOobBit = 1u << 21u;
constexpr uint32_t GenericBufferAddTidBit  = 1u << 20u;
constexpr uint32_t GenericBufferSwizzleBit = 1u << 14u; // with a stride: the tiled address calculation
constexpr uint32_t GenericImageCubeBit     = 1u;
constexpr uint32_t GenericImageClassShift  = 1u; // two bits: 0 float, 1 unsigned, 2 signed
constexpr uint32_t GenericImageSwizzleShift = 8u; // twelve bits

/// The member of a numeric-class group that holds the descriptor: 0 float, 1 unsigned, 2 signed.
[[nodiscard]] uint32_t GenericClassOrdinal(Prospero::TextureNumericClass numeric_class);

/// Fills `snapshot.generic_images` and `snapshot.generic_data` for a dispatch of a generic program:
/// the descriptors in the program's image order (inactive group members get an empty descriptor)
/// and the dynamic section of the shader data. `live` is the specialization materialized from the
/// same call as `snapshot`.
void BuildGenericBinding(const ShaderInfo& info, const Bindings& bindings,
                         const ResourceSpecialization& live, ResourceSnapshot& snapshot);

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
