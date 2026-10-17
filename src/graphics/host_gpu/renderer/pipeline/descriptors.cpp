#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <optional>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/gpuPhaseStats.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/compareView.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/resolvedDrawStats.h"
#include "graphics/host_gpu/renderer/pipeline/scalarReadHazard.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/skipDepsHooks.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace Libs::Graphics {

namespace {

using BindingKind = ShaderRecompiler::IR::DescriptorBindingKind;

// Times a target-flagged image without an attachment was bound as a sampled image (CommitBindings).
// The first kTargetFallbackLogLimit occurrences are logged, each with the running count; the summary
// at exit gives the total, so a session log shows whether the fallback fired and how often.
constexpr uint32_t    kTargetFallbackLogLimit = 16;
std::atomic<uint32_t> g_target_fallback_count {0};
std::once_flag        g_target_fallback_summary_once;

// Whether `format` can be sampled with a comparison sampler on this device
// (VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT); the answer per format never changes.
bool ViewFormatSupportsDepthComparison(const GraphicContext& graphics, vk::Format format) {
	// Draws repeat the same view format, so each thread remembers its last answer and skips the lock.
	thread_local VkPhysicalDevice last_device = nullptr;
	thread_local int              last_format = -1;
	thread_local bool             last_answer = false;
	const auto device = static_cast<VkPhysicalDevice>(graphics.physical_device);
	if (last_device == device && last_format == static_cast<int>(format)) {
		return last_answer;
	}
	static std::mutex                                       mutex;
	static std::map<std::pair<VkPhysicalDevice, int>, bool> answers;
	std::scoped_lock lock {mutex};
	auto [it, inserted] = answers.try_emplace(std::pair {device, static_cast<int>(format)}, false);
	if (inserted) {
		vk::FormatProperties3 properties3 {};
		vk::FormatProperties2 properties2 {};
		properties2.pNext = &properties3;
		graphics.physical_device.getFormatProperties2(format, &properties2);
		it->second = static_cast<bool>(properties3.optimalTilingFeatures &
		                               vk::FormatFeatureFlagBits2::eSampledImageDepthComparison);
	}
	last_device = device;
	last_format = static_cast<int>(format);
	last_answer = it->second;
	return it->second;
}

CompareViewReport g_compare_view_report;

// A texture slot the shader samples with a comparison sampler must be bound to a view whose format
// supports depth comparison (VUID-*-None-06479, see compareView.h). Safety net, not a fix: a colour
// image found at the address of a depth texture cannot be viewed as a depth format, so the draw
// still runs, with undefined behaviour on a GPU like NVIDIA's, and this says so, loudly, with
// everything needed to find where the image came from. Logs the first kLogLimit occurrences.
void ReportComparedView(const GraphicContext& graphics, const TextureBinding& binding,
                        const Image& image) {
	const auto host_view = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
	if (host_view == image.views.end()) {
		return;
	}
	const auto verdict = ClassifyComparedView(
	    true, ViewFormatSupportsDepthComparison(graphics, host_view->info.format));
	const auto count = g_compare_view_report.Note(verdict);
	if (!CompareViewReport::ShouldLog(count)) {
		return;
	}
	std::printf("Texture: depth-compare sampling through a view format the GPU cannot compare "
	            "(undefined behaviour, VUID-*-None-06479): addr=0x%016" PRIx64
	            " image=%u/%u image_format=%d view_format=%d depth_image=%d render_target=%d "
	            "tile=%u guest_format=%u (%" PRIu32 " so far)\n",
	            image.info.data.address, binding.image_id.index, binding.image_id.generation,
	            static_cast<int>(image.backing.format), static_cast<int>(host_view->info.format),
	            image.info.IsDepth() ? 1 : 0, image.binding.is_target ? 1 : 0,
	            static_cast<uint32_t>(image.info.tile_mode),
	            static_cast<uint32_t>(image.info.guest_format), count);
	std::fflush(stdout);
}

void PrintTargetFallbackSummary() {
	static std::atomic<bool> printed {false};
	if (!printed.exchange(true)) {
		VRAM_LOG("Texture: target-without-attachment fallback bound %u image(s) as sampled this "
		         "session\n",
		         g_target_fallback_count.load(std::memory_order_relaxed));
	}
}

} // namespace

void RegisterTargetFallbackSummary() {
	// Registered for both exit paths: the emulator normally leaves through std::quick_exit.
	std::call_once(g_target_fallback_summary_once, [] {
		std::atexit(PrintTargetFallbackSummary);
		std::at_quick_exit(PrintTargetFallbackSummary);
	});
}

vk::DescriptorType NativeDescriptorType(BindingKind kind) {
	const auto image_class = ShaderRecompiler::IR::ImageBindingResourceClass(kind);
	if (image_class == ShaderRecompiler::IR::ImageResourceClass::Sampled) {
		return vk::DescriptorType::eSampledImage;
	}
	if (image_class == ShaderRecompiler::IR::ImageResourceClass::Storage) {
		return vk::DescriptorType::eStorageImage;
	}
	switch (kind) {
		case BindingKind::Samplers: return vk::DescriptorType::eSampler;
		case BindingKind::Buffers:
		case BindingKind::Gds:
		case BindingKind::LoopGuard:
		case BindingKind::BdaPagetable:
		case BindingKind::FaultBuffer:
		case BindingKind::FlattenedSrt:
		case BindingKind::ShaderData:
		case BindingKind::SharedMemory: return vk::DescriptorType::eStorageBuffer;
		case BindingKind::Count: EXIT("invalid native descriptor binding kind");
	}
	EXIT("invalid native descriptor binding kind");
}

vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture, uint32_t element) {
	if (!texture.image_id) {
		EXIT_IF(texture.image_view != nullptr || !texture.mip_views.empty());
		return {nullptr, nullptr, vk::ImageLayout::eGeneral};
	}
	vk::ImageView view = nullptr;
	if (texture.mip_views.empty()) {
		if (element == 0u) {
			view = texture.image_view;
		}
	} else if (element < texture.mip_views.size()) {
		view = texture.mip_views[element];
	}
	EXIT_IF(view == nullptr || texture.layout == vk::ImageLayout::eUndefined);
	return {nullptr, view, texture.layout};
}

static Prospero::ImageType TextureType(const ShaderTextureResource& descriptor) {
	const auto type = descriptor.Type();
	return type == Prospero::ImageType::kCube ? Prospero::ImageType::kColor2DArray : type;
}

static Prospero::ImageType TextureBaseType(Prospero::ImageType type) {
	switch (type) {
		case Prospero::ImageType::kColor1DArray: return Prospero::ImageType::kColor1D;
		case Prospero::ImageType::kColor2DArray:
		case Prospero::ImageType::kColor2DMsaa:
		case Prospero::ImageType::kColor2DMsaaArray: return Prospero::ImageType::kColor2D;
		default: return type;
	}
}

static bool IsMultisampledTexture(Prospero::ImageType type) {
	return type == Prospero::ImageType::kColor2DMsaa ||
	       type == Prospero::ImageType::kColor2DMsaaArray;
}

static vk::DescriptorBufferInfo
NativeStorageBuffer(RenderContext& context, const PreparedBindings::BufferSource& source,
                    const ShaderRecompiler::IR::BufferResource& resource, uint32_t& buffer_offset) {
	buffer_offset = 0;

	const auto& [address, size, id] = source;
	if (address == 0 || size == 0) {
		return {context.GetBufferCache().GetBuffer(NULL_BUFFER_ID).Handle(), 0, 16};
	}
	const auto& graphics  = context.GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	if (size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange) {
		EXIT("storage buffer range is unsupported\n");
	}
	auto [buffer, offset] = context.GetBufferCache().ObtainBuffer(
	    address, size, resource.written,
	    resource.formatted || (resource.read && Config::SyncRawImageBuffersEnabled()), id);
	const auto aligned_offset = Common::AlignDown(offset, alignment);
	const auto adjustment     = offset - aligned_offset;
	const auto max_range      = graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
	if (adjustment >= 256 || size > max_range - adjustment) {
		EXIT("storage buffer offset adjustment is unsupported\n");
	}
	buffer_offset = static_cast<uint32_t>(adjustment);
	const vk::DescriptorBufferInfo result {buffer->Handle(), aligned_offset, size + adjustment};
	if (resource.written) {
		context.GetTextureCache().InvalidateMemoryFromGPU(address, size);
	}
	return result;
}

bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor, bool r128) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	const uint32_t     field3_expected = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                     (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t     field4_expected = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	const uint32_t     field5_expected = (static_cast<uint32_t>(descriptor.PerfMod5()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.MaxMip()) << 4u);
	const bool         common          = (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	                                     (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	                                     descriptor.fields[3] == field3_expected;
	if (r128) {
		return common && descriptor.fields[4] == 0 && descriptor.fields[5] == 0 &&
		       descriptor.fields[6] == 0 && descriptor.fields[7] == 0;
	}
	const bool full = common && descriptor.fields[4] == field4_expected &&
	                  descriptor.fields[5] == field5_expected;
	if (!full ||
	    (descriptor.MsaaDepth() && !IsMultisampledTexture(descriptor.Type()))) {
		return false;
	}
	const auto metadata_control = descriptor.fields[6] & 0x00ffffffu;
	if (metadata_control == 0) {
		return true;
	}
	constexpr uint32_t htile_control = 0x00280000u;
	const uint32_t expected_control  = htile_control | (descriptor.MsaaDepth() ? (1u << 10u) : 0u);
	const auto     metadata_addr     = descriptor.MetaAddr() << 8u;
	return metadata_control == expected_control && GuestRange {metadata_addr, 1}.Valid() &&
	       (metadata_addr & 0x7fffu) == 0 &&
	       descriptor.TileMode() == Prospero::TileMode::kDepth;
}

static void ValidateSampledDepthBinding(const ShaderRecompiler::IR::ImageResource& resource,
                                        const ShaderTextureResource& descriptor, const Image& image,
                                        vk::Format view_format, uint64_t size) {
	const bool resource_ok = IsSupportedSampledDepthResource(resource);
	// Comparison sampling changes the host format, not the guest metadata encoding.
	const bool encoding_ok = descriptor.TileMode() != Prospero::TileMode::kDepth ||
	                         IsSupportedDepthTextureEncoding(descriptor, resource.r128);
	const bool view_ok =
	    IsSupportedSampledDepthView(image.info.pixel_format, view_format, descriptor.DstSelXYZW());
	if (resource_ok && encoding_ok && view_ok) {
		return;
	}
	const auto descriptor_pitch =
	    TileGetTexturePitch(descriptor.Format(), static_cast<uint32_t>(descriptor.Width5()) + 1u,
	                        descriptor.TileMode());
	EXIT("unsupported sampled depth image: resource=%d encoding=%d view=%d "
	     "class=%u numeric=%u dimension=%u mip_mode=%u read=%d written=%d atomic=%d compare=%d "
	     "guest_format=%u swizzle=0x%03x image_format=%d view_format=%d image_layers=%u "
	     "descriptor_type=%u base_array=%u depth=%u descriptor_pitch=%u target_pitch=%u "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, encoding_ok, view_ok,
	     static_cast<uint32_t>(resource.resource_class),
	     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
	     static_cast<uint32_t>(resource.mip_mode), resource.read, resource.written, resource.atomic,
	     resource.depth_compare, static_cast<uint32_t>(descriptor.Format()),
	     descriptor.DstSelXYZW(), static_cast<int>(image.info.pixel_format),
	     static_cast<int>(view_format), image.info.resources.layers,
	     static_cast<uint32_t>(descriptor.Type()), descriptor.BaseArray5(), descriptor.Depth(),
	     descriptor_pitch, image.info.pitch, descriptor.Base40(), size, descriptor.fields[0],
	     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
	     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

static bool IsSupportedStorageTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                                                const ShaderTextureResource& descriptor) {
	const auto tile              = descriptor.TileMode();
	const bool is_color_1d       = descriptor.Type() == Prospero::ImageType::kColor1D;
	const bool is_color_1d_array = descriptor.Type() == Prospero::ImageType::kColor1DArray;
	const bool valid_1d_slice =
	    (is_color_1d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_1d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_1d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1D &&
	                   descriptor.Height5() == 0 && valid_1d_slice;
	const bool is_1d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1DArray &&
	    is_color_1d_array && descriptor.Height5() == 0 &&
	    descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_color_2d       = descriptor.Type() == Prospero::ImageType::kColor2D;
	const bool is_color_2d_array = descriptor.Type() == Prospero::ImageType::kColor2DArray;
	const bool valid_2d_slice =
	    (is_color_2d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_2d =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D && valid_2d_slice;
	// Storage cube coordinates address individual faces, including partial cube views.
	const bool is_cube = resource.cube && descriptor.Type() == Prospero::ImageType::kCube &&
	                     descriptor.Width5() == descriptor.Height5() &&
	                     descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_2d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
	    ((!resource.cube && is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth()) ||
	     is_cube);
	const bool is_3d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
	                   descriptor.Type() == Prospero::ImageType::kColor3D &&
	                   descriptor.BaseArray5() == 0;
	TileTextureBlockLayout tile_layout {};
	bool                   supported_tile = false;
	switch (tile) {
		case Prospero::TileMode::kLinear: supported_tile = true; break;
		case Prospero::TileMode::kDepth:
			supported_tile =
			    !resource.read && !Prospero::IsFmaskTextureFormat(descriptor.Format()) &&
			    (is_2d || is_2d_array) &&
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		case Prospero::TileMode::kStandard256B:
			supported_tile =
			    (is_2d || is_2d_array) &&
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		case Prospero::TileMode::kStandard4KB:
		case Prospero::TileMode::kStandard64KB:
			supported_tile =
			    TileGetTextureBlockLayout(descriptor.Format(), tile, is_3d, tile_layout);
			break;
		case Prospero::TileMode::kRenderTarget:
			supported_tile =
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		default: break;
	}
	const auto swizzle = descriptor.DstSelXYZW();
	const bool supported_swizzle =
	    IsValidImageSwizzle(swizzle) &&
	    (swizzle == DstSel(4, 5, 6, 7) || !resource.read || resource.atomic);
	return (is_1d || is_1d_array || is_2d || is_2d_array || is_3d) && supported_tile &&
	       descriptor.BaseLevel() <= descriptor.LastLevel() &&
	       descriptor.MinLod() == 0 && supported_swizzle && descriptor.BCSwizzle() == 0 &&
	       !descriptor.MsaaDepth();
}

static bool IsSupportedStorageTextureEncoding(const ShaderRecompiler::IR::ImageResource& resource,
                                              const ShaderTextureResource& descriptor) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	constexpr uint32_t field5_expected      = 0x00700000u;
	constexpr uint32_t field5_max_mip_mask  = 0x000000f0u;
	const uint32_t     expected_field3 = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                     (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t     expected_field4 =
	    descriptor.Depth() | (static_cast<uint32_t>(descriptor.BaseArray5()) << 16u);
	const bool common = (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	                    (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	                    descriptor.fields[3] == expected_field3;
	if (resource.r128) {
		return common && descriptor.fields[4] == 0 && descriptor.fields[5] == 0 &&
		       descriptor.fields[6] == 0 && descriptor.fields[7] == 0;
	}
	return common && descriptor.fields[4] == expected_field4 &&
	       (descriptor.fields[5] & ~field5_max_mip_mask) == field5_expected;
}

void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size) {
	const auto format        = descriptor.Format();
	const bool resource_ok   = IsSupportedStorageImageResource(resource);
	const bool descriptor_ok = IsSupportedStorageTextureDescriptor(resource, descriptor);
	const bool encoding_ok   = IsSupportedStorageTextureEncoding(resource, descriptor);
	const bool uint_resource    = resource.numeric_class == Prospero::TextureNumericClass::Uint;
	const bool raw_sint_storage = format == Prospero::BufferFormat::k32SInt && uint_resource &&
	                              resource.written && !resource.read && !resource.atomic;
	const auto numeric_class = Prospero::SampledTextureNumericClass(format);
	const bool raw_float_atomic = format == Prospero::BufferFormat::k32Float && uint_resource &&
	                              resource.atomic && !resource.atomic64;
	// 32-bit atomics are typed by the opcode: a SINT view is bound as R32_UINT like a UINT one.
	const bool raw_sint_atomic = format == Prospero::BufferFormat::k32SInt && uint_resource &&
	                             resource.atomic && !resource.atomic64;
	const bool format_ok =
	    raw_sint_storage || raw_float_atomic || raw_sint_atomic ||
	    (numeric_class != Prospero::TextureNumericClass::Unsupported &&
	     numeric_class != Prospero::TextureNumericClass::Sint &&
	     uint_resource == (numeric_class == Prospero::TextureNumericClass::Uint) &&
	     (!resource.atomic || format == (resource.atomic64 ? Prospero::BufferFormat::k32_32UInt
	                                                       : Prospero::BufferFormat::k32UInt)));
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	EXIT("unsupported storage texture: resource=%d descriptor=%d encoding=%d format=%d "
	     "class=%u numeric=%u dimension=%u mip_mode=%u atomic=%d compare=%d "
	     "base_level=%u last_level=%u max_mip=%u min_lod=%u base_array=%u bc=%u msaa=%d "
	     "depth_tile_bpe=%u swizzle_ok=%d "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x read=%d written=%d "
	     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok,
	     static_cast<uint32_t>(resource.resource_class),
	     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
	     static_cast<uint32_t>(resource.mip_mode), resource.atomic, resource.depth_compare,
	     descriptor.BaseLevel(), descriptor.LastLevel(), descriptor.MaxMip(), descriptor.MinLod(),
	     descriptor.BaseArray5(), descriptor.BCSwizzle(), descriptor.MsaaDepth(),
	     Prospero::RenderTargetBytesPerElement(format),
	     IsValidImageSwizzle(descriptor.DstSelXYZW()), descriptor.Base40(), size,
	     static_cast<uint32_t>(descriptor.Width5()) + 1u,
	     static_cast<uint32_t>(descriptor.Height5()) + 1u,
	     static_cast<uint32_t>(descriptor.Depth()) + 1u, static_cast<uint32_t>(descriptor.Type()),
	     static_cast<uint32_t>(format), static_cast<uint32_t>(descriptor.TileMode()),
	     descriptor.DstSelXYZW(), resource.read, resource.written, descriptor.fields[0],
	     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
	     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

static ImageViewInfo TextureViewInfo(const ShaderRecompiler::IR::ImageResource& resource,
                                     const ShaderTextureResource& descriptor, vk::Format format,
                                     const SurfaceFormatInfo& surface_format, bool storage,
                                     uint32_t view_levels, uint32_t image_layers) {
	ImageViewInfo view {};
	view.format      = format;
	view.aspect      = vk::ImageAspectFlagBits::eColor;
	view.base_level  = descriptor.BaseLevel();
	view.level_count = view_levels;
	if (descriptor.MinLod() > descriptor.LastLevel() * 256u) {
		EXIT("texture minimum LOD exceeds last mip level: min_lod=%u last_level=%u\n",
		     descriptor.MinLod(), descriptor.LastLevel());
	}
	const auto base_lod = view.base_level * 256u;
	if (descriptor.MinLod() > base_lod) {
		view.min_lod = descriptor.MinLod() - base_lod;
	}
	view.usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
	view.mapping =
	    storage || surface_format.conversion_format != Prospero::BufferFormat::kInvalid
	        ? vk::ComponentMapping {}
	        : TextureGetComponentMapping(descriptor.DstSelXYZW(), surface_format.host_to_storage);
	switch (resource.dimension) {
		case ShaderRecompiler::Decoder::ImageDimension::Dim1D:
			view.type       = vk::ImageViewType::e1D;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture base layer is out of bounds\n");
			}
			view.layer_count = 1;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim1DArray:
			view.type       = vk::ImageViewType::e1DArray;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture array base layer is out of bounds\n");
			}
			view.layer_count = image_layers - view.base_layer;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim3D:
			view.type        = vk::ImageViewType::e3D;
			view.base_layer  = 0;
			view.layer_count = 1;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DArray:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray:
			view.type       = vk::ImageViewType::e2DArray;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture array base layer is out of bounds\n");
			}
			view.layer_count = image_layers - view.base_layer;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim2D:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa:
			view.type       = vk::ImageViewType::e2D;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture base layer is out of bounds\n");
			}
			view.layer_count = 1;
			break;
		default: EXIT("unsupported texture view dimension\n");
	}
	return view;
}

static bool ResolveTextureMipView(ImageInfo& info, uint32_t physical_levels, bool metadata,
                                  uint32_t view_levels, uint32_t& base_level) {
	if (!info.IsTiled()) {
		return false;
	}
	ImageInfo physical        = info;
	physical.resources.levels = physical_levels;
	physical.UpdateSize();
	info.UpdateSize();
	const auto same_mip = [&](uint32_t target, uint32_t source) {
		const auto& left  = physical.mip_layout[target];
		const auto& right = info.mip_layout[source];
		return left.offset == right.offset && left.size == right.size &&
		       left.pitch == right.pitch && left.height == right.height &&
		       left.tail_x == right.tail_x && left.tail_y == right.tail_y &&
		       left.surface_z == right.surface_z &&
		       physical.MipExtent(target) == info.MipExtent(source);
	};
	if (physical.first_tail_level == info.first_tail_level &&
	    physical.tiled_slice_stride == info.tiled_slice_stride &&
	    physical.data.size == info.data.size) {
		bool matches = true;
		for (uint32_t level = 0; level < physical_levels; ++level) {
			matches &= same_mip(level, level);
		}
		if (matches) {
			return true;
		}
	}
	if (metadata ||
	    (info.TransferLayers() > 1 && physical.tiled_slice_stride != info.tiled_slice_stride)) {
		return false;
	}
	// T# addresses the last mip. A view can select the same stored subresources
	// with different mip indices; inaccessible mips need no host representation.
	for (uint32_t base = 0; base + view_levels <= physical_levels; ++base) {
		bool matches = true;
		for (uint32_t i = 0; i < view_levels; ++i) {
			const auto source = base_level + i;
			const auto target = base + i;
			if (!same_mip(target, source) ||
			    (target >= physical.first_tail_level) != (source >= info.first_tail_level)) {
				matches = false;
				break;
			}
		}
		if (matches) {
			info       = std::move(physical);
			base_level = base;
			return true;
		}
	}
	return false;
}

namespace {

// What ResolveTexture works out from a texture descriptor before it asks the texture cache: a pure function of the shader's image resource and
// the descriptor dwords (it is also where an unsupported descriptor stops the emulator).
struct DecodedTexture {
	TextureCache::ImageDesc desc;
	bool                    shader_conversion = false;
	vk::Format              pixel_format      = vk::Format::eUndefined;
	vk::Format              view_format       = vk::Format::eUndefined;
};

void DecodeTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                             const ShaderTextureResource& descriptor, bool storage, DecodedTexture& out) {
	const auto address         = descriptor.Base40();
	const auto width           = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height          = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto base_level      = descriptor.BaseLevel();
	const auto last_level      = descriptor.LastLevel();
	const auto type            = TextureType(descriptor);
	const bool multisampled    = IsMultisampledTexture(type);
	const auto max_mip         = resource.r128 ? last_level : descriptor.MaxMip();
	const auto physical_levels = multisampled ? 1u : static_cast<uint32_t>(max_mip) + 1u;
	// IMAGE_STORE addresses BASE_LEVEL; only IMAGE_STORE_MIP selects other view mips.
	const bool single_storage_mip =
	    storage && resource.mip_mode != ShaderRecompiler::IR::ImageMipMode::Dynamic;
	const auto view_levels = multisampled || single_storage_mip
	                             ? 1u
	                             : static_cast<uint32_t>(last_level - base_level) + 1u;
	auto levels =
	    multisampled ? 1u : std::max(physical_levels, base_level + view_levels);
	const auto tile       = descriptor.TileMode();
	const bool depth_tile = tile == Prospero::TileMode::kDepth;
	const bool msaa_tile  = depth_tile || tile == Prospero::TileMode::kRenderTarget;
	const bool msaa_array = type == Prospero::ImageType::kColor2DMsaaArray;
	if ((!multisampled && base_level > last_level) ||
	    (multisampled &&
	     (base_level != 0 || last_level == 0 || last_level > 3 || max_mip != last_level ||
	      !msaa_tile || (descriptor.MsaaDepth() && !depth_tile) ||
	      (!msaa_array && (descriptor.Depth() != 0 || descriptor.BaseArray5() != 0))))) {
		EXIT("unsupported texture mip view: base=%u last=%u levels=%u max=%u type=%u tile=%u "
		     "class=%u numeric=%u dimension=%u mip_mode=%u read=%d written=%d "
		     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
		     base_level, last_level, levels, descriptor.MaxMip(),
		     static_cast<uint32_t>(descriptor.Type()), static_cast<uint32_t>(tile),
		     static_cast<uint32_t>(resource.resource_class),
		     static_cast<uint32_t>(resource.numeric_class),
		     static_cast<uint32_t>(resource.dimension), static_cast<uint32_t>(resource.mip_mode),
		     resource.read, resource.written, descriptor.fields[0], descriptor.fields[1],
		     descriptor.fields[2], descriptor.fields[3], descriptor.fields[4], descriptor.fields[5],
		     descriptor.fields[6], descriptor.fields[7]);
	}
	const auto samples = multisampled ? 1u << last_level : 1u;
	const auto depth          = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	const auto format         = descriptor.Format();
	const auto surface_format = TextureGetSurfaceFormatInfo(format);
	const bool shader_conversion =
	    surface_format.conversion_format != Prospero::BufferFormat::kInvalid;
	const bool sampled_numeric_class =
	    storage || resource.numeric_class == Prospero::SampledTextureNumericClass(format);
	if (!storage && resource.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled &&
	    !sampled_numeric_class) {
		EXIT("sampled image numeric class mismatch: numeric=%u format=%u addr=0x%016" PRIx64 "\n",
		     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(format), address);
	}

	const bool    volume       = type == Prospero::ImageType::kColor3D;
	const bool    layered      = type == Prospero::ImageType::kColor1DArray ||
	                             type == Prospero::ImageType::kColor2DArray ||
	                             type == Prospero::ImageType::kColor2DMsaaArray;
	const auto    image_layers = layered ? depth : 1u;
	auto          view_base    = static_cast<uint32_t>(base_level);
	auto pixel_format = surface_format.vk_format;
	if (resource.depth_compare) {
		if (const auto* depth_format = FindGuestDepthFormatPolicy(format)) {
			pixel_format = depth_format->depth_attachment_format;
		}
	}
	const auto storage_view_format = resource.atomic64 ? vk::Format::eR64Uint
	                                 : storage && (resource.atomic ||
	                                               format == Prospero::BufferFormat::k32SInt)
	                                     ? vk::Format::eR32Uint
	                                     : SrgbStorageViewFormat(pixel_format);
	const auto view_format         = storage && storage_view_format != vk::Format::eUndefined
	                                     ? storage_view_format
	                                     : pixel_format;
	const auto block_bytes         = Prospero::BlockCompressedBytesPerBlock(format);
	auto& desc = out.desc;
	desc = {};
	desc.info.data         = {address, 0};
	desc.info.pixel_format = pixel_format;
	desc.info.guest_format = format;
	desc.info.type         = TextureBaseType(type);
	desc.info.extent       = {width, height, volume ? depth : 1u};
	desc.info.resources    = {levels, image_layers};
	desc.info.bytes_per_block =
	    block_bytes != 0 ? block_bytes : Prospero::NumBytesPerElement(format);
	desc.info.samples   = samples;
	desc.info.tile_mode = tile;
	if (levels > physical_levels) {
		if (!ResolveTextureMipView(desc.info, physical_levels,
		                           !resource.r128 && descriptor.MetaCompress(), view_levels,
		                           view_base)) {
			EXIT("unsupported texture mip view changes physical layout: base=%u last=%u max=%u "
			     "extent=%ux%ux%u tile=%u\n",
			     base_level, last_level, max_mip, width, height, depth,
			     static_cast<uint32_t>(tile));
		}
	} else {
		desc.info.UpdateSize();
	}
	const auto alignment =
	    tile == Prospero::TileMode::kLinear ? 256u : desc.info.tiling.block.block_size;
	EXIT_NOT_IMPLEMENTED(desc.info.data.size == 0 || alignment == 0 ||
	                     (address & (static_cast<uint64_t>(alignment) - 1u)) != 0);
	if (storage) {
		ValidateStorageTexture(resource, descriptor, desc.info.data.size);
	}
	if (!resource.r128 && descriptor.MetaCompress() && tile != Prospero::TileMode::kDepth) {
		TileSizeAlign metadata_size {};
		(void)TileGetDccSize(width, height, volume ? depth : image_layers,
		                     desc.info.bytes_per_block, physical_levels, tile, metadata_size,
		                     std::countr_zero(samples));
		desc.info.metadata.kind          = ImageMetadataKind::Dcc;
		desc.info.metadata.range         = {descriptor.MetaAddr() << 8u, metadata_size.size};
		desc.info.metadata.dcc_alpha_msb = descriptor.DccAlphaPos();
	}
	desc.view_info = TextureViewInfo(resource, descriptor, view_format, surface_format, storage,
	                                 view_levels, desc.info.resources.layers);
	desc.view_info.base_level = view_base;
	desc.type = storage ? TextureCache::BindingType::Storage : TextureCache::BindingType::Texture;

	out.shader_conversion = shader_conversion;
	out.pixel_format      = pixel_format;
	out.view_format       = view_format;
}

// --draw-state-memo: descriptors repeat (a texture is bound by many draws, and every frame again), so the last decodes are kept, found by the
// descriptor dwords and compared with the resource they were decoded for.
struct DecodeMemoEntry {
	bool                                         valid = false;
	ShaderRecompiler::IR::ImageResource          resource;
	ShaderRecompiler::IR::DescriptorValue        value;
	bool                                         storage = false;
	DecodedTexture                               decoded;
};

bool SameDecodedDesc(const TextureCache::ImageDesc& a, const TextureCache::ImageDesc& b) {
	const auto& x = a.info;
	const auto& y = b.info;
	bool same = a.type == b.type && a.view_info == b.view_info && x.data == y.data && x.stencil == y.stencil &&
	            x.extent == y.extent && x.resources == y.resources && x.pixel_format == y.pixel_format &&
	            x.guest_format == y.guest_format && x.bytes_per_block == y.bytes_per_block && x.samples == y.samples &&
	            x.tile_mode == y.tile_mode && x.type == y.type && x.pitch == y.pitch && x.bgra16 == y.bgra16 &&
	            x.metadata.range == y.metadata.range && x.metadata.kind == y.metadata.kind &&
	            x.metadata.dcc_alpha_msb == y.metadata.dcc_alpha_msb;
	for (uint32_t level = 0; same && level < x.resources.levels && level < x.mip_layout.size(); ++level) {
		same = x.mip_layout[level] == y.mip_layout[level];
	}
	return same;
}

} // namespace

TextureBinding RenderExecutor::ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
                                              const ShaderRecompiler::IR::DescriptorValue& value,
                                              PreparedBindings::ImageSource*               source) {
	if (source != nullptr) {
		source->generation = 0;
	}
	if (resource.atomic64 && !m_context.GetGraphics().shader_image_int64_atomics_enabled) {
		EXIT("64-bit image atomics require shaderImageInt64Atomics\n");
	}
	auto descriptor = DecodeNativeDescriptor<ShaderTextureResource>(value);
	const bool storage = resource.written;
	if (storage) {
		ValidateStorageImageResource(resource);
	}

	auto& texture_cache = m_context.GetTextureCache();
	if (descriptor.IsNull()) {
		return {.layout = vk::ImageLayout::eGeneral};
	}

	// Binding resolves every texture a shader references, and while a level loads some slots still
	// hold stale descriptors that the draw never samples (the GPU does not read them). A sampled
	// view of eight dwords that cannot describe an image at all (a base mip above the last, a
	// format with no texel layout) is bound as the null image, loudly; a storage view is written
	// back and still has to be exact. A well-formed descriptor of an unsupported feature is not
	// this case and keeps its fatal report.
	if (!storage) {
		const char* invalid = nullptr;
		if (!IsMultisampledTexture(TextureType(descriptor)) &&
		    descriptor.BaseLevel() > descriptor.LastLevel()) {
			invalid = "base mip above last mip";
		} else if (TileTextureElementLayout element {};
		           !TileGetTextureElementLayout(descriptor.Format(), element)) {
			invalid = "format without a texel layout";
		}
		if (invalid != nullptr) {
			static std::atomic<uint32_t> reported {0};
			const auto                   count = reported.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= 32) {
				std::printf("Texture: sampled descriptor is not an image (%s), bound as null: "
				            "addr=0x%016" PRIx64 " format=%u dwords=%08x,%08x,%08x,%08x,%08x,%08x,"
				            "%08x,%08x (%" PRIu32 " so far)\n",
				            invalid, descriptor.Base40(),
				            static_cast<uint32_t>(descriptor.Format()), descriptor.fields[0],
				            descriptor.fields[1], descriptor.fields[2], descriptor.fields[3],
				            descriptor.fields[4], descriptor.fields[5], descriptor.fields[6],
				            descriptor.fields[7], count);
				std::fflush(stdout);
			}
			return {.layout = vk::ImageLayout::eGeneral};
		}
	}

	TextureCache::ImageDesc desc;
	bool                    shader_conversion = false;
	vk::Format              pixel_format      = vk::Format::eUndefined;
	vk::Format              view_format       = vk::Format::eUndefined;
	{
		static thread_local std::array<DecodeMemoEntry, 256> memo;
		const bool use_memo = Config::DrawStateMemoEnabled();
		DecodeMemoEntry* entry = nullptr;
		if (use_memo) {
			uint64_t hash = 0x9e3779b97f4a7c15ull ^ static_cast<uint64_t>(storage);
			for (uint32_t i = 0; i < 8; ++i) {
				hash = (hash ^ value.dwords[i]) * 0x100000001b3ull;
			}
			hash ^= reinterpret_cast<uintptr_t>(&resource) >> 3u;
			entry = &memo[(hash ^ (hash >> 29u)) % memo.size()];
		}
		if (entry != nullptr && entry->valid && entry->storage == storage && entry->value == value &&
		    entry->resource == resource) {
			desc              = entry->decoded.desc;
			shader_conversion = entry->decoded.shader_conversion;
			pixel_format      = entry->decoded.pixel_format;
			view_format       = entry->decoded.view_format;
			if (Config::ResolvedDrawVerify()) {
				DecodedTexture fresh;
				DecodeTextureDescriptor(resource, descriptor, storage, fresh);
				if (!SameDecodedDesc(fresh.desc, desc) || fresh.shader_conversion != shader_conversion ||
				    fresh.pixel_format != pixel_format || fresh.view_format != view_format) {
					EXIT("draw state memo: a remembered texture decode differs from a fresh one, address=0x%016" PRIx64 "\n",
					     descriptor.Base40());
				}
				static std::atomic<uint64_t> verified {0};
				if ((verified.fetch_add(1, std::memory_order_relaxed) + 1) % (1u << 20) == 0) {
					std::printf("DrawStateMemo verify: %" PRIu64 " remembered texture decodes agree with a fresh decode\n",
					            verified.load(std::memory_order_relaxed));
					std::fflush(stdout);
				}
			}
		} else {
			DecodedTexture decoded;
			DecodeTextureDescriptor(resource, descriptor, storage, decoded);
			desc              = decoded.desc;
			shader_conversion = decoded.shader_conversion;
			pixel_format      = decoded.pixel_format;
			view_format       = decoded.view_format;
			if (entry != nullptr) {
				entry->valid    = true;
				entry->resource = resource;
				entry->value    = value;
				entry->storage  = storage;
				entry->decoded  = std::move(decoded);
			}
		}
	}
	const auto metadata_base_layer = desc.view_info.base_layer;
	uint64_t   generation          = 0;
	const bool record_source       = source != nullptr && Config::TextureBindingReuseEnabled();
	auto       id = texture_cache.FindImage(desc, shader_conversion, record_source ? &generation : nullptr);
	if (record_source && generation != 0) {
		source->resource            = resource;
		source->resource_address    = &resource;
		source->value               = value;
		source->found               = id;
		source->generation          = generation;
		source->metadata_base_layer = metadata_base_layer;
	}
	auto*      image               = &texture_cache.GetImage(id);
	const bool stencil_association = static_cast<bool>(image->depth_id);
	if (stencil_association) {
		id    = image->depth_id;
		image = &texture_cache.GetImage(id);
	} else if (image->info.IsDepth()) {
		if (storage) {
			EXIT("depth target cannot be bound as a storage image\n");
		}
		ValidateSampledDepthBinding(resource, descriptor, *image, pixel_format,
		                            desc.info.data.size);
	} else if (storage) {
		ValidateStorageColorView(image->info.pixel_format, view_format, descriptor.DstSelXYZW());
	} else {
		(void)SelectSampledColorView(image->info.pixel_format, pixel_format,
		                             descriptor.DstSelXYZW());
	}
	return {id, nullptr, std::move(desc)};
}

static vk::Sampler NativeSampler(RenderContext&                       context,
                                 const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                 uint32_t index,
                                 const ShaderRecompiler::IR::DescriptorValue& value) {
	auto        descriptor = DecodeNativeDescriptor<ShaderSamplerResource>(value);
	const auto& sampler = program.info.samplers[index];
	if (!sampler.depth_compare) {
		descriptor.fields[0] &= ~(0x7u << 12u);
	}
	if (sampler.force_point_filtering) {
		descriptor.SetPointFiltering();
	}
	return context.GetSamplerCache().GetSampler(descriptor, sampler.integer_border);
}

static vk::DescriptorBufferInfo NativeUpload(RenderContext&            context,
                                             std::span<const uint32_t> data) {
	Common::GpuPhase::Scope gpu_phase_upload(Common::GpuPhase::Phase::DescUpload);
	EXIT_IF(data.empty());
	auto& command_buffer = context.GetCommandScheduler().Current();
	EXIT_IF(command_buffer.IsInvalid());
	auto&      buffer = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
	const auto offset = buffer.Copy(data.data(), data.size_bytes(), 256);
	return {buffer.Handle(), offset, data.size_bytes()};
}

void RenderExecutor::BindImage(ImageId id, bool storage) {
	if (!id) {
		return;
	}
	auto& image = m_context.GetTextureCache().GetImage(id);
	if (image.binding.is_bound) {
		image.binding.force_general |= image.binding.shader_write != storage;
	}
	image.binding.is_bound = true;
	image.binding.shader_write |= storage;
	m_bound_images.push_back(id);
}

void RenderExecutor::BindRenderTarget(ImageId id) {
	auto& image             = m_context.GetTextureCache().GetImage(id);
	image.binding.is_target = true;
	m_bound_images.push_back(id);
}

void RenderExecutor::ResetBindings() {
	for (const auto id: m_bound_images) {
		if (auto* image = m_context.GetTextureCache().m_slot_images.try_get(id); image != nullptr) {
			image->binding = {};
		}
	}
	m_bound_images.clear();
}

// KYTY_TEXTURE_REUSE_VERIFY=1: every kept binding is compared with a fresh ResolveTexture.
static bool TextureReuseVerifyEnabled() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_TEXTURE_REUSE_VERIFY");
		return text != nullptr && text[0] != '\0' && text[0] != '0';
	}();
	return enabled;
}

static void VerifyKeptTexture(const TextureBinding& kept, const TextureBinding& fresh) {
	static std::atomic<uint64_t> verified {0};
	const auto& a = kept.desc.info;
	const auto& b = fresh.desc.info;
	bool same = kept.image_id == fresh.image_id && kept.desc.type == fresh.desc.type &&
	            kept.desc.view_info == fresh.desc.view_info && a.data == b.data &&
	            a.stencil == b.stencil && a.extent == b.extent && a.resources == b.resources &&
	            a.pixel_format == b.pixel_format && a.guest_format == b.guest_format &&
	            a.bytes_per_block == b.bytes_per_block && a.samples == b.samples &&
	            a.tile_mode == b.tile_mode && a.type == b.type && a.pitch == b.pitch &&
	            a.bgra16 == b.bgra16 && a.metadata.range == b.metadata.range &&
	            a.metadata.kind == b.metadata.kind &&
	            a.metadata.dcc_alpha_msb == b.metadata.dcc_alpha_msb;
	for (uint32_t level = 0; same && level < a.resources.levels && level < a.mip_layout.size(); ++level) {
		same = a.mip_layout[level] == b.mip_layout[level];
	}
	if (!same) {
		EXIT("texture binding reuse disagrees with a fresh ResolveTexture: kept image %u/%u fresh %u/%u "
		     "address=0x%016" PRIx64 "\n",
		     kept.image_id.index, kept.image_id.generation, fresh.image_id.index,
		     fresh.image_id.generation, a.data.address);
	}
	const auto n = verified.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((n & ((1u << 20) - 1u)) == 0) {
		std::printf("TextureBindingReuse verify: %" PRIu64 " kept bindings agree with a fresh resolve, 0 mismatches\n", n);
		std::fflush(stdout);
	}
}

// Whether a slot's previous image binding stands for ResolveTexture(resource, value): the same
// resource and descriptor find the same image while the image set is unchanged (RefindImage checks
// the generation and repeats FindImage's bookkeeping), and the binding must still follow that
// image's stencil association.
static bool ReuseTexture(TextureCache& cache, const PreparedBindings::ImageSource& source,
                         const TextureBinding&                        binding,
                         const ShaderRecompiler::IR::ImageResource&   resource,
                         const ShaderRecompiler::IR::DescriptorValue& value) {
	if (source.generation == 0 || !(source.value == value) ||
	    (source.resource_address != &resource && !(source.resource == resource)) ||
	    !cache.RefindImage(source.found, source.generation, binding.desc,
	                       source.metadata_base_layer)) {
		return false;
	}
	const auto depth_id = cache.GetImage(source.found).depth_id;
	return (depth_id ? depth_id : source.found) == binding.image_id;
}

void RenderExecutor::PrepareBindings(const ShaderStageRuntime& runtime,
                                     PreparedBindings& prepared) {
	Common::GpuPhase::Scope gpu_phase_srt(Common::GpuPhase::Phase::Srt);
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!runtime);
	const auto& program  = *runtime.program;
	const auto& snapshot = *runtime.resources;
	prepared.runtime = &runtime;
	prepared.gds = {nullptr, 0, VK_WHOLE_SIZE};
	prepared.loop_guard = {nullptr, 0, VK_WHOLE_SIZE};
	prepared.flattened_srt = {};
	prepared.shader_data_buffer = {};
	prepared.shared_memory = {};
	prepared.images.resize(program.info.images.size());
	prepared.samplers.clear();
	prepared.shader_data.clear();
	prepared.image_sources.resize(program.info.images.size());
	const bool reuse_bindings = Config::TextureBindingReuseEnabled();
	std::optional<TextureCache::LockBatch> texture_batch;
	texture_batch.emplace(m_context.GetTextureCache());
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		// Consecutive draws mostly bind the same textures: keep the slot's binding while the
		// descriptor, the shader's image resource and the image set are unchanged.
		Common::GpuPhase::Scope gpu_phase_reuse(Common::GpuPhase::Phase::SrtReuse);
		if (reuse_bindings && ReuseTexture(m_context.GetTextureCache(), prepared.image_sources[i],
		                                   prepared.images[i], program.info.images[i],
		                                   snapshot.images[i])) {
			auto& kept = prepared.images[i];
			// As ResolveTexture returns it; RebindImages acquires the views.
			kept.image_view = nullptr;
			kept.layout     = vk::ImageLayout::eUndefined;
			kept.mip_views.clear();
			if (TextureReuseVerifyEnabled()) {
				VerifyKeptTexture(kept, ResolveTexture(program.info.images[i], snapshot.images[i]));
			}
			BindImage(kept.image_id, kept.desc.type == TextureCache::BindingType::Storage);
			continue;
		}
		gpu_phase_reuse.End();
		Common::GpuPhase::Scope gpu_phase_resolve(Common::GpuPhase::Phase::SrtResolve);
		auto binding = ResolveTexture(program.info.images[i], snapshot.images[i],
		                              &prepared.image_sources[i]);
		BindImage(binding.image_id, binding.desc.type == TextureCache::BindingType::Storage);
		binding.mip_views.swap(prepared.images[i].mip_views);
		binding.mip_views.clear();
		prepared.images[i] = std::move(binding);
	}
	texture_batch.reset();
	Common::GpuPhase::Scope gpu_phase_samplers(Common::GpuPhase::Phase::SrtSamplers);
	prepared.samplers.reserve(program.info.samplers.size());
	for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
		prepared.samplers.push_back(NativeSampler(
		    m_context, program, i, snapshot.samplers[program.info.samplers[i].snapshot_index]));
	}
	gpu_phase_samplers.End();
	prepared.shader_data.reserve(program.bindings.ShaderDataDwords());
	for (const auto reg: program.info.user_data_registers) {
		prepared.shader_data.push_back(snapshot.user_data[reg - program.user_data_base]);
	}
	prepared.shader_data.resize(program.bindings.ShaderDataDwords());
	if (program.bindings.descriptor_counts[static_cast<size_t>(BindingKind::Gds)] != 0) {
		prepared.gds.buffer = m_context.GetBufferCache().GetGdsBuffer()->Handle();
	}
	if (program.bindings.descriptor_counts[static_cast<size_t>(BindingKind::LoopGuard)] != 0) {
		prepared.loop_guard.buffer = m_context.GetBufferCache().GetLoopGuardBuffer()->Handle();
	}
}

void RenderExecutor::FindBuffers(std::span<PreparedBindings* const> stages) {
	Common::GpuPhase::Scope gpu_phase_srt(Common::GpuPhase::Phase::SrtBuffers);
	KYTY_PROFILER_FUNCTION();
	auto& cache = m_context.GetBufferCache();
	for (auto* stage: stages) {
		auto& prepared = *stage;
		EXIT_IF(prepared.runtime == nullptr || !*prepared.runtime);
		const auto& program  = *prepared.runtime->program;
		const auto& snapshot = *prepared.runtime->resources;
		const auto& layout = program.bindings;
		prepared.buffer_sources.resize(layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);
		if (layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)] == 0) {
			continue;
		}
		for (uint32_t resource = 0; resource < program.info.buffers.size(); ++resource) {
			const auto& metadata = program.info.buffers[resource];
			if (metadata.descriptor_index == UINT32_MAX) continue;
			auto& source = prepared.buffer_sources[metadata.descriptor_index];
			const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[resource]);
			const auto address = descriptor.Base48();
			auto size = descriptor.GetSize();
			if (address == 0 || size == 0) {
				source = {};
				continue;
			}
			if (descriptor.NumRecords() == UINT32_MAX) {
				if (program.info.buffers[resource].written) {
					// Sentinel write ranges end before tables captured by any bound stage. A
					// compute dispatch's own scalar reads may overlap its writes (see
					// CommitBindings), so a table at the start of its own range is allowed; later
					// tables still end the range so it never grows past the existing limits.
					for (const auto* reader: stages) {
						const bool own_compute_reads =
						    reader == stage && program.stage == ShaderType::Compute;
						for (const auto [read_address, read_size]: reader->runtime->resources->specialization_reads) {
							if (read_size == 0) continue;
							if (read_address > address) {
								size = std::min(size, read_address - address);
							} else if (address - read_address < read_size && !own_compute_reads) {
								EXIT("scalar resource reads overlap a shader buffer write\n");
							}
						}
					}
				} else {
					const auto& graphics = m_context.GetGraphics();
					const auto limit = uint64_t {graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange} -
					                   (graphics.StorageMinAlignment() - 1);
					size = std::min({size, uint64_t {256} * 1024 * 1024, limit});
				}
			}
			size = Libs::LibKernel::Memory::ClampRangeSize(address, size);
			if (size == 0) { // unmapped: bound as a null buffer
				source = {};
				continue;
			}
			source = {address, size, cache.FindBuffer(address, size)};
		}
	}
}

void RenderExecutor::RebindBuffers(PreparedBindings& prepared) {
	Common::GpuPhase::Scope gpu_phase_desc(Common::GpuPhase::Phase::DescBuffers);
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.runtime == nullptr || !*prepared.runtime);
	const auto& program   = *prepared.runtime->program;
	const auto& snapshot  = *prepared.runtime->resources;
	const auto& layout    = program.bindings;
	EXIT_IF(prepared.buffer_sources.size() != layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);

	prepared.buffers.resize(layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);
	EXIT_IF(prepared.shader_data.size() != layout.ShaderDataDwords());
	std::fill(prepared.shader_data.begin() + layout.memory_offset_dword,
	          prepared.shader_data.end(), 0);
	auto pack_memory_offset = [&](uint32_t index, uint32_t offset) {
		const auto dword = layout.memory_offset_dword + index / 4u;
		const auto shift = (index % 4u) * 8u;
		prepared.shader_data[dword] |= offset << shift;
	};
	for (const auto& resource: program.info.buffers) {
		const auto index = resource.descriptor_index;
		if (index == UINT32_MAX) continue;
		uint32_t buffer_offset = 0;
		prepared.buffers[index] = NativeStorageBuffer(m_context, prepared.buffer_sources[index],
		                                              resource, buffer_offset);
		pack_memory_offset(index, buffer_offset);
	}
	if (layout.descriptor_counts[static_cast<size_t>(BindingKind::FlattenedSrt)] != 0) {
		prepared.flattened_srt = NativeUpload(m_context, snapshot.flattened_srt);
	}
	if (layout.descriptor_counts[static_cast<size_t>(BindingKind::ShaderData)] != 0) {
		prepared.shader_data_buffer = NativeUpload(m_context, prepared.shader_data);
	}
}

void RenderExecutor::RebindImages(PreparedBindings& prepared) {
	Common::GpuPhase::Scope gpu_phase_desc(Common::GpuPhase::Phase::DescImages);
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.runtime == nullptr || !*prepared.runtime);
	const auto& program  = *prepared.runtime->program;
	const auto& snapshot = *prepared.runtime->resources;
	auto&       images   = prepared.images;
	EXIT_IF(images.size() != program.info.images.size());
	// Bindings assembled without PrepareBindings have no recorded sources.
	prepared.image_sources.resize(images.size());
	auto& texture_cache = m_context.GetTextureCache();
	const TextureCache::LockBatch texture_batch(texture_cache);
	// Acquire each view before a later overlapping descriptor can replace its image.
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		if (!images[i].image_id) continue;
		const auto old_image = texture_cache.m_slot_images.try_get(images[i].image_id);
		if (old_image == nullptr || !old_image->registered ||
		    old_image->binding.needs_rebind) {
			if (old_image != nullptr) {
				old_image->binding = {};
			}
			Common::GpuPhase::Scope gpu_phase_resolve(Common::GpuPhase::Phase::DescResolve);
			images[i] = ResolveTexture(program.info.images[i], snapshot.images[i],
			                           &prepared.image_sources[i]);
			BindImage(images[i].image_id,
			          images[i].desc.type == TextureCache::BindingType::Storage);
		} else {
			texture_cache.MaterializeViewClear(images[i].image_id, images[i].desc);
		}
		auto& binding = images[i];
		binding.mip_views.clear();
		const auto& resource = program.info.images[i];
		if (resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::Dynamic) {
			EXIT_IF(resource.mip_count == 0u ||
			        resource.mip_count != binding.desc.view_info.level_count);
			binding.mip_views.reserve(resource.mip_count);
			for (uint32_t mip = 0; mip < resource.mip_count; mip++) {
				auto desc = binding.desc;
				desc.view_info.base_level += mip;
				desc.view_info.level_count = 1;
				// The shader selects the mip after applying the guest minimum LOD.
				desc.view_info.min_lod = 0;
				Common::GpuPhase::Scope gpu_phase_find(Common::GpuPhase::Phase::DescFindTexture);
				binding.mip_views.push_back(texture_cache.FindTexture(binding.image_id, desc));
			}
			binding.image_view = binding.mip_views.front();
		} else {
			Common::GpuPhase::Scope gpu_phase_find(Common::GpuPhase::Phase::DescFindTexture);
			binding.image_view = texture_cache.FindTexture(binding.image_id, binding.desc);
		}
		auto&      image   = texture_cache.GetImage(binding.image_id);
		const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
		image.usage.storage |= storage;
		image.usage.texture |= !storage;
	}
}

void RenderExecutor::PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
                                             std::span<RenderColorInfo> colors) {
	bool uses_dma      = false;
	bool address_writes = false;
	FindBuffers(stages);
	for (auto* stage: stages) {
		uses_dma |= stage->runtime->program->info.uses_dma;
		address_writes |= stage->runtime->program->has_address_writes;
	}
	if (uses_dma) {
		m_context.PrepareBda(address_writes);
	}
	auto& cache = m_context.GetTextureCache();
	std::optional<TextureCache::LockBatch> texture_batch;
	texture_batch.emplace(cache);
	for (auto* stage: stages) {
		RebindImages(*stage);
	}
	for (auto& target: colors) {
		EXIT_IF(!target.image_id);
		const auto old_image = cache.m_slot_images.try_get(target.image_id);
		if (old_image == nullptr || !old_image->registered ||
		    old_image->binding.needs_rebind) {
			if (old_image != nullptr) {
				old_image->binding = {};
			}
			target.desc.view_info.base_level = target.guest_mip_level;
			target.desc.view_info.base_layer = target.guest_array_layer;
			target.image_id = cache.FindImage(target.desc);
			BindRenderTarget(target.image_id);
		}
	}
	texture_batch.reset();
	// Discovery can read back PS5 metadata and submit the scheduler. Reserve draw buffers only
	// after image identities are final; attachment layout transitions follow buffer alias copies.
	for (auto* stage: stages) {
		RebindBuffers(*stage);
	}
}

// The words a pushed write is compared by (--push-descriptor-delta): the type and count, then the handles and ranges of each
// descriptor. False for a write that carries neither buffer nor image infos.
static bool PushWriteContent(const vk::WriteDescriptorSet& write, std::vector<uint64_t>& out) {
	out.clear();
	out.push_back((static_cast<uint64_t>(write.descriptorType) << 32u) | write.descriptorCount);
	if (write.pBufferInfo != nullptr) {
		for (uint32_t i = 0; i < write.descriptorCount; i++) {
			const auto& info = write.pBufferInfo[i];
			out.push_back(std::bit_cast<uint64_t>(info.buffer));
			out.push_back(info.offset);
			out.push_back(info.range);
		}
		return true;
	}
	if (write.pImageInfo != nullptr) {
		for (uint32_t i = 0; i < write.descriptorCount; i++) {
			const auto& info = write.pImageInfo[i];
			out.push_back(std::bit_cast<uint64_t>(info.sampler));
			out.push_back(std::bit_cast<uint64_t>(info.imageView));
			out.push_back(static_cast<uint64_t>(info.imageLayout));
		}
		return true;
	}
	return false;
}

void RenderExecutor::CommitBindings(CommandBuffer&                     buffer,
                                    vk::PipelineBindPoint              pipeline_bind_point,
                                    const PipelineCache::Pipeline&     pipeline,
                                    std::span<PreparedBindings* const> prepared_bindings) {
	KYTY_PROFILER_FUNCTION();
	Common::GpuPhase::Scope gpu_phase_desc(Common::GpuPhase::Phase::DescCommit);
	auto   vk_buffer        = buffer.Handle();
	size_t image_descriptor_count = 0;
	size_t write_count      = 0;
	ShaderRecompiler::IR::PushData push_data;
	bool                           has_push_data = false;
	constexpr auto                 GraphicsStages =
	    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eMeshEXT |
	    vk::ShaderStageFlagBits::eTessellationControl |
	    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eFragment;
	vk::ShaderStageFlags push_stages = pipeline_bind_point == vk::PipelineBindPoint::eGraphics
	                                       ? vk::ShaderStageFlagBits::eFragment
	                                       : vk::ShaderStageFlags {};
	for (const auto* prepared: prepared_bindings) {
		EXIT_IF(prepared == nullptr || prepared->runtime == nullptr || !*prepared->runtime);
		const auto& program = *prepared->runtime->program;
		write_count += std::popcount(program.bindings.descriptor_mask);
		for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
			const auto index = std::countr_zero(mask);
			const auto count = program.bindings.descriptor_counts[index];
			if (index >= ShaderRecompiler::IR::FirstImageBinding &&
			    index <= static_cast<size_t>(BindingKind::Samplers)) {
				image_descriptor_count += count;
			}
		}
		const auto shader_stage = NativeShaderStage(program.stage);
		push_stages |= shader_stage;
		EXIT_IF((pipeline_bind_point == vk::PipelineBindPoint::eGraphics &&
		         (shader_stage & GraphicsStages) == vk::ShaderStageFlags {}) ||
		        (pipeline_bind_point == vk::PipelineBindPoint::eCompute &&
		         shader_stage != vk::ShaderStageFlagBits::eCompute));
	}
	if (SkipDeps::Poisoned()) {
		// --debug-skip-log: what this pass reads of the memory a skipped draw left stale, and what it
		// writes on the strength of it (the writes become poison too).
		const bool compute = pipeline_bind_point == vk::PipelineBindPoint::eCompute;
		const char* op     = compute ? "dispatch" : "draw";
		auto        hits   = std::move(SkipDeps::PendingHits()); // geometry reads of this draw
		SkipDeps::PendingHits().clear();
		std::vector<SkipDeps::Range> writes;
		auto&                        textures = m_context.GetTextureCache();
		const auto note_range = [&](const GuestRange& range, bool write, SkipDeps::ReadKind kind,
		                            uint64_t hash) {
			if (range.Empty()) return;
			if (write) {
				writes.push_back({range.address, range.size});
			} else {
				SkipDeps::NoteRead({range.address, range.size}, kind, hash, op, hits);
			}
		};
		for (const auto* prepared: prepared_bindings) {
			const auto& program = *prepared->runtime->program;
			const auto  hash    = program.shader_hash;
			const auto  hits_before = hits.size();
			for (const auto& resource: program.info.buffers) {
				if (resource.descriptor_index == UINT32_MAX ||
				    resource.descriptor_index >= prepared->buffer_sources.size()) continue;
				const auto& source = prepared->buffer_sources[resource.descriptor_index];
				if (source.size == 0) continue;
				const GuestRange range {source.address, source.size};
				if (resource.read || !resource.written) {
					note_range(range, false, SkipDeps::ReadKind::BufferRead, hash);
				}
				if (resource.written || resource.atomic) note_range(range, true, {}, hash);
			}
			for (uint32_t i = 0; i < program.info.images.size() && i < prepared->images.size(); i++) {
				const auto& resource = program.info.images[i];
				const auto* image = textures.m_slot_images.try_get(prepared->images[i].image_id);
				if (image == nullptr) continue;
				const bool reads = resource.read || !resource.written;
				for (const auto& range: {image->info.data, image->info.stencil}) {
					if (reads) note_range(range, false, SkipDeps::ReadKind::SampledImage, hash);
					if (resource.written || resource.atomic) note_range(range, true, {}, hash);
				}
			}
			const char* blind = program.has_address_writes ? "address_writes"
			                    : ((program.bindings.descriptor_mask >>
			                        static_cast<uint32_t>(BindingKind::BdaPagetable)) & 1u)
			                        ? "bda_reads"
			                        : nullptr;
			if (blind != nullptr) {
				const bool reads_poison = hits.size() > hits_before;
				SkipDeps::NoteUntracked(hash, op, blind, reads_poison);
				// --skip-guard: what this shader dereferences through 64-bit addresses is invisible, and
				// it reads a skipped draw's output through its bound resources: be conservative.
				if (reads_poison) {
					SkipDeps::NoteBdaSuspect(hash, op, blind,
					                         std::span<const SkipDeps::Hit>(hits).subspan(hits_before));
				}
			}
		}
		for (const auto id: m_bound_images) {
			const auto* image = textures.m_slot_images.try_get(id);
			if (image == nullptr || (!image->binding.shader_write && !image->binding.is_target)) continue;
			for (const auto& range: {image->info.data, image->info.stencil, image->info.metadata.range}) {
				note_range(range, true, {}, 0);
			}
		}
		if (!hits.empty()) SkipDeps::Global().NoteTaint(hits, writes);
	}
	Common::GpuPhase::Scope gpu_phase_hazard(Common::GpuPhase::Phase::DescHazard);
	{
		// Scalar reads versus writes of the same draw (see scalarReadHazard.h): the written image and
		// buffer ranges are collected once per commit and each read is answered by a binary search.
		static thread_local WrittenRangeIndex image_writes;
		static thread_local WrittenRangeIndex buffer_writes;
		static thread_local WrittenRangeIndex buffer_writes_of_others;
		bool collected = false;
		auto collect_buffer_writes = [&](WrittenRangeIndex& index, const PreparedBindings* skip) {
			index.Clear();
			for (const auto* writer: prepared_bindings) {
				if (writer == skip) continue;
				const auto& program = *writer->runtime->program;
				for (const auto& resource: program.info.buffers) {
					if (!resource.written || resource.descriptor_index == UINT32_MAX) continue;
					const auto& written = writer->buffer_sources[resource.descriptor_index];
					index.Add(written.address, written.size);
				}
			}
			index.Build();
		};
		for (const auto* reader: prepared_bindings) {
			const auto& reads = reader->runtime->resources->specialization_reads;
			if (reads.empty()) continue;
			for (const auto* writer: prepared_bindings) {
				if (writer->runtime->program->has_address_writes) {
					EXIT("scalar resource reads cannot be proven disjoint from shader address writes\n");
				}
			}
			if (!collected) {
				collected = true;
				image_writes.Clear();
				for (const auto id: m_bound_images) {
					const auto* image = m_context.GetTextureCache().m_slot_images.try_get(id);
					if (image == nullptr ||
					    (!image->binding.shader_write && !image->binding.is_target)) continue;
					for (const auto written: {image->info.data, image->info.stencil,
					                          image->info.metadata.range}) {
						image_writes.Add(written.address, written.size);
					}
				}
				image_writes.Build();
				collect_buffer_writes(buffer_writes, nullptr);
			}
			// A compute dispatch reading a buffer it writes itself through scalar loads is racy
			// on hardware: the scalar cache is not coherent with the dispatch's own vector
			// writes, so the value captured at dispatch start is a valid result. The next
			// dispatch re-materializes its specialization from memory, so the writes are still
			// observed from then on.
			const WrittenRangeIndex* buffers = &buffer_writes;
			if (reader->runtime->program->stage == ShaderType::Compute) {
				collect_buffer_writes(buffer_writes_of_others, reader);
				buffers = &buffer_writes_of_others;
			}
			// Resolved draw: the plan's read hull proves the common case (no written range touches
			// a read) without the per-read search; any doubt runs the full check below.
			if (Config::ResolvedDrawEnabled()) {
				const auto& hull  = reader->runtime->resources->read_hull;
				auto&       stats = ResolvedDraw::GetStats().hazard_hull;
				if (!hull.valid) {
					stats.Absent();
				} else if (HullProvesNoHazard(hull, image_writes, *buffers)) {
					stats.Used();
					if (Config::ResolvedDrawVerify()) {
						if (FindScalarReadHazard(reads, image_writes, *buffers).kind !=
						    ScalarReadHazardKind::None) {
							EXIT("resolved draw: the read hull proved no hazard but the full check found one\n");
						}
						stats.Verified();
					}
					continue;
				} else {
					stats.Fallback();
				}
			}
			switch (FindScalarReadHazard(reads, image_writes, *buffers).kind) {
				case ScalarReadHazardKind::None: break;
				case ScalarReadHazardKind::ImageWrite:
					EXIT("scalar resource reads overlap an image or attachment write\n");
					break;
				case ScalarReadHazardKind::BufferWrite:
					EXIT("scalar resource reads overlap a shader buffer write\n");
					break;
				case ScalarReadHazardKind::InvalidRange:
					EXIT("invalid image overlap range\n");
					break;
			}
		}
	}
	gpu_phase_hazard.End();
	m_descriptor_images.resize(image_descriptor_count);
	m_descriptor_writes.clear();
	m_descriptor_writes.reserve(write_count);
	size_t image_cursor = 0;
	std::array<vk::DescriptorBufferInfo, 2> address_buffers;

	Common::GpuPhase::Scope gpu_phase_writes(Common::GpuPhase::Phase::DescWrites);
	for (auto* prepared: prepared_bindings) {
		const auto& program       = *prepared->runtime->program;
		auto&       descriptors   = *prepared;
		const auto  shader_stage  = NativeShaderStage(program.stage);
		const auto  shader_stages = ShaderPipelineStages(shader_stage);
		if (descriptors.gds.buffer != nullptr) {
			buffer.EndRendering();
			const auto barrier = MakeGdsDependency(descriptors.gds.buffer);
			Common::GpuPhase::Scope gpu_phase_vk(Common::GpuPhase::Phase::VkCmd);
			vk_buffer.pipelineBarrier(
			    vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eTransfer |
			        vk::PipelineStageFlagBits::eAllGraphics |
			        vk::PipelineStageFlagBits::eComputeShader,
			    shader_stages, vk::DependencyFlags {}, 0, nullptr, 1, &barrier, 0, nullptr);
		}

		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			auto& binding = descriptors.images[i];
			if (!binding.image_id) continue;
			auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
			const auto&                 view = binding.desc.view_info;
			const ImageSubresourceRange range {view.base_level, view.level_count, view.base_layer,
			                                   view.layer_count};
			const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
			// AcquireRenderTargets gives every attachment of this draw a layout before this runs. An
			// image flagged as a target without one is not written through the framebuffer here, so
			// it is bound like any other sampled image (reported) instead of aborting.
			const bool target_without_attachment =
			    image.binding.is_target &&
			    image.binding.attachment_layout == vk::ImageLayout::eUndefined;
			if (target_without_attachment) {
				const auto count = g_target_fallback_count.fetch_add(1, std::memory_order_relaxed) + 1;
				if (count <= kTargetFallbackLogLimit) {
					VRAM_LOG("Texture: image flagged as render target is not an attachment of this "
					         "draw, bound as a sampled image: id=%u gen=%u format=%d depth=%d "
					         "storage=%d is_bound=%d shader_write=%d force_general=%d "
					         "needs_rebind=%d data=0x%llx (%u so far)\n",
					         binding.image_id.index, binding.image_id.generation,
					         static_cast<int>(image.backing.format), image.info.IsDepth() ? 1 : 0,
					         storage ? 1 : 0, image.binding.is_bound ? 1 : 0,
					         image.binding.shader_write ? 1 : 0, image.binding.force_general ? 1 : 0,
					         image.binding.needs_rebind ? 1 : 0,
					         static_cast<unsigned long long>(image.info.data.address), count);
				}
			}
			Common::GpuPhase::Scope gpu_phase_transit(Common::GpuPhase::Phase::DescTransit);
			if (image.binding.is_target && !target_without_attachment) {
				const auto layout = image.binding.attachment_layout;
				if (image.info.IsDepth()) {
					const auto host_view =
					    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
					EXIT_IF(storage || host_view == image.views.end());
					const auto aspect = host_view->info.aspect;
					if (aspect & ~DepthReadableAspects(layout)) {
						EXIT("sampling a writable depth/stencil attachment aspect\n");
					}
				}
				image.Transit(layout,
				              image.binding.attachment_access | vk::AccessFlagBits2::eShaderRead |
				                  (image.binding.shader_write ? vk::AccessFlagBits2::eShaderWrite
				                                              : vk::AccessFlags2 {}),
				              {}, vk_buffer);
			} else if (image.binding.force_general && !image.info.IsDepth()) {
				const vk::AccessFlags2 storage_access = image.binding.shader_write
				                                            ? vk::AccessFlagBits2::eShaderWrite
				                                            : vk::AccessFlags2 {};
				image.Transit(vk::ImageLayout::eGeneral,
				              vk::AccessFlagBits2::eShaderRead | storage_access, {}, vk_buffer);
			} else if (storage) {
				image.Transit(vk::ImageLayout::eGeneral,
				              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
				              range, vk_buffer);
			} else {
				image.Transit(image.info.IsDepth() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
				                                   : vk::ImageLayout::eShaderReadOnlyOptimal,
				              vk::AccessFlagBits2::eShaderRead, range, vk_buffer);
			}
			binding.layout = image.backing.state.layout;
			if (!storage && program.info.images[i].depth_compare) {
				ReportComparedView(m_context.GetGraphics(), binding, image);
			}
			gpu_phase_transit.End();
		}

		std::array<size_t, ShaderRecompiler::IR::ImageBindingCount> image_starts {};
		for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
			const auto index = std::countr_zero(mask);
			const auto kind = static_cast<BindingKind>(index);
			vk::WriteDescriptorSet write {};
			write.dstBinding = ShaderRecompiler::IR::NativeBinding(program.stage, kind);
			write.descriptorType = NativeDescriptorType(kind);
			write.descriptorCount = program.bindings.descriptor_counts[index];
			const auto image_start = image_cursor;
			if (ShaderRecompiler::IR::ImageBindingResourceClass(kind) !=
			    ShaderRecompiler::IR::ImageResourceClass::None) {
				image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] = image_start;
				image_cursor += write.descriptorCount;
			} else {
				switch (kind) {
					case BindingKind::Buffers:
						EXIT_IF(descriptors.buffers.size() != write.descriptorCount);
						for (const auto& view: descriptors.buffers) {
							EXIT_IF(view.buffer == nullptr);
						}
						write.pBufferInfo = descriptors.buffers.data();
						break;
					case BindingKind::BdaPagetable:
					case BindingKind::FaultBuffer: {
						auto& cache = m_context.GetBufferCache();
						const auto* bda_buffer = kind == BindingKind::BdaPagetable
						                             ? cache.GetBdaPageTableBuffer()
						                             : cache.GetFaultBuffer();
						auto& view = address_buffers[kind == BindingKind::FaultBuffer];
						view = {bda_buffer->Handle(), 0, bda_buffer->Size()};
						write.pBufferInfo = &view;
						break;
					}
					case BindingKind::FlattenedSrt:
					case BindingKind::ShaderData:
					case BindingKind::SharedMemory:
					case BindingKind::LoopGuard:
					case BindingKind::Gds: {
						const vk::DescriptorBufferInfo* view = &descriptors.gds;
						if (kind == BindingKind::LoopGuard) {
							view = &descriptors.loop_guard;
						} else if (kind == BindingKind::FlattenedSrt) {
							view = &descriptors.flattened_srt;
						} else if (kind == BindingKind::ShaderData) {
							view = &descriptors.shader_data_buffer;
						} else if (kind == BindingKind::SharedMemory) {
							view = &descriptors.shared_memory;
						}
						EXIT_IF(view->buffer == nullptr);
						write.pBufferInfo = view;
						break;
					}
					case BindingKind::Samplers:
						for (const auto sampler: descriptors.samplers) {
							EXIT_IF(sampler == nullptr);
							m_descriptor_images[image_cursor++] = {sampler, nullptr, vk::ImageLayout::eUndefined};
						}
						break;
					case BindingKind::Count: EXIT("invalid descriptor binding kind");
				}
			}
			if (image_cursor != image_start) {
				write.pImageInfo = m_descriptor_images.data() + image_start;
			}
			m_descriptor_writes.push_back(write);
		}
		for (uint32_t i = 0; i < program.info.images.size(); ++i) {
			const auto& resource = program.info.images[i];
			const auto& texture = descriptors.images[i];
			EXIT_IF(texture.image_id &&
			        resource.mip_count != (texture.mip_views.empty() ? 1u : texture.mip_views.size()));
			const auto kind = *ShaderRecompiler::IR::DescriptorBindingForImage(resource);
			const auto start = image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] +
			                   resource.descriptor_index;
			for (uint32_t mip = 0; mip < resource.mip_count; ++mip) {
				m_descriptor_images[start + mip] = MakeImageInfo(texture, mip);
			}
		}

		const auto shader_data_dwords = program.bindings.ShaderDataDwords();
		EXIT_IF(prepared->shader_data.size() != shader_data_dwords);
		if (program.bindings.UsesPushData()) {
			std::ranges::copy(prepared->shader_data,
			                  push_data.dwords.begin() + program.bindings.push_data_start_dword);
			has_push_data = true;
		}
	}

	gpu_phase_writes.End();
	if (has_push_data) {
		Common::GpuPhase::Scope gpu_phase_vk(Common::GpuPhase::Phase::DescPush);
		vk_buffer.pushConstants(pipeline.pipeline_layout, push_stages, 0, sizeof(push_data),
		                        push_data.dwords.data());
	}

	const bool graphics_bind = pipeline_bind_point == vk::PipelineBindPoint::eGraphics;
	auto&      push_cache    = m_context.GetCommandScheduler().GetPushDescriptorCache();
	if (m_descriptor_writes.empty()) {
		// Binding this pipeline may disturb the pushed set (its layout is not known to match).
		if (graphics_bind) push_cache.Invalidate();
	} else {
		Common::GpuPhase::Scope gpu_phase_vk(Common::GpuPhase::Phase::DescPush);
		EXIT_IF(pipeline.descriptor_set_layout == nullptr);
		if (pipeline.uses_push_descriptors) {
			const vk::WriteDescriptorSet* writes = m_descriptor_writes.data();
			uint32_t                      count  = static_cast<uint32_t>(m_descriptor_writes.size());
			if (graphics_bind && Config::PushDescriptorDeltaEnabled()) {
				// The pushed set keeps the bindings a push does not write (until it is disturbed): leave out
				// those whose content is what it holds.
				push_cache.Begin(std::bit_cast<uint64_t>(pipeline.pipeline_layout));
				m_push_writes.clear();
				for (const auto& write: m_descriptor_writes) {
					if (!PushWriteContent(write, m_push_content)) {
						push_cache.Forget(write.dstBinding);
						m_push_writes.push_back(write);
					} else if (push_cache.Changed(write.dstBinding, m_push_content)) {
						m_push_writes.push_back(write);
					}
				}
				writes = m_push_writes.data();
				count  = static_cast<uint32_t>(m_push_writes.size());
				if (count == 0) push_cache.NoteElided();
				static const bool stats = std::getenv("KYTY_DRAW_STATE_STATS") != nullptr;
				if (stats && (push_cache.GetStats().pushes & 0x3ffff) == 0) {
					const auto& s = push_cache.GetStats();
					std::printf("[push-descriptor-delta] pushes=%lu elided=%.1f%% writes_pushed=%.1f%% of %lu disturbed=%lu\n",
					            (unsigned long)s.pushes, 100.0 * s.pushes_elided / s.pushes,
					            100.0 * s.writes_pushed / (s.writes_seen ? s.writes_seen : 1),
					            (unsigned long)s.writes_seen, (unsigned long)s.disturbed);
				}
			} else if (graphics_bind) {
				push_cache.Invalidate();
			}
			if (count != 0) {
				vk_buffer.pushDescriptorSetKHR(pipeline_bind_point, pipeline.pipeline_layout, 0, count, writes);
			}
		} else {
			if (graphics_bind) push_cache.Invalidate(); // a bound set replaces the pushed one
			const auto set = m_context.GetDescriptorHeap().Commit(pipeline.descriptor_set_layout);
			for (auto& write: m_descriptor_writes) {
				write.dstSet = set;
			}
			m_context.GetGraphics().device.updateDescriptorSets(
			    static_cast<uint32_t>(m_descriptor_writes.size()), m_descriptor_writes.data(), 0,
			    nullptr);
			vk_buffer.bindDescriptorSets(pipeline_bind_point, pipeline.pipeline_layout, 0, 1, &set,
			                             0, nullptr);
		}
	}
}

} // namespace Libs::Graphics
