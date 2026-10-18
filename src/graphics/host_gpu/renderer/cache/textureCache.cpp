#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/verifyRation.h"
#include "graphics/host_gpu/vramPolicy.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "common/gpuPhaseStats.h"
#include "graphics/host_gpu/renderer/cache/gcClock.h"
#include "graphics/host_gpu/renderer/cache/textureGcPolicy.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <xxhash.h>
#include <mutex>
#include <span>
#include <tuple>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

constexpr uint64_t NumFramesBeforeRemoval = 32;

// KYTY_IMAGE_LOOKUP_VERIFY=1: every hit of the --image-lookup-memo runs the full lookup too and
// aborts on a disagreement; a line with the counts goes to stdout every 2^20 verified hits.
[[nodiscard]] bool ImageLookupVerifyEnabled() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_IMAGE_LOOKUP_VERIFY");
		return text != nullptr && text[0] != '\0' && text[0] != '0';
	}();
	return enabled;
}

void ImageLookupNoteVerified(const ImageLookupMemo::Stats& stats) {
	static std::atomic<uint64_t> verified {0};
	const auto                   n = verified.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((n & ((1u << 20) - 1u)) == 0) {
		std::printf("ImageLookupMemo verify: %" PRIu64 " hits agree with the full lookup, 0 mismatches "
		            "(memo hits %" PRIu64 " misses %" PRIu64 " records %" PRIu64 ")\n",
		            n, stats.hits, stats.misses, stats.records);
		std::fflush(stdout);
	}
}

[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                  vk::ClearColorValue& clear) {
	const auto& metadata = desc.info.metadata;
	const auto  format   = desc.info.IsDepth()
	                           ? TextureGetSurfaceFormatInfo(desc.info.guest_format).vk_format
	                           : desc.view_info.format;
	const bool  cmask    = metadata.kind == ImageMetadataKind::Cmask;
	if (cmask ? code == 0 : code == 0x20) {
		// Register clears belong to the color buffer; the texture pipe cannot decode them.
		return desc.type == TextureCache::BindingType::RenderTarget &&
		       metadata.clear_register_valid &&
		       DecodeRegisterColorClear(format, metadata.clear_word, metadata.clear_word_hi, clear);
	}
	if (cmask) {
		return false;
	}
	switch (code) {
		case 0x00:
		case 0x40:
		case 0x80:
		case 0xc0: break;
		default: return false;
	}
	clear = {};
	if (code == 0x00) {
		return true;
	}
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32: break;
		default: return false;
	}
	const float          rgb   = (code & 0x80u) != 0 ? 1.0f : 0.0f;
	const float          alpha = (code & 0x40u) != 0 ? 1.0f : 0.0f;
	std::array<float, 4> channels {rgb, rgb, rgb, alpha};
	if (!metadata.dcc_alpha_msb) {
		std::swap(channels[0], channels[3]);
	}
	// DCC clear decoding clamps missing lanes before applying the format swizzle.
	const auto components = vk::componentCount(format);
	if (components == 1) {
		channels[0] = channels[3];
	} else if (components == 2) {
		channels[1] = channels[3];
	}
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR5G6B5UnormPack16: std::swap(channels[0], channels[2]); break;
		case vk::Format::eR4G4B4A4UnormPack16:
			std::reverse(channels.begin(), channels.end());
			break;
		default: break;
	}
	clear.float32 = channels;
	return true;
}

} // namespace

TextureCache::LockBatch::LockBatch(TextureCache& cache): m_cache(Config::TextureLockBatchEnabled() ? &cache : nullptr) {
	if (m_cache != nullptr) m_cache->m_lock.lock();
}

TextureCache::LockBatch::~LockBatch() {
	if (m_cache != nullptr) m_cache->m_lock.unlock();
}

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager),
      m_blit_helper(graphics, scheduler),
      m_tiler(graphics, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)),
      m_image_copy_kernels(graphics, scheduler),
      m_buffer_cache(buffer_cache),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled()) {
	m_graphics.image_copy_kernels = &m_image_copy_kernels;
	if (m_graphics.CanReportMemoryUsage()) {
		const auto budget =
		    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
		const auto thresholds = VramPolicy::ComputeGcThresholds(budget);
		m_pressure_gc_memory  = thresholds.pressure;
		m_critical_gc_memory  = thresholds.critical;
		m_trigger_gc_memory   = thresholds.trigger;
	}
	constexpr uint64_t MiB = 1024ull * 1024;
	if (Config::GcTextureTriggerMb() != 0) {
		m_trigger_gc_memory = Config::GcTextureTriggerMb() * MiB;
	}
	if (Config::GcTexturePressureMb() != 0) {
		m_pressure_gc_memory = Config::GcTexturePressureMb() * MiB;
	}
	if (Config::GcTextureCriticalMb() != 0) {
		m_critical_gc_memory = Config::GcTextureCriticalMb() * MiB;
	}
	if (m_graphics.CanReportMemoryUsage()) {
		VRAM_LOG("Texture GC thresholds: trigger=%" PRIu64 " MiB pressure=%" PRIu64
		     " MiB critical=%" PRIu64 " MiB\n",
		     m_trigger_gc_memory / MiB, m_pressure_gc_memory / MiB, m_critical_gc_memory / MiB);
	}
	m_graphics.reclaim_memory = [this] { ReclaimMemory(); };
}

TextureCache::~TextureCache() {
	m_graphics.reclaim_memory = nullptr;
	m_graphics.image_copy_kernels = nullptr;
	m_slot_images.ForEach([&](ImageId id, const Image& image) {
		if (image.registered) {
			UnregisterImage(id);
		}
	});
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	if (cached.data.address != requested.data.address) {
		return false;
	}
	if (cached.data.size != requested.data.size) {
		return false;
	}
	if (cached.extent != requested.extent) {
		return false;
	}
	if (cached.resources.levels < requested.resources.levels ||
	    cached.resources.layers < requested.resources.layers) {
		return false;
	}
	if (cached.samples != requested.samples) {
		return false;
	}
	if (cached.bytes_per_block != requested.bytes_per_block) {
		return false;
	}
	if (cached.tile_mode != requested.tile_mode) {
		return false;
	}
	if (cached.GetColorTransform() != requested.GetColorTransform()) {
		return false;
	}
	if (!ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format) ||
	    cached.type != requested.type) {
		return false;
	}
	if (exact_format && cached.pixel_format != requested.pixel_format) {
		return false;
	}
	return true;
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		if (image.info.tile_mode == Prospero::TileMode::kDepth ||
		    image.info.tile_mode == Prospero::TileMode::kLinear) {
			return BindingType::DepthTarget;
		}
		return BindingType::Texture;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
	const auto id = m_slot_images.insert(m_graphics, m_scheduler, info);
	RegisterImage(id);
	return id;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: image registration is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		m_image_page_table[page].push_back(id);
	});
	// Before the image can be tracked (write-protected): a fault on one of its pages finds the count.
	m_image_presence.Add(image.info.data.address, image.info.data.size);
	image.registered = true;
	++m_image_set_generation;
	image.lru_id     = m_lru_cache.Insert(id, m_gc_tick);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	UntrackImage(id);
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: registered image is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr || !owners->Erase(id)) {
			EXIT("TextureCache: image missing from page owner index\n");
		}
	});
	m_image_presence.Remove(image.info.data.address, image.info.data.size);
	m_lru_cache.Free(image.lru_id);
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
	++m_image_set_generation;
}

void TextureCache::DeleteImage(ImageId id) {
	auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) {
		return;
	}
	if (!image->depth_id) {
		std::vector<ImageId> associations;
		m_slot_images.ForEach([&](ImageId candidate, const Image& associated) {
			if (associated.depth_id == id) {
				associations.push_back(candidate);
			}
		});
		for (const auto association: associations) {
			FreeImage(association);
		}
	}
	if (image->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (image->info.HasMetadata()) {
		const auto metadata = m_surface_metas.find(image->info.metadata.range.address);
		if (metadata != m_surface_metas.end() &&
		    image->info.metadata.kind == ImageMetadataKind::Htile &&
		    metadata->second.type == MetaDataInfo::Type::HTile) {
			// A later binding may have reused this address for another metadata type.
			m_surface_metas.erase(metadata);
		}
	}
	UnregisterImage(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_images.erase(id); });
	} else {
		m_slot_images.erase(id);
	}
}

void TextureCache::FreeImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.IsGpuModified()) {
		image.ClearGpuModified();
	}
	DeleteImage(id);
}

void TextureCache::TouchImage(Image& image) {
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::MarkAsMaybeDirty(ImageId id, Image& image) {
	image.MarkMaybeCpuDirty();
	if (image.NeedsMaybeCpuHash()) {
		image.SetMaybeCpuHash(image.HashGuestEdges());
	}
	UntrackImage(id);
}

void TextureCache::TrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	const auto image_end   = image.info.data.End();
	if (image_begin == image.track_addr && image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked()) {
		image.track_addr     = image_begin;
		image.track_addr_end = image_end;
		m_page_manager.UpdatePageWatchers<true>(image_begin, image.info.data.size);
		return;
	}
	if (image_begin < image.track_addr) {
		TrackImageHead(id);
	}
	if (image.track_addr_end < image_end) {
		TrackImageTail(id);
	}
}

void TextureCache::TrackImageHead(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	if (image_begin == image.track_addr) {
		return;
	}
	if (!image.IsTracked() || image_begin > image.track_addr) {
		EXIT("TextureCache: invalid image head tracking range\n");
	}
	const auto size  = image.track_addr - image_begin;
	image.track_addr = image_begin;
	m_page_manager.UpdatePageWatchers<true>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_end = image.info.data.End();
	if (image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked() || image.track_addr_end > image_end) {
		EXIT("TextureCache: invalid image tail tracking range\n");
	}
	const auto address   = image.track_addr_end;
	const auto size      = image_end - address;
	image.track_addr_end = image_end;
	m_page_manager.UpdatePageWatchers<true>(address, size);
}

void TextureCache::UntrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.IsTracked()) {
		return;
	}
	const auto address   = image.track_addr;
	const auto size      = image.track_addr_end - image.track_addr;
	image.track_addr     = 0;
	image.track_addr_end = 0;
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::UntrackImageHead(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto begin = image.info.data.address;
	if (!image.IsTracked() || begin < image.track_addr) {
		return;
	}
	const auto address = Common::AlignDown(begin + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	const auto size    = address - begin;
	image.track_addr   = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(begin, size);
	}
}

void TextureCache::UntrackImageTail(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto end   = image.info.data.End();
	if (!image.IsTracked() || image.track_addr_end < end) {
		return;
	}
	const auto address   = Common::AlignDown(end, TRACKER_PAGE_SIZE);
	const auto size      = end - address;
	image.track_addr_end = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::TrackImageDownload(ImageId id, Image& image) {
	if (m_readback_linear_images && !image.info.IsTiled()) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

TextureCache::ImageIds TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                        bool page_overlap) const {
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
		return {};
	}

	uint32_t query_epoch = ++m_image_query_epoch;
	if (query_epoch == 0) {
		m_slot_images.ForEach([](ImageId, const Image& image) { image.query_epoch = 0; });
		query_epoch = ++m_image_query_epoch;
	}

	ImageIds result;
	ForEachPage(address, size, [&](uint64_t page) {
		const auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr) {
			return;
		}
		owners->ForEach([&](ImageId id) {
			auto* image = m_slot_images.try_get(id);
			if (image == nullptr) {
				return;
			}
			if (image->query_epoch == query_epoch) {
				return;
			}
			image->query_epoch = query_epoch;
			if (image->Overlaps(address, size, page_overlap)) {
				result.push_back(id);
			}
		});
	});
	return result;
}

void TextureCache::ValidateImageDesc(const ImageDesc& desc) const {
	ImageOps::Validate(desc.info);
	if (desc.view_info.format == vk::Format::eUndefined || desc.view_info.level_count == 0 ||
	    desc.view_info.layer_count == 0 ||
	    desc.view_info.base_level >= desc.info.resources.levels ||
	    desc.view_info.level_count > desc.info.resources.levels - desc.view_info.base_level ||
	    (!desc.info.IsVolume() &&
	     (desc.view_info.base_layer >= desc.info.resources.layers ||
	      desc.view_info.layer_count > desc.info.resources.layers - desc.view_info.base_layer))) {
		EXIT("TextureCache: invalid image view description\n");
	}
	if (desc.type == BindingType::DepthTarget && !IsSupportedDepthTargetFormat(desc.info)) {
		EXIT("TextureCache: unsupported depth image description\n");
	}
	if (desc.type == BindingType::VideoOut && !IsSupportedVideoOutFormat(desc.info)) {
		EXIT("TextureCache: unsupported video-out image description\n");
	}
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression == VideoOutCompression::Unsupported) {
		EXIT("TextureCache: unsupported compressed video-out description\n");
	}
}

void TextureCache::PrepareImageCopy(Image& image) {
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = m_slot_images[id];
	RefreshImage(id);
	if (image.IsDefinitelyCpuDirty()) {
		EXIT("TextureCache: image copy source remained CPU-dirty after refresh\n");
	}
}

bool TextureCache::CopyD16(Image& destination, Image& source) {
	const bool source_depth      = source.info.IsDepth();
	const bool destination_depth = destination.info.IsDepth();
	if (source_depth == destination_depth) {
		return false;
	}
	auto&      depth          = source_depth ? source : destination;
	auto&      color          = source_depth ? destination : source;
	const auto transfer_bytes = DepthAspectTransferBytes(depth.backing.format);
	if (depth.info.bytes_per_block != sizeof(uint16_t) ||
	    color.info.bytes_per_block != sizeof(uint16_t) || transfer_bytes != sizeof(uint32_t)) {
		return false;
	}
	EXIT_IF(source.backing.samples != 1 || destination.backing.samples != 1 ||
	        source.info.resources.levels != 1 || destination.info.resources.levels != 1 ||
	        source.info.extent != destination.info.extent ||
	        source.info.resources.layers != destination.info.resources.layers);

	const auto     layers = depth.info.resources.layers;
	const uint64_t depth_slice =
	    static_cast<uint64_t>(depth.info.pitch) * depth.info.extent.height * transfer_bytes;
	const uint64_t color_slice =
	    static_cast<uint64_t>(color.info.pitch) * color.info.extent.height * sizeof(uint16_t);
	EXIT_IF(layers == 0 || depth_slice > UINT64_MAX / layers || color_slice > UINT64_MAX / layers);
	const auto                       depth_size = depth_slice * layers;
	const auto                       color_size = color_slice * layers;
	const std::array                 depth_copies {
	    vk::BufferImageCopy {0,
	                         depth.info.pitch,
	                         depth.info.extent.height,
	                         {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                         {},
	                         depth.info.extent}};
	const std::array color_copies {
	    vk::BufferImageCopy {0,
	                         color.info.pitch,
	                         color.info.extent.height,
	                         {vk::ImageAspectFlagBits::eColor, 0, 0, layers},
	                         {},
	                         color.info.extent}};

	auto                         depth_buffer = m_tiler.GetScratchBuffer(depth_size);
	auto                         color_buffer = m_tiler.GetScratchBuffer(color_size, depth_buffer.buffer);
	const TileManager::D16Layout promote_layout {
	    .width               = depth.info.extent.width,
	    .height              = depth.info.extent.height,
	    .layers              = layers,
	    .source_row_stride   = static_cast<uint64_t>(color.info.pitch) * sizeof(uint16_t),
	    .target_row_stride   = static_cast<uint64_t>(depth.info.pitch) * transfer_bytes,
	    .source_slice_stride = color_slice,
	    .target_slice_stride = depth_slice,
	};
	const bool d32 = DepthAspectTransferFormat(depth.backing.format) == vk::Format::eD32Sfloat;
	if (source_depth) {
		source.Download(depth_copies, depth_buffer.buffer, depth_buffer.offset, depth_buffer.size);
		m_tiler.ConvertD16(depth_buffer, color_buffer, TileManager::D16Direction::Demote, d32,
		                   {.width               = promote_layout.width,
		                    .height              = promote_layout.height,
		                    .layers              = promote_layout.layers,
		                    .source_row_stride   = promote_layout.target_row_stride,
		                    .target_row_stride   = promote_layout.source_row_stride,
		                    .source_slice_stride = promote_layout.target_slice_stride,
		                    .target_slice_stride = promote_layout.source_slice_stride});
		destination.Upload(color_copies, color_buffer.buffer, color_buffer.offset,
		                   color_buffer.size);
	} else {
		source.Download(color_copies, color_buffer.buffer, color_buffer.offset, color_buffer.size);
		m_tiler.ConvertD16(color_buffer, depth_buffer, TileManager::D16Direction::Promote, d32,
		                   promote_layout);
		destination.Upload(depth_copies, depth_buffer.buffer, depth_buffer.offset,
		                   depth_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    source.info.GetColorTransform() == destination.info.GetColorTransform() &&
	    (source.backing.image_type == destination.backing.image_type ||
	     (source.backing.image_type != vk::ImageType::e1D &&
	      destination.backing.image_type != vk::ImageType::e1D)) &&
	    (source.backing.format == destination.backing.format ||
	     (!source_depth && !dest_depth &&
	      vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format)));
	if (direct_copy) {
		destination.CopyImage(source);
	} else if (!CopyD16(destination, source)) {
		if (source.backing.samples != 1 || destination.backing.samples != 1) {
			EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
		}
		auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
		destination.CopyImageWithBuffer(source, copy_buffer, m_tiler);
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	destination.CopyMip(source, mip, layer);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = m_slot_images[cached_id];
	if ((!cached.info.IsDepth() && !requested.IsDepth()) ||
	    cached.info.tile_mode != requested.tile_mode) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	// PPSA04264
	const bool raw_d16_texture =
	    binding == BindingType::Texture && cached.info.IsDepth() &&
	    cached.info.guest_format == Prospero::BufferFormat::k16UNorm &&
	    requested.guest_format == Prospero::BufferFormat::k16UInt &&
	    requested.pixel_format == vk::Format::eR16Uint && cached.backing.samples == 1 &&
	    requested.samples == 1 && requested.data == cached.info.data &&
	    requested.extent == cached.info.extent && requested.resources == cached.info.resources &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	// PPSA04264, PPSA04288
	// A partial view retains the entire matching array layout. HTile belongs to
	// the depth source, independently of the data slices copied into color storage.
	const bool retain_cached_layout =
	    requested.samples == 1 && cached.info.samples == 1 && cached.backing.samples == 1 &&
	    requested.bytes_per_block == cached.info.bytes_per_block &&
	    requested.data.address == cached.info.data.address &&
	    requested.data.size < cached.info.data.size && requested.extent == cached.info.extent &&
	    requested.resources.levels == 1 && cached.info.resources.levels == 1 &&
	    requested.resources.layers != 0 && cached.info.resources.layers != 0 &&
	    requested.resources.layers < cached.info.resources.layers &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    requested.mip_layout[0].offset == 0 &&
	    cached.info.mip_layout[0].offset == 0 &&
	    requested.mip_layout[0].size == requested.data.size &&
	    cached.info.mip_layout[0].size == cached.info.data.size &&
	    requested.data.size % requested.resources.layers == 0 &&
	    cached.info.data.size % cached.info.resources.layers == 0 &&
	    requested.data.size / requested.resources.layers ==
	        cached.info.data.size / cached.info.resources.layers &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    (cached.info.metadata.kind == ImageMetadataKind::None ||
	     cached.info.metadata.kind == ImageMetadataKind::Htile);
	bool recreate = cached.info.resources < requested.resources ||
	                requested.IsVolume() != cached.info.IsVolume();
	switch (binding) {
		case BindingType::Texture:
			recreate |= requested.IsDepth() && !cached.info.IsDepth();
			recreate |= raw_d16_texture;
			break;
		case BindingType::Storage: recreate |= cached.info.IsDepth(); break;
		case BindingType::RenderTarget: recreate |= cached.info.IsDepth(); break;
		case BindingType::DepthTarget:
			recreate |= !cached.info.IsDepth();
			recreate |= cached.info.IsDepth() && !(stencil_match && bpp_match);
			break;
		case BindingType::VideoOut: recreate |= cached.info.IsDepth(); break;
	}
	if (!recreate) {
		return cached_id;
	}
	RefreshImage(cached_id);
	auto info = requested;
	if (retain_cached_layout) {
		info.data       = cached.info.data;
		info.resources  = cached.info.resources;
		info.mip_layout          = cached.info.mip_layout;
		info.tiling              = cached.info.tiling;
		info.tiled_slice_stride  = cached.info.tiled_slice_stride;
		info.linear_slice_stride = cached.info.linear_slice_stride;
		info.first_tail_level    = cached.info.first_tail_level;
	}
	info.htile_clear_mask     = 0;
	const auto replacement_id = InsertImage(info);
	auto&      replacement    = m_slot_images[replacement_id];
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
		} else {
			LOGF_COLOR(Log::Color::BrightYellow,
			           "TextureCache: unsupported cross-format multisample depth copy\n");
		}
	} else if (cached.backing.samples == 1 && replacement.backing.samples > 1 &&
	           replacement.info.IsDepth()) {
		RefreshCopySource(cached_id);
		if (cached.IsBufferModified() || cached.IsDefinitelyCpuDirty()) {
			EXIT("TextureCache: multisample depth conversion source is not native-current\n");
		}
		PrepareImageCopy(replacement);
		m_blit_helper.ReinterpretColorAsMsDepth(cached, replacement);
		CommitGpuWrite(replacement);
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	FreeImage(cached_id);
	return replacement_id;
}

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = m_slot_images.try_get(cached_id);
	if (owner == nullptr) {
		return {merged_id};
	}
	auto&      cached       = *owner;
	const auto current_tick = m_scheduler.CurrentTick();
	const bool safe_to_delete =
	    current_tick - std::min(current_tick, cached.tick_accessed_last) > NumFramesBeforeRemoval;

	const uint32_t requested_block = requested.bytes_per_block * requested.samples;
	const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
	if (requested.data.address == cached.info.data.address &&
	    requested.BlockExtent() == cached.info.BlockExtent() && requested_block == cached_block) {
		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		// Equal pitch does not imply equal mip placement: a changed extent can move
		// a level into or out of the mip tail. These are separate guest layouts.
		if (requested.tile_mode != cached.info.tile_mode ||
		    (requested.resources == cached.info.resources &&
		     !std::equal(requested.mip_layout.begin(), requested.mip_layout.end(),
		                 cached.info.mip_layout.begin(),
		                 [](const ImageMipInfo& left, const ImageMipInfo& right) {
			                 return left.offset == right.offset && left.size == right.size &&
			                        left.pitch == right.pitch && left.height == right.height &&
			                        left.tail_x == right.tail_x && left.tail_y == right.tail_y &&
			                        left.surface_z == right.surface_z;
		                 }))) {
			if (safe_to_delete) {
				FreeImage(cached_id);
			}
			return {merged_id};
		}
		if (requested.GetColorTransform() != cached.info.GetColorTransform()) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			return {ExpandImage(requested, cached_id)};
		}
		// Volume depth is not an array-layer count. A larger depth can retain the
		// same block-slice layout while requiring a larger native image.
		if ((requested.IsVolume() || cached.info.IsVolume()) &&
		    (requested.data.size == cached.info.data.size ||
		     (requested.type == cached.info.type && requested.resources == cached.info.resources &&
		      requested.extent.width == cached.info.extent.width &&
		      requested.extent.height == cached.info.extent.height &&
		      requested.extent.depth > cached.info.extent.depth))) {
			return {ExpandImage(requested, cached_id)};
		}
		// PPSA08394
		// A view cannot change the native image type or grow its extent.
		if (requested.data.size == cached.info.data.size &&
		    requested.resources == cached.info.resources &&
		    ImageViewOps::FormatsCompatible(cached.info.pixel_format, requested.pixel_format) &&
		    (requested.type != cached.info.type
		         ? requested.extent == cached.info.extent
		         : requested.extent.width > cached.info.extent.width &&
		               requested.extent.height >= cached.info.extent.height &&
		               requested.extent.depth >= cached.info.extent.depth)) {
			return {ExpandImage(requested, cached_id)};
		}
		// PS5 mip tails can expose more levels without increasing the guest allocation.
		if (requested.pixel_format == cached.info.pixel_format &&
		    requested.type == cached.info.type && requested.resources > cached.info.resources &&
		    (requested.data.size > cached.info.data.size ||
		     (requested.data.size == cached.info.data.size &&
		      requested.extent == cached.info.extent &&
		      cached.info.resources.levels > 1 &&
		      requested.resources.layers == cached.info.resources.layers))) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.type != cached.info.type) {
			return {merged_id};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = m_slot_images.try_get(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), static_cast<uint32_t>(requested.tile_mode),
		     static_cast<uint32_t>(cached.info.tile_mode));
	}

	const int32_t requested_mip = requested.MipOf(cached.info);
	if (requested_mip >= 0) {
		const int32_t layer = requested.SliceOf(cached.info, requested_mip);
		return {cached_id, requested_mip, layer};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (!merged_id) {
			return {ExpandImage(requested, cached_id)};
		}
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		m_slot_images[merged_id].binding.is_target |= cached.binding.is_target;
		CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
		FreeImage(cached_id);
		return {merged_id};
	}
	if (requested.data.address >= cached.info.data.address && safe_to_delete) {
		FreeImage(cached_id);
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info);
	auto&      source      = m_slot_images[source_id];
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id);
	const int32_t mip = source.info.MipOf(info);
	const int32_t layer = source.info.SliceOf(info, mip);
	if (layer >= 0) {
		CopyImageMip(expanded_id, source_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
	} else {
		CopyImage(expanded_id, source_id);
	}
	FreeImage(source_id);
	return expanded_id;
}

bool TextureCache::CanDownload(const Image& image) const {
	const auto& info = image.info;
	if (info.samples != 1 || image.backing.samples != 1 || info.resources.layers == 0 ||
	    info.data.size % info.resources.layers != 0) {
		return false;
	}
	if (UploadBinding(image) == BindingType::DepthTarget) {
		return IsSupportedDepthPlaneReadback(info) &&
		       Prospero::NumBytesPerElement(info.guest_format) == info.bytes_per_block;
	}
	return info.metadata.compression == VideoOutCompression::Uncompressed &&
	       (!info.IsTiled() || !Prospero::IsFmaskTextureFormat(info.guest_format)) &&
	       info.LinearSize() != 0;
}

void TextureCache::UploadImage(Image& image, Buffer& source, uint64_t source_offset) {
	auto& destination = image.depth_id ? m_slot_images[image.depth_id] : image;
	const auto& info        = image.info;
	const auto  binding     = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	if (binding != BindingType::DepthTarget) {
		if ((binding == BindingType::RenderTarget || binding == BindingType::VideoOut) &&
		    (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
		     info.samples != 1 || image.backing.samples != 1)) {
			EXIT("TextureCache: invalid color-attachment upload\n");
		}
		if (binding == BindingType::VideoOut &&
		    info.metadata.compression != VideoOutCompression::Uncompressed) {
			EXIT("TextureCache: invalid video-out upload\n");
		}
		const auto linear = m_tiler.Detile(source.Handle(), source_offset, info);
		const auto copies = info.BufferCopies(linear.offset);
		destination.Upload(copies, linear.buffer, linear.offset, linear.size);
		return;
	}

	if (info.samples != 1 || destination.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	const auto layers          = info.resources.layers;
	const auto full_slice_size = info.linear_slice_stride;
	auto       linear          = m_tiler.Detile(source.Handle(), source_offset, info);
	auto       copies          = info.BufferCopies(linear.offset);
	if (image.depth_id) {
		for (auto& copy: copies) {
			copy.imageSubresource.baseArrayLayer += image.stencil_subresources.base_layer;
		}
	}
	const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler.GetScratchBuffer(transfer_slice * layers, linear.buffer);
		m_tiler.ConvertD16(
		    linear, promoted, TileManager::D16Direction::Promote,
		    info.pixel_format == vk::Format::eD32SfloatS8Uint,
		    {.width               = info.extent.width,
		     .height              = info.extent.height,
		     .layers              = layers,
		     .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
		     .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
		     .source_slice_stride = full_slice_size,
		     .target_slice_stride = transfer_slice});
		linear = promoted;
		copies.resize(1);
		copies[0].bufferOffset      = linear.offset;
		copies[0].bufferRowLength   = info.pitch;
		copies[0].bufferImageHeight = info.extent.height;
		copies[0].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, 0, layers};
		copies[0].imageExtent       = info.extent;
	}
	destination.Upload(copies, linear.buffer, linear.offset, linear.size);
}

namespace {
struct ZeroUploadStats {
	std::atomic<uint64_t> images {0};
	std::atomic<uint64_t> bytes {0};
	std::atomic<uint64_t> mismatches {0};
	std::atomic<int64_t>  last_log_ms {0};
};
ZeroUploadStats g_zero_uploads;
} // namespace

bool TextureCache::TryUploadNeverWritten(Image& image) {
	if (!Config::SkipZeroUploadsEnabled()) {
		return false;
	}
	const auto& info = image.info;
	// Zero bytes decode to zero only for plain color formats: block-compressed formats and depth or
	// stencil have other encodings of zero, multisampled images are not uploaded here at all.
	if (image.depth_id || info.IsDepth() || info.HasStencil() || info.IsBlock() ||
	    info.samples != 1 || image.backing.samples != 1 || info.data.Empty() ||
	    UploadBinding(image) == BindingType::DepthTarget) {
		return false;
	}
	const auto address = info.data.address;
	const auto size    = info.data.size;
	// GPU-written bytes (a buffer the shaders wrote) are not in the guest memory.
	if (m_buffer_cache.IsRegionGpuModified(address, size) ||
	    !LibKernel::Memory::GuestRangeNeverWritten(address, size)) {
		return false;
	}
	if (Config::VerifyZeroUploadsEnabled() && !LibKernel::Memory::GuestRangeReadsAsZero(address, size)) {
		g_zero_uploads.mismatches.fetch_add(1, std::memory_order_relaxed);
		char text[256];
		std::snprintf(text, sizeof(text),
		              "ERROR: --skip-zero-uploads: guest range 0x%016" PRIx64 "+0x%" PRIx64
		              " has no data in its backing file but a non-zero byte; uploading it instead\n",
		              address, size);
		Log::WriteToConsoleAndLog(text);
		return false;
	}
	if (!image.ClearColorToZero()) {
		return false;
	}
	g_zero_uploads.images.fetch_add(1, std::memory_order_relaxed);
	g_zero_uploads.bytes.fetch_add(size, std::memory_order_relaxed);
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	auto last = g_zero_uploads.last_log_ms.load(std::memory_order_relaxed);
	if (now_ms - last >= 10000 &&
	    g_zero_uploads.last_log_ms.compare_exchange_strong(last, now_ms)) {
		char text[256];
		std::snprintf(text, sizeof(text),
		              "[zero-uploads] images cleared instead of uploaded %" PRIu64
		              ", guest bytes not read %.3f GiB, verify mismatches %" PRIu64 "\n",
		              g_zero_uploads.images.load(),
		              static_cast<double>(g_zero_uploads.bytes.load()) / (1u << 30),
		              g_zero_uploads.mismatches.load());
		Log::WriteToConsoleAndLog(text);
	}
	return true;
}

void TextureCache::InitializeImage(ImageId id) {
	auto& image = m_slot_images[id];
	TrackImage(id);
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	if (image.info.samples > 1) {
		return;
	}
	const bool upload = image.IsBufferModified() || image.IsCpuDirty();
	if (upload) {
		if (!TryUploadNeverWritten(image)) {
			const auto [source, source_offset] =
			    m_buffer_cache.ObtainBufferForImage(image.info.data.address, image.info.data.size);
			if (source == nullptr) {
				EXIT("TextureCache: failed to obtain image upload source\n");
			}
			UploadImage(image, *source, source_offset);
		}
		image.ClearBufferModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	if (upload && MemResidency::Enabled() && image.IsTracked() && !image.IsGpuModified() &&
	    UploadBinding(image) == BindingType::Texture) {
		// --mem-residency: the guest copy of this texture is now redundant with the GPU copy.
		// Watch for the first CPU read of each page.
		MemResidency::NoteProbeArmed(
		    m_page_manager.ArmReadProbes(image.info.data.address, image.info.data.size));
	}
}

void TextureCache::MaterializeViewClear(ImageId id, const ImageDesc& desc) {
	{
		std::scoped_lock lock {m_lock};
		const auto*      image = m_slot_images.try_get(id);
		if (image == nullptr || !image->info.metadata.clear_register_valid) {
			return;
		}
	}
	MaterializeColorClear(id, desc, desc.view_info.base_layer);
}

void TextureCache::MaterializeColorClear(ImageId id, const ImageDesc& binding,
                                         uint32_t metadata_base_layer) {
	const bool        binding_metadata = binding.info.metadata.kind == ImageMetadataKind::Dcc ||
	                                     binding.info.metadata.kind == ImageMetadataKind::Cmask;
	ImageMetadataInfo target {};
	bool              use_target = false;
	if (binding.type == BindingType::Texture || binding.type == BindingType::Storage) {
		// A texture or storage view of a fast-cleared color target sees the clear as well, through
		// the metadata and clear registers of the target's last binding, even if the view has DCC.
		std::scoped_lock lock {m_lock};
		const auto&      image = m_slot_images[id];
		target                 = image.info.metadata;
		use_target =
		    (target.kind == ImageMetadataKind::Dcc || target.kind == ImageMetadataKind::Cmask) &&
		    target.clear_register_valid && image.info.data == binding.info.data &&
		    image.info.resources.levels == 1 &&
		    image.info.resources.layers == binding.info.resources.layers &&
		    binding.info.resources.levels == 1 && binding.view_info.level_count == 1 &&
		    !image.info.IsVolume();
	}
	// Most lookups are plain textures of images without color metadata: leave before copying.
	if (!binding_metadata && !use_target) {
		if (binding.type == BindingType::RenderTarget) {
			// A target bound without color metadata drops the previous binding's clear state.
			std::scoped_lock lock {m_lock};
			auto&            image = m_slot_images[id];
			if (image.info.metadata.kind == ImageMetadataKind::Dcc ||
			    image.info.metadata.kind == ImageMetadataKind::Cmask) {
				image.info.metadata = binding.info.metadata;
			}
		}
		return;
	}
	// From here on, desc decodes the clear as the color target that owns the metadata.
	ImageDesc desc = binding;
	if (use_target) {
		desc.info.metadata    = target;
		desc.type             = BindingType::RenderTarget;
		desc.view_info.format = target.clear_format;
	}
	const auto range = desc.info.metadata.range;
	if (!use_target) {
		std::scoped_lock lock {m_lock};
		auto& image         = m_slot_images[id];
		image.info.metadata = desc.info.metadata;
		// The clear registers are read through the target's format, not a later view's.
		if (desc.type == BindingType::RenderTarget) {
			image.info.metadata.clear_format = desc.view_info.format;
		}
		// Native color metadata must not retain a reused HTile/CMask/FMask clear flag.
		m_surface_metas.erase(range.address);
	}
	if (range.size == 0 || desc.info.resources.levels != 1) {
		return;
	}
	const auto layers = desc.info.TransferLayers();
	// These one-mip surfaces use complete 4 KiB color metadata blocks.
	constexpr uint64_t MetadataBlockSize = 0x1000;
	if (!range.Valid() || range.address % MetadataBlockSize != 0 || layers == 0 ||
	    range.size % layers != 0 || (range.size / layers) % MetadataBlockSize != 0) {
		EXIT("TextureCache: color metadata slices must contain aligned 4 KiB blocks\n");
	}
	const auto& view           = desc.view_info;
	const bool  volume_texture = desc.info.IsVolume() && view.type == vk::ImageViewType::e3D;
	const auto  first          = volume_texture ? 0u : metadata_base_layer;
	const auto  image_first    = volume_texture ? 0u : view.base_layer;
	const auto  count          = volume_texture ? desc.info.extent.depth : view.layer_count;
	if (first >= layers || count > layers - first) {
		EXIT("TextureCache: color view exceeds its native metadata slices\n");
	}
	// Finish native metadata writes before reading backing bytes. This can submit the scheduler,
	// so discovery runs before final draw uploads and never holds the texture lock across it.
	// --known-fills: when the GPU wrote the whole range with one byte-uniform fill, its value is
	// known and the readback (a full GPU drain) is skipped.
	// --known-copies: when the GPU wrote the range by copying it from a source the CPU could read,
	// each slice it needs may be known on its own (one run of equal dwords).
	const bool gpu_modified = m_buffer_cache.IsRegionGpuModified(range.address, range.size);
	uint32_t   fill_value   = 0;
	bool       known_fill   = gpu_modified &&
	                        m_buffer_cache.TryGetKnownFill(range.address, range.size, &fill_value) &&
	                        (fill_value & 0xffu) * 0x01010101u == fill_value;
	const auto slice_size   = range.size / layers;
	std::array<uint32_t, 16> slice_values {};
	bool                     known_slices = false;
	if (gpu_modified && !known_fill && Config::KnownCopiesEnabled() && count <= slice_values.size()) {
		known_slices = true;
		for (uint32_t slice = 0; slice < count && known_slices; slice++) {
			known_slices = m_buffer_cache.TryGetKnownCopy(range.address + slice_size * (first + slice),
			                                              slice_size, &slice_values[slice]);
		}
	}
	// --debug-known-verify: once per interval a known answer is not used. The GPU is drained as
	// usual and the real bytes are compared with what was known (see below).
	static Common::VerifyRation verify_ration;
	bool                        verifying = false;
	if (known_slices) {
		const auto seconds = Config::DebugKnownVerifySeconds();
		const auto now_ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
		                        std::chrono::steady_clock::now().time_since_epoch())
		                        .count();
		verifying = Config::DebugKnownVerifyAll() || (seconds != 0 && verify_ration.Due(now_ns, seconds));
	}
	if (gpu_modified) {
		if (known_fill) {
			Common::GpuPhase::NoteKnownFill(Common::GpuPhase::KnownFill::Hit);
		} else if (known_slices && !verifying) {
			Common::GpuPhase::NoteKnownCopy(Common::GpuPhase::KnownCopy::Hit, count);
		} else {
			if (Config::KnownFillsEnabled()) {
				Common::GpuPhase::NoteKnownFill(Common::GpuPhase::KnownFill::Readback);
			}
			if (Config::KnownCopiesEnabled() && !known_slices) {
				Common::GpuPhase::NoteKnownCopy(Common::GpuPhase::KnownCopy::Partial);
			}
			m_buffer_cache.ReadMemory(range.address, range.size, false,
			                          Common::GpuPhase::DrainCause::ReadbackTextureMeta);
			if (verifying) {
				// The drain is done: the guest copy now holds the GPU's bytes.
				bool mismatch = false;
				for (uint32_t slice = 0; slice < count; slice++) {
					std::vector<uint32_t> real(slice_size / 4);
					const auto address = range.address + slice_size * (first + slice);
					if (!LibKernel::Memory::TryReadBacking(address, real.data(), slice_size)) {
						continue;
					}
					mismatch |= !std::all_of(real.begin(), real.end(),
					                         [&](uint32_t w) { return w == slice_values[slice]; });
				}
				Common::GpuPhase::NoteKnownCopy(Common::GpuPhase::KnownCopy::Verified);
				if (mismatch) {
					Common::GpuPhase::NoteKnownCopy(Common::GpuPhase::KnownCopy::Mismatch);
					std::printf("KNOWN-COPIES MISMATCH metadata range=0x%016" PRIx64 "+0x%" PRIx64
					            ": the recorded copy knowledge was wrong, dropped\n",
					            range.address, range.size);
					std::fflush(stdout);
					m_buffer_cache.ForgetKnownCopies(range.address, range.size);
				}
				known_slices = false;
			}
		}
	}
	const bool known = known_fill || known_slices;
	for (uint32_t slice = 0; slice < count; slice++) {
		const auto address = range.address + slice_size * (first + slice);
		const auto value   = known_fill ? fill_value : (known_slices ? slice_values[slice] : 0u);
		if (known_slices && (value & 0xffu) * 0x01010101u != value) {
			continue; // bytes that differ cannot be a clear: what the read would have found
		}
		uint8_t code = static_cast<uint8_t>(value);
		if (!known && !LibKernel::Memory::TryReadBacking(address, &code, sizeof(code))) {
			EXIT("TextureCache: failed to read color metadata backing\n");
		}
		vk::ClearValue clear {};
		if (!DecodeColorClear(desc, code, clear.color)) {
			continue;
		}
		std::vector<uint8_t> bytes(slice_size);
		if (!known) {
			if (!LibKernel::Memory::TryReadBacking(address, bytes.data(), bytes.size())) {
				EXIT("TextureCache: failed to read color metadata slice\n");
			}
			if (!std::all_of(bytes.begin(), bytes.end(),
			                 [code](uint8_t byte) { return byte == code; })) {
				continue;
			}
		}
		{
			std::scoped_lock lock {m_lock};
			const auto& image = m_slot_images[id];
			const bool depth = image.info.IsDepth();
			if (depth) {
				const auto value = clear.color.float32[0];
				clear.depthStencil = {value, 0u};
			}
			ClearImage(m_scheduler.Current(), id, depth ? image.backing.format : view.format,
			           {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
			            view.base_level, view.level_count,
			            image_first + slice, 1}, clear);
		}
		// Publish the conversion's expanded keys without treating them as guest writes
		// to overlapping image data. Invalidate the buffer before updating its backing.
		if (desc.type != BindingType::VideoOut) {
			std::fill(bytes.begin(), bytes.end(), uint8_t {0xff});
			if (known) {
				// The CPU is about to overwrite the whole slice: the GPU's fill need not be
				// downloaded first (InvalidateMemory would drain the GPU for it).
				Common::GpuPhase::NoteKnownFill(m_buffer_cache.TryDiscardGpuModified(address, slice_size)
				                                    ? Common::GpuPhase::KnownFill::Discarded
				                                    : Common::GpuPhase::KnownFill::DiscardRefused);
			}
			m_buffer_cache.InvalidateMemory(address, slice_size);
			const LibKernel::Memory::ScopedBackingWriteTag write_tag(
			    LibKernel::Memory::BackingWriteTag::Metadata);
			LibKernel::Memory::WriteBacking(address, bytes.data(), bytes.size());
		}
	}
}

void TextureCache::RefreshImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id &&
	    (m_slot_images[image.depth_id].info.metadata.stencil_compressed ||
	     m_slot_images[image.depth_id].info.samples != 1)) {
		return;
	}
	TrackImage(id);
	if (image.IsMaybeCpuDirty()) {
		const auto hash = image.HashGuestEdges();
		if (image.NeedsMaybeCpuHash()) {
			image.SetMaybeCpuHash(hash);
			return;
		}
		(void)image.ResolveMaybeCpuHash(hash);
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		return;
	}
	if (!cpu_dirty) {
		return;
	}
	InitializeImage(id);
}

ImageId TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = m_slot_images[depth_id];
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->info.data == stencil &&
		    owner->info.extent == depth.info.extent) {
			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info);
	}
	auto& record = m_slot_images[association];
	TouchImage(record);
	if (record.depth_id != depth_id || record.stencil_subresources != depth.stencil_subresources) {
		ImageInfo plane {};
		plane.data         = stencil;
		plane.pixel_format = vk::Format::eS8Uint;
		plane.guest_format = Prospero::BufferFormat::k8UInt;
		plane.type         = depth.info.type;
		plane.extent       = depth.info.extent;
		plane.resources    = {depth.stencil_subresources.level_count,
		                      depth.stencil_subresources.layer_count};
		plane.tile_mode    = depth.info.tile_mode;
		plane.pitch =
		    plane.IsTiled() ? TileGetDepthPitch(plane.extent.width, 1, 0) : depth.info.pitch;
		plane.bytes_per_block = 1;
		plane.UpdateSize();
		record.info                 = plane;
		record.depth_id             = depth_id;
		record.stencil_subresources = depth.stencil_subresources;
		// FindImage reads the info and the depth association of registered images.
		++m_image_set_generation;
	}

	return association;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format, uint64_t* unique_generation) {
	if (unique_generation != nullptr) {
		*unique_generation = 0;
	}
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	EXIT_IF(desc.info.data.Empty());
	const auto metadata_base_layer = desc.view_info.base_layer;

	ImageId result {};
	{
		std::scoped_lock lock {m_lock};
		int32_t          view_mip   = -1;
		int32_t          view_layer = -1;
		// The lookup proper: every registered image that overlaps the request is tested with
		// SameBacking; `unique` tells whether the answer came from exactly one SameBacking match and
		// nothing else (see imageLookupMemo.h).
		bool       unique = false;
		const auto lookup = [&]() {
			ImageId      found {};
			bool         stencil_shortcut = false;
			uint32_t     backing_matches  = 0;
			int32_t      mip              = -1;
			int32_t      layer            = -1;
			const auto candidates =
			    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);

			for (const auto id: candidates) {
				const auto& image = m_slot_images[id];
				// Stencil descriptors name the associated plane; its depth owner supplies
				// the native image and the canonical guest tiling.
				if ((desc.type == BindingType::Texture || desc.type == BindingType::Storage) &&
				    image.depth_id && image.info.data == desc.info.data &&
				    m_slot_images[image.depth_id].info.samples == desc.info.samples &&
				    image.info.extent == desc.info.extent && image.info.type == desc.info.type &&
				    image.info.resources.levels >= desc.info.resources.levels &&
				    image.info.resources.layers >= desc.info.resources.layers &&
				    desc.info.bytes_per_block == 1 &&
				    desc.info.guest_format == Prospero::BufferFormat::k8UInt) {
					found            = id;
					stencil_shortcut = true;
					break;
				}
				if (SameBacking(image.info, desc.info, exact_format)) {
					found = id;
					++backing_matches;
				}
			}

			if (!found) {
				for (const auto candidate: candidates) {
					mip                     = -1;
					layer                   = -1;
					const auto& merged_info = found ? m_slot_images[found].info : desc.info;
					const auto  overlap     = ResolveOverlap(merged_info, desc.type, candidate, found);
					if (overlap.image) {
						found = overlap.image;
						mip   = overlap.mip;
						layer = overlap.layer;
					}
				}
			}
			unique     = found && !stencil_shortcut && backing_matches == 1 && mip < 0 && layer < 0;
			view_mip   = mip;
			view_layer = layer;
			return found;
		};

		const bool memo_enabled = Config::ImageLookupMemoEnabled();
		const auto memo_key = ImageLookupKey::Make(desc.info, exact_format,
		                                           static_cast<uint8_t>(desc.type));
		bool       memo_hit = false;
		if (memo_enabled) {
			Common::SlotId remembered;
			if (m_image_lookup_memo.Find(memo_key, m_image_set_generation, remembered)) {
				if (ImageLookupVerifyEnabled()) {
					const auto full = lookup();
					if (!(full == remembered) || !unique) {
						EXIT("TextureCache: image lookup memo disagrees with the full lookup: "
						     "memo=%u/%u full=%u/%u unique=%d address=0x%016" PRIx64 " size=%" PRIu64 "\n",
						     remembered.index, remembered.generation, full.index, full.generation,
						     unique ? 1 : 0, desc.info.data.address, desc.info.data.size);
					}
					ImageLookupNoteVerified(m_image_lookup_memo.GetStats());
				}
				result     = remembered;
				memo_hit   = true;
				view_mip   = -1;
				view_layer = -1;
			}
		}
		if (!memo_hit) {
			result = lookup();
		}
		const bool recordable = memo_enabled && !memo_hit && unique;
		const bool by_unique  = memo_hit || unique;
		const auto looked_up  = result;

		if (result && !m_slot_images[result].depth_id) {
			auto& resolved = m_slot_images[result];
			if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources < desc.info.resources) {
				FreeImage(result);
				result = {};
			}
		}
		if (recordable && result && !m_slot_images[result].depth_id) {
			m_image_lookup_memo.Record(memo_key, m_image_set_generation, result);
		}
		if (!result) {
			result         = InsertImage(desc.info);
			auto& inserted = m_slot_images[result];
			if (m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                    inserted.info.data.size)) {
				inserted.MarkBufferModified();
			}
		}
		if (unique_generation != nullptr && by_unique && result == looked_up) {
			*unique_generation = m_image_set_generation;
		}
		auto& image = m_slot_images[result];
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
	}
	FinishFind(result, desc, metadata_base_layer);
	return result;
}

bool TextureCache::RefindImage(ImageId id, uint64_t generation, const ImageDesc& desc,
                               uint32_t metadata_base_layer) {
	{
		std::scoped_lock lock {m_lock};
		if (generation == 0 || generation != m_image_set_generation) {
			return false;
		}
		auto& image              = m_slot_images[id];
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
	}
	FinishFind(id, desc, metadata_base_layer);
	return true;
}

static bool IsColorMetadata(ImageMetadataKind kind) {
	return kind == ImageMetadataKind::Dcc || kind == ImageMetadataKind::Cmask;
}

bool TextureCache::ReplayKeptImage(ImageId id, uint64_t generation, const ImageDesc& desc, KeptStamp& stamp) {
	if (generation == 0 || __atomic_load_n(&m_image_set_generation, __ATOMIC_ACQUIRE) != generation ||
	    desc.type != BindingType::Texture || IsColorMetadata(desc.info.metadata.kind)) {
		return false;
	}
	auto& image = m_slot_images[id];
	// FinishFind returns before doing anything unless the binding carries colour metadata or the image holds a fast-clear that a
	// texture view of it sees (MaterializeColorClear's binding_metadata / use_target).
	if (!image.registered || (IsColorMetadata(image.info.metadata.kind) && image.info.metadata.clear_register_valid)) {
		return false;
	}
	const auto tick = m_scheduler.CurrentTick();
	if (stamp.access_tick != tick) {
		image.tick_accessed_last = tick;
		stamp.access_tick        = tick;
	}
	if (stamp.touch_tick != m_gc_tick) {
		std::scoped_lock lock {m_lock};
		TouchImage(image);
		stamp.touch_tick = m_gc_tick;
	}
	return true;
}

bool TextureCache::KeptImageNeedsNoWork(ImageId id, uint64_t generation) const {
	if (generation == 0 || __atomic_load_n(&m_image_set_generation, __ATOMIC_ACQUIRE) != generation) {
		return false;
	}
	const auto& image = m_slot_images[id];
	return image.registered && !image.binding.needs_rebind && !image.depth_id && !image.info.HasStencil() &&
	       !image.info.metadata.clear_register_valid && image.RefreshIsNoop();
}

void TextureCache::FinishFind(ImageId id, const ImageDesc& desc, uint32_t metadata_base_layer) {
	MaterializeColorClear(id, desc, metadata_base_layer);
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
		std::scoped_lock lock {m_lock};
		const auto& image = m_slot_images[id];
		const bool guest_dirty = image.IsBufferModified() || image.IsCpuDirty();
		const bool native_current =
		    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
		if (!native_current) {
			EXIT("TextureCache: compressed video-out read requires clean native GPU "
			     "contents\n");
		}
	}
}

void TextureCache::UpdateImage(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	RefreshImage(id);
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
	std::scoped_lock lock {m_lock};
	ImageIds         matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || owner->info.data.address != address) {
			continue;
		}
		if (ensure_valid && owner->depth_id) {
			owner = m_slot_images.try_get(owner->depth_id);
		}
		if (owner == nullptr || (ensure_valid && !owner->SafeToDownload())) {
			continue;
		}
		matches.push_back(id);
	}
	ImageId selected {};
	if (matches.size() == 1) {
		selected = matches.front();
	} else {
		for (const auto id: matches) {
			const auto& image = m_slot_images[id];
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	if (selected && ensure_valid) {
		const auto owner = m_slot_images.try_get(selected);
		if (owner != nullptr && owner->depth_id) {
			selected = owner->depth_id;
		}
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
	}
	if (desc.type == BindingType::Storage) {
		image.MarkGpuModified();
	}
	RefreshImage(id);
	if (image.info.HasStencil() &&
	    desc.info.data.address >= image.info.stencil.address &&
	    desc.info.data.End() <= image.info.stencil.End()) {
		for (const auto stencil_id:
		     FindImagesInRegion(image.info.stencil.address, image.info.stencil.size, false)) {
			const auto* stencil = m_slot_images.try_get(stencil_id);
			if (stencil != nullptr && stencil->depth_id == id &&
			    stencil->info.data == image.info.stencil) {
				RefreshImage(stencil_id);
				break;
			}
		}
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			CommitGpuWrite(image);
			TrackImageDownload(id, image);
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.render_target = true;
	RefreshImage(id);
	CommitGpuWrite(image);
	TrackImageDownload(id, image);
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.depth_target = true;
	if (desc.info.HasStencil()) {
		const auto layers = image.info.resources.layers;
		EXIT_IF(layers == 0 || image.info.data.size % layers != 0 ||
		        desc.info.data.address < image.info.data.address);
		const auto slice_size = image.info.data.size / layers;
		const auto offset     = desc.info.data.address - image.info.data.address;
		EXIT_IF(slice_size == 0 || offset % slice_size != 0 || offset / slice_size >= layers ||
		        desc.info.resources.layers > layers - offset / slice_size);
		image.stencil_subresources = {0, desc.info.resources.levels,
		                             static_cast<uint32_t>(offset / slice_size),
		                             desc.info.resources.layers};
	}
	image.info.stencil = desc.info.stencil;
	image.info.metadata = desc.info.metadata;
	if (desc.info.HasMetadata()) {
		m_surface_metas.emplace(desc.info.metadata.range.address,
		                        MetaDataInfo {.type       = MetaDataInfo::Type::HTile,
		                                      .clear_mask = image.info.htile_clear_mask});
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	if (desc.info.HasStencil()) {
		RefreshImage(AssociateStencil(id, desc.info.stencil));
	}
	return image.FindView(desc.view_info);
}

void TextureCache::MarkGpuWritten(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	TrackImage(id);
	CommitGpuWrite(image);
	if (image.info.HasStencil()) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
}

void TextureCache::CommitGpuWrite(Image& image) {
	if (!image.depth_id && image.backing.image == nullptr) {
		EXIT("TextureCache: GPU writes require a native image or stencil association\n");
	}
	image.ClearBufferModified();
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.MarkGpuModified();
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
	std::scoped_lock      lock {m_lock};
	ImageId               selected {};
	ImageId               stencil_id {};
	vk::ImageAspectFlags  aspect {};
	ImageSubresourceRange subresources;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		vk::ImageAspectFlags candidate {};
		ImageId              candidate_id = id;
		ImageSubresourceRange candidate_subresources {
		    0, owner->info.resources.levels, 0, owner->info.TransferLayers()};
		if (owner->depth_id && owner->info.data.address == address &&
		    owner->info.data.size == size) {
			candidate              = vk::ImageAspectFlagBits::eStencil;
			candidate_id           = owner->depth_id;
			candidate_subresources = owner->stencil_subresources;
			owner        = m_slot_images.try_get(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
				continue;
			}
		} else if (!owner->depth_id && owner->info.data.address == address &&
		           owner->info.data.size == size) {
			candidate = owner->info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
			                                  : vk::ImageAspectFlagBits::eColor;
		}
		if (!candidate) {
			continue;
		}
		if (selected && (selected != candidate_id || subresources != candidate_subresources)) {
			return false;
		}
		selected     = candidate_id;
		stencil_id   = candidate == vk::ImageAspectFlagBits::eStencil ? id : ImageId {};
		aspect       = candidate;
		subresources = candidate_subresources;
	}
	if (!selected) {
		return false;
	}
	auto&          image = m_slot_images[selected];
	vk::ClearValue clear {};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (!DecodeColorDwordFill(image.info.pixel_format, packed_clear, clear.color)) {
			return false;
		}
	} else {
		uint8_t stencil_clear = 0;
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, clear.depthStencil.depth)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
		clear.depthStencil.stencil = stencil_clear;
	}
	ClearImage(command, selected, image.backing.format,
	           {aspect, subresources.base_level, subresources.level_count,
	            subresources.base_layer, subresources.layer_count}, clear);
	if (stencil_id) {
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
	return true;
}

void TextureCache::ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
                              const vk::ImageSubresourceRange& range, const vk::ClearValue& clear) {
	auto& image = m_slot_images[id];
	const auto aspects = image.info.IsDepth() ? ImageViewOps::DepthAspectMask(image.backing.format)
	                                          : vk::ImageAspectFlagBits::eColor;
	EXIT_IF(range.baseMipLevel >= image.info.resources.levels);
	const auto layers = image.info.IsVolume()
	                        ? std::max(image.info.extent.depth >> range.baseMipLevel, 1u)
	                        : image.backing.layers;
	EXIT_IF(command.IsInvalid() || image.depth_id || !range.aspectMask || range.levelCount == 0 ||
	        range.levelCount > image.info.resources.levels - range.baseMipLevel ||
	        range.layerCount == 0 || range.baseArrayLayer >= layers ||
	        range.layerCount > layers - range.baseArrayLayer ||
	        (range.aspectMask & aspects) != range.aspectMask);
	const bool full_subresources = range.baseMipLevel == 0 &&
	                               range.levelCount == image.info.resources.levels &&
	                               range.baseArrayLayer == 0 && range.layerCount == layers;
	const bool full_image = range.aspectMask == aspects && full_subresources;
	TrackImage(id);
	if (!full_image && (image.IsBufferModified() || image.IsCpuDirty())) {
		InitializeImage(id);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	if (image.info.HasStencil() && (range.aspectMask & vk::ImageAspectFlagBits::eStencil)) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		if (!full_subresources) {
			RefreshImage(stencil_id);
		} else {
			TrackImage(stencil_id);
			CommitGpuWrite(m_slot_images[stencil_id]);
		}
	}
	command.EndRendering();
	// Transfer clears use the backing format; aliased clears must encode through their view.
	if (format != image.backing.format || (image.info.IsVolume() && !full_image)) {
		EXIT_NOT_IMPLEMENTED(range.aspectMask != vk::ImageAspectFlagBits::eColor ||
		                     range.levelCount != 1);
		ImageViewInfo view {};
		view.format = format;
		view.type   = range.layerCount == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
		view.base_level  = range.baseMipLevel;
		view.base_layer  = range.baseArrayLayer;
		view.layer_count = range.layerCount;
		view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
		image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		              vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		vk::RenderingAttachmentInfo attachment {};
		attachment.imageView   = image.FindView(view);
		attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.loadOp      = vk::AttachmentLoadOp::eClear;
		attachment.storeOp     = vk::AttachmentStoreOp::eStore;
		attachment.clearValue  = clear;
		vk::RenderingInfo rendering {};
		rendering.renderArea.extent = {
		    std::max(image.info.extent.width >> range.baseMipLevel, 1u),
		    std::max(image.info.extent.height >> range.baseMipLevel, 1u)};
		rendering.layerCount           = range.layerCount;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &attachment;
		command.Handle().beginRendering(&rendering);
		command.Handle().endRendering();
		CommitGpuWrite(image);
		return;
	}
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	auto native_range = range;
	if (image.info.IsVolume()) {
		native_range.baseArrayLayer = 0;
		native_range.layerCount     = 1;
	}
	if (range.aspectMask == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &clear.color, 1, &native_range);
	} else {
		command.Handle().clearDepthStencilImage(image.backing.image,
		                                        vk::ImageLayout::eTransferDstOptimal,
		                                        &clear.depthStencil, 1, &native_range);
	}
	CommitGpuWrite(image);
}

void TextureCache::InvalidateMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid memory-invalidation range\n");
	}
	// --image-presence: a write to pages no image covers needs nothing from the cache.
	if (Config::ImagePresenceEnabled() && !m_image_presence.Any(address, size)) {
		if (Config::ResolvedDrawVerify()) {
			std::scoped_lock lock {m_lock};
			if (!FindImagesInRegion(address, size, true).empty()) {
				EXIT("image presence: no image on 0x%016" PRIx64 "+0x%" PRIx64 " but the page table has one\n", address, size);
			}
		}
		return;
	}
	std::scoped_lock lock {m_lock};
	InvalidateCpuAliases(address, size);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	const auto&    info             = image.info;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.linear_slice_stride;
	const auto     transfer_bytes   = DepthAspectTransferBytes(info.pixel_format);
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			const auto copies = info.BufferCopies(destination_offset);
			image.Download(copies, destination.Handle(), destination_offset, info.data.size);
		} else {
			const auto copies = info.BufferCopies();
			m_tiler.TileImage(image, copies, destination.Handle(), destination_offset,
			                  info.data.size);
		}
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	const std::array copies {vk::BufferImageCopy {0,
	                                              info.pitch,
	                                              info.extent.height,
	                                              {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                                              {},
	                                              info.extent}};
	auto host_linear = m_tiler.GetScratchBuffer(transfer_size);
	image.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto guest_linear = tiled ? m_tiler.GetScratchBuffer(info.LinearSize(), host_linear.buffer)
	                          : TileManager::Result {destination.Handle(), destination_offset,
	                                                 destination.Size() - destination_offset};
	m_tiler.ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                   DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                   {.width               = info.extent.width,
	                    .height              = info.extent.height,
	                    .layers              = layers,
	                    .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                    .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                    .source_slice_stride = transfer_slice,
	                    .target_slice_stride = full_slice_size});
	if (tiled) {
		m_tiler.Tile(guest_linear.buffer, guest_linear.offset, guest_linear.size,
		             destination.Handle(), destination_offset, info.data.size, info);
	}
}

void TextureCache::DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
                                 uint64_t destination_size, uint32_t levels) {
	if (!CanDownload(image)) {
		EXIT("TextureCache: invalid image download\n");
	}
	if (UploadBinding(image) == BindingType::DepthTarget) {
		EXIT_IF(destination_size != image.info.data.size || levels != image.info.resources.levels);
		DownloadDepth(image, destination, destination_offset);
		return;
	}

	const auto& info      = image.info;
	const auto  transform = info.GetColorTransform();
	if (info.IsTiled()) {
		const auto copies = info.BufferCopies(0, levels);
		m_tiler.TileImage(image, copies, destination.Handle(), destination_offset, destination_size,
		                  levels);
	} else if (transform != ColorTransform::None) {
		auto       linear = m_tiler.GetScratchBuffer(destination_size);
		const auto copies = info.BufferCopies(0, levels);
		image.Download(copies, linear.buffer, 0, linear.size);
		m_tiler.TransformColor(linear, {destination.Handle(), destination_offset, destination_size},
		                       transform, false);
	} else {
		const auto copies = info.BufferCopies(destination_offset, levels);
		image.Download(copies, destination.Handle(), destination_offset, destination_size);
	}
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto selected = m_texture_cache.FindImageFromRange(vaddr, size);
	if (!selected) {
		return false;
	}

	std::scoped_lock lock {m_texture_cache.m_lock};
	auto& image = m_texture_cache.m_slot_images[selected];
	// The GPU thread owns image retirement; CPU invalidation can dirty this image after lookup.
	if (!image.SafeToDownload()) {
		return false;
	}
	if (!buffer.IsInBounds(image.info.data.address, 1)) {
		return false;
	}
	const auto buf_offset = buffer.Offset(image.info.data.address);
	const auto available  = buffer.Size() - buf_offset;
	uint32_t   levels     = 0;
	uint64_t   copy_size  = 0;
	if (buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
		levels    = image.info.resources.levels;
		copy_size = image.info.data.size;
	} else if (image.info.IsVolume() || image.info.resources.layers > 1) {
		// Array and volume slices are strided across the full mip chain.
		return false;
	} else {
		for (; levels < image.info.resources.levels; ++levels) {
			const auto& mip = image.info.mip_layout[levels];
			if (mip.size == 0 || mip.offset > available || mip.size > available - mip.offset) {
				break;
			}
			copy_size = std::max(copy_size, mip.offset + mip.size);
		}
	}
	if (copy_size == 0) {
		return false;
	}
	if (!m_texture_cache.CanDownload(image) ||
	    (TextureCache::UploadBinding(image) == TextureCache::BindingType::DepthTarget &&
	     copy_size != image.info.data.size)) {
		return false;
	}
	m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, levels);
	return true;
}

bool TextureCache::DownloadImageMemory(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id) {
		return false;
	}
	if (!image.SafeToDownload() || !CanDownload(image)) {
		return false;
	}
	const auto range    = image.info.data;
	auto&      download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] =
	    download.Map(range.size, std::max<uint64_t>(image.info.bytes_per_block, 4));
	if (mapped == nullptr) {
		EXIT("TextureCache: failed to map reusable download buffer\n");
	}
	download.Commit();
	if (!LibKernel::Memory::TryReadBacking(range.address, mapped, range.size)) {
		return false;
	}
	download.Flush(offset, range.size);

	DownloadImage(image, download, offset, range.size, image.info.resources.levels);
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.Handle();
	barrier.offset              = offset;
	barrier.size                = range.size;
	m_scheduler.EndRendering();
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	m_scheduler.DeferPriorityOperation([&download, range, mapped, offset] {
		download.Invalidate(offset, range.size);
		const LibKernel::Memory::ScopedBackingWriteTag write_tag(
		    LibKernel::Memory::BackingWriteTag::Download);
		LibKernel::Memory::WriteBacking(range.address, mapped, range.size);
	});
	return true;
}

bool TextureCache::HasGpuModifiedImage(uint64_t address, uint64_t size) {
	// --image-presence: no image covers these pages, so none is GPU-modified (no lock, no page-table walk).
	if (Config::ImagePresenceEnabled() && !m_image_presence.Any(address, size)) {
		if (Config::ResolvedDrawVerify() && !m_lock.HeldByCurrentThread()) {
			std::scoped_lock lock {m_lock};
			if (!FindImagesInRegion(address, size, true).empty()) {
				EXIT("image presence: no image on 0x%016" PRIx64 "+0x%" PRIx64 " but the page table has one\n", address, size);
			}
		}
		return false;
	}
	if (m_lock.HeldByCurrentThread()) {
		return true;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, true)) {
		const auto* image = m_slot_images.try_get(id);
		if (image != nullptr && image->IsGpuModified()) {
			return true;
		}
	}
	return false;
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto& image = m_slot_images[id];
		if (image.info.data.address != address) {
			continue;
		}
		if (image.IsGpuModified()) {
			image.ClearGpuModified();
		}
		image.MarkBufferModified();
	}
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size) {
	const auto page_begin = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(address + size, TRACKER_PAGE_SIZE);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		if (owner->Overlaps(address, size)) {
			owner->InvalidateCpuWrite(address, size);
			UntrackImage(id);
			continue;
		}
		const auto image_begin = owner->info.data.address;
		const auto image_end   = owner->info.data.End();
		if (page_end < image_end) {
			UntrackImageHead(id);
		} else if (image_begin < page_begin) {
			UntrackImageTail(id);
		} else {
			MarkAsMaybeDirty(id, *owner);
		}
	}
}

bool TextureCache::IsMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	return found != m_surface_metas.end();
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	return (found->second.clear_mask & (1u << slice)) != 0;
}

bool TextureCache::ClearMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask = UINT32_MAX;
	return true;
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	if (is_clear) {
		found->second.clear_mask |= 1u << slice;
	} else {
		found->second.clear_mask &= ~(1u << slice);
	}
	return true;
}

bool TextureCache::IsRegionRegistered(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_lock};
	return !FindImagesInRegion(address, size, false).empty();
}

void TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::scoped_lock lock {m_lock};
	for (auto metadata = m_surface_metas.begin(); metadata != m_surface_metas.end();) {
		const auto base = metadata->first;
		if (base >= address && base < address + size) {
			metadata = m_surface_metas.erase(metadata);
		} else {
			++metadata;
		}
	}
	auto images = FindImagesInRegion(address, size, false);
	for (const auto id: images) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		FreeImage(id);
	}
}

static TextureGcPolicy::Candidate GcCandidate(const Image& image) {
	TextureGcPolicy::Candidate c;
	c.registered      = image.registered;
	c.has_depth_owner = static_cast<bool>(image.depth_id);
	c.gpu_modified    = image.IsGpuModified();
	c.tiled           = image.info.IsTiled();
	c.buffer_modified = image.IsBufferModified();
	c.cpu_dirty       = image.IsDefinitelyCpuDirty();
	c.maybe_cpu_dirty = image.IsMaybeCpuDirty();
	return c;
}

void TextureCache::LogGpuWrittenFree(ImageId id, const char* path, uint64_t tick, bool downloaded) {
	const auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->IsGpuModified()) {
		return;
	}
	VRAM_LOG("TexGC: free gpu-written image id=%u via=%s gc_tick=%" PRIu64 " addr=0x%" PRIx64
	         " size=%" PRIu64 " %ux%u fmt=%d tiled=%d rt=%d downloaded=%d safe=%d buffer_modified=%d "
	         "cpu_dirty=%d maybe_cpu_dirty=%d used_mem_mib=%" PRIu64 "\n",
	         static_cast<unsigned>(id.index), path, tick, image->info.data.address,
	         image->info.data.size, image->info.extent.width, image->info.extent.height,
	         static_cast<int>(image->info.pixel_format), image->info.IsTiled() ? 1 : 0,
	         image->usage.render_target ? 1 : 0, downloaded ? 1 : 0,
	         image->SafeToDownload() ? 1 : 0, image->IsBufferModified() ? 1 : 0,
	         image->IsDefinitelyCpuDirty() ? 1 : 0, image->IsMaybeCpuDirty() ? 1 : 0,
	         m_total_used_memory >> 20);
}

void TextureCache::RunGarbageCollector() {
	std::scoped_lock lock {m_lock};
	const bool       frame_clock = Config::GcClockFrames();
	const uint64_t   run         = m_gc_runs++;
	uint64_t         tick        = 0;
	if (frame_clock) {
		// Monotone: the frame count only grows, and the fallback step covers stretches without
		// presents.
		tick      = std::max(m_gc_tick, GcClock::Tick(GcClock::Mode::Frames, run,
		                                              Common::GpuPhase::Frames()));
		m_gc_tick = tick;
	} else {
		tick = m_gc_tick++;
	}
	const auto clock_mode = frame_clock ? GcClock::Mode::Frames : GcClock::Mode::Runs;
	const auto ages       = GcClock::TextureAgesFor(clock_mode, Config::GetVblankFrequency());
	if (Config::VramAuditSeconds() != 0 && !m_vram_audit_done &&
	    std::chrono::steady_clock::now() - m_created >=
	        std::chrono::seconds(Config::VramAuditSeconds())) {
		m_vram_audit_done = true;
		AuditVram();
	}
	if (Config::VramStatsEnabled()) {
		m_graphics.LogMemoryBudgetPeriodic();
	}
	if (!Config::TextureGcEnabled()) {
		return;
	}
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = m_total_used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && m_total_used_memory >= m_critical_gc_memory;
		const uint64_t want_age = aggressive ? ages.aggressive : pressured ? ages.pressured : ages.normal;
		if (!GcClock::AgeReached(clock_mode, tick, want_age)) {
			return;
		}
		const uint64_t age       = std::min<uint64_t>(want_age, tick);
		size_t         deletions = aggressive ? 40 : pressured ? 20 : 10;
		// Images the policy keeps are passed over without using the deletion budget: they are
		// the ones nobody touches again, so they stay at the front of the LRU list and would
		// otherwise starve the walk (textureGcPolicy.h).
		std::vector<ImageId> candidates;
		candidates.reserve(deletions);
		// Deleting depth recursively deletes its stencil association, so finish LRU traversal
		// first.
		m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) {
			const auto* owner = m_slot_images.try_get(id);
			if (owner == nullptr ||
			    TextureGcPolicy::Classify(GcCandidate(*owner), pressured, aggressive) ==
			        TextureGcPolicy::Action::Keep) {
				return false;
			}
			candidates.push_back(id);
			return candidates.size() == deletions;
		});
		TextureGcPolicy::Walk(
		    candidates, deletions,
		    [&](ImageId id) {
			    const auto* owner = m_slot_images.try_get(id);
			    return owner == nullptr
			               ? TextureGcPolicy::Action::Keep
			               : TextureGcPolicy::Classify(GcCandidate(*owner), pressured, aggressive);
		    },
		    [&](ImageId id, TextureGcPolicy::Action action) {
			    const bool download = action == TextureGcPolicy::Action::DownloadThenFree;
			    if (download && !DownloadImageMemory(id)) {
				    return false;
			    }
			    if (Config::TextureGcLog()) {
				    LogGpuWrittenFree(id, "gc", tick, download);
			    }
			    FreeImage(id);
			    if (m_total_used_memory < m_critical_gc_memory && aggressive) {
				    deletions >>= 2;
				    aggressive = false;
			    }
			    if (m_total_used_memory < m_pressure_gc_memory && pressured) {
				    deletions >>= 1;
				    pressured = false;
			    }
			    return true;
		    });
	};
	collect(false);
	if (m_total_used_memory >= m_critical_gc_memory) {
		collect(true);
	}
}

// Called when an image allocation failed, with m_lock held by the caller (InsertImage paths).
// Evicts images idle for a couple of GC ticks (oldest first, up to 1/8 of the device budget) that
// can be dropped without losing guest-visible data, then waits for the GPU so the memory is really released.
void TextureCache::ReclaimMemory() {
	const uint64_t tick = m_gc_tick;
	const uint64_t age  = std::min<uint64_t>(2, tick);
	std::vector<ImageId> candidates;
	m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) {
		candidates.push_back(id);
		return false;
	});
	// Free oldest-first until a share of the device budget is back (scales with the GPU), or
	// until nothing idle is left.
	const uint64_t target =
	    m_graphics.CanReportMemoryUsage() ? m_graphics.GetTotalMemoryBudget() / 8
	                                       : std::numeric_limits<uint64_t>::max();
	size_t   freed = 0;
	uint64_t bytes = 0;
	for (const auto id: candidates) {
		if (bytes >= target) {
			break;
		}
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || !owner->registered || owner->depth_id) {
			continue;
		}
		if (owner->IsGpuModified()) {
			if (!owner->SafeToDownload() || owner->info.IsTiled() || !DownloadImageMemory(id)) {
				continue;
			}
		}
		bytes += owner->AccountedSize();
		if (Config::TextureGcLog()) {
			LogGpuWrittenFree(id, "reclaim", tick, true);
		}
		FreeImage(id);
		freed++;
	}
	VRAM_LOG("Texture cache reclaim: freed %zu images (%" PRIu64 " MiB accounted)\n", freed,
	     bytes >> 20);
	// Deferred deletions are released once the GPU is done with the current work.
	const auto tick_now = m_scheduler.CurrentTick();
	Common::GpuPhase::DrainCauseScope drain_cause(Common::GpuPhase::DrainCause::TextureReclaim);
	m_scheduler.Wait(tick_now);
	m_scheduler.PopPendingOperations();
}

// Debug (--vram-audit): one-off report of the resident image mix by class and format, and of
// textures whose guest content is identical (xxHash3 of the full guest range). Caller holds m_lock.
void TextureCache::AuditVram() {
	struct Group {
		uint64_t     bytes  = 0;
		uint32_t     count  = 0;
		std::string  label;
	};
	struct Totals {
		uint64_t bytes = 0;
		uint32_t count = 0;
	};
	std::map<std::string, Totals> by_format_uncompressed;
	std::map<std::string, Totals> by_format_block;
	Totals                        render_targets, depth, textures_block, textures_plain, other;
	// (hash, guest size) -> images with that content; the exact key adds format and extent.
	std::map<std::pair<uint64_t, uint64_t>, Group>                     content;
	std::map<std::tuple<uint64_t, uint64_t, int, uint32_t, uint32_t, uint32_t>, Group> exact;
	uint64_t hashed_bytes = 0;
	uint32_t hash_failures = 0;
	std::vector<uint8_t> chunk(1u << 20);

	m_slot_images.ForEach([&](ImageId, const Image& image) {
		if (image.backing.image == nullptr) {
			return;
		}
		const uint64_t vram = image.backing.alloc_size;
		const auto     fmt  = static_cast<int>(image.info.pixel_format);
		const bool     block = fmt >= 131 && fmt <= 184;
		if (image.usage.depth_target) {
			depth.bytes += vram, depth.count++;
			return;
		}
		if (image.usage.render_target || image.usage.video_out) {
			render_targets.bytes += vram, render_targets.count++;
			return;
		}
		if (!image.usage.texture) {
			other.bytes += vram, other.count++;
			return;
		}
		auto& totals = block ? textures_block : textures_plain;
		totals.bytes += vram, totals.count++;
		auto& per_format = block ? by_format_block : by_format_uncompressed;
		auto& entry      = per_format[vk::to_string(image.info.pixel_format)];
		entry.bytes += vram, entry.count++;

		const auto range = image.info.data;
		if (range.Empty()) {
			return;
		}
		XXH3_state_t* state = XXH3_createState();
		XXH3_64bits_reset(state);
		bool ok = true;
		for (uint64_t offset = 0; offset < range.size && ok; offset += chunk.size()) {
			const auto size = std::min<uint64_t>(chunk.size(), range.size - offset);
			ok = LibKernel::Memory::TryReadBacking(range.address + offset, chunk.data(), size);
			if (ok) {
				XXH3_64bits_update(state, chunk.data(), static_cast<size_t>(size));
			}
		}
		const uint64_t hash = XXH3_64bits_digest(state);
		XXH3_freeState(state);
		if (!ok) {
			hash_failures++;
			return;
		}
		hashed_bytes += range.size;
		auto label = fmt::format("{} {}x{}x{} mips={}", vk::to_string(image.info.pixel_format),
		                         image.info.extent.width, image.info.extent.height,
		                         image.info.extent.depth, image.info.resources.levels);
		auto& by_content = content[{hash, range.size}];
		by_content.bytes += vram, by_content.count++, by_content.label = label;
		auto& by_exact = exact[{hash, range.size, fmt, image.info.extent.width,
		                        image.info.extent.height, image.info.resources.levels}];
		by_exact.bytes += vram, by_exact.count++, by_exact.label = label;
	});

	const auto mib = [](uint64_t bytes) { return bytes >> 20; };
	const auto resident = textures_block.bytes + textures_plain.bytes + render_targets.bytes +
	                      depth.bytes + other.bytes;
	VRAM_LOG("VRAM audit (resident images %" PRIu64 " MiB): sampled BC/ASTC %" PRIu64 " MiB (%u), "
	         "sampled uncompressed %" PRIu64 " MiB (%u), render targets %" PRIu64 " MiB (%u), "
	         "depth %" PRIu64 " MiB (%u), other %" PRIu64 " MiB (%u)\n",
	         mib(resident), mib(textures_block.bytes), textures_block.count,
	         mib(textures_plain.bytes), textures_plain.count, mib(render_targets.bytes),
	         render_targets.count, mib(depth.bytes), depth.count, mib(other.bytes), other.count);
	const auto dump_formats = [&](const char* title, const std::map<std::string, Totals>& m) {
		std::vector<std::pair<std::string, Totals>> sorted(m.begin(), m.end());
		std::sort(sorted.begin(), sorted.end(),
		          [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
		for (const auto& [name, totals]: sorted) {
			VRAM_LOG("VRAM audit %s: %s = %" PRIu64 " MiB (%u images)\n", title, name.c_str(),
			         mib(totals.bytes), totals.count);
		}
	};
	dump_formats("uncompressed", by_format_uncompressed);
	dump_formats("block", by_format_block);
	const auto duplicate_bytes = [](const auto& groups, std::vector<const Group*>& dups) {
		uint64_t total = 0;
		for (const auto& [key, group]: groups) {
			if (group.count > 1) {
				total += group.bytes - group.bytes / group.count;
				dups.push_back(&group);
			}
		}
		return total;
	};
	std::vector<const Group*> content_dups, exact_dups;
	const auto content_total = duplicate_bytes(content, content_dups);
	const auto exact_total   = duplicate_bytes(exact, exact_dups);
	VRAM_LOG("VRAM audit duplicates: hashed %" PRIu64 " MiB of guest data (%u unreadable); "
	         "same content (any format) wastes %" PRIu64 " MiB in %zu groups, same content+format+"
	         "extent wastes %" PRIu64 " MiB in %zu groups\n",
	         mib(hashed_bytes), hash_failures, mib(content_total), content_dups.size(),
	         mib(exact_total), exact_dups.size());
	std::sort(exact_dups.begin(), exact_dups.end(),
	          [](const Group* a, const Group* b) { return a->bytes > b->bytes; });
	for (size_t i = 0; i < std::min<size_t>(exact_dups.size(), 12); i++) {
		VRAM_LOG("VRAM audit duplicate: %s x%u = %" PRIu64 " MiB total\n",
		         exact_dups[i]->label.c_str(), exact_dups[i]->count, mib(exact_dups[i]->bytes));
	}
}

void TextureCache::CollectResidencyRanges(MemResidency::Snapshot& snapshot) {
	using MemResidency::RangeClass;
	std::scoped_lock lock {m_lock};
	m_slot_images.ForEach([&](ImageId, const Image& image) {
		if (!image.registered || image.info.data.Empty() || image.backing.image == nullptr) {
			return;
		}
		RangeClass cls = RangeClass::TextureOther;
		if (image.usage.depth_target) {
			cls = RangeClass::Depth;
		} else if (image.usage.render_target || image.usage.video_out) {
			cls = RangeClass::RenderTarget;
		} else if (image.usage.texture && image.IsTracked() && !image.IsCpuDirty() &&
		           !image.IsGpuModified()) {
			cls = RangeClass::TextureClean;
		}
		snapshot.ranges[static_cast<size_t>(cls)].emplace_back(image.info.data.address,
		                                                       image.info.data.size);
	});
}

MemResidency::FaultClass TextureCache::ClassifyAddress(uint64_t address) {
	using MemResidency::FaultClass;
	if (m_lock.HeldByCurrentThread() || !GuestRange {address, 1}.Valid()) {
		return FaultClass::NonImage;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, 1, true)) {
		const auto* image = m_slot_images.try_get(id);
		if (image == nullptr) {
			continue;
		}
		if (image->usage.depth_target) {
			return FaultClass::Depth;
		}
		if (image->usage.render_target || image->usage.video_out) {
			return FaultClass::RenderTarget;
		}
		return FaultClass::Texture;
	}
	return FaultClass::NonImage;
}

void TextureCache::ProcessDownloadImages() {
	std::scoped_lock lock {m_lock};
	for (const auto id: m_download_images) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)DownloadImageMemory(id);
		}
	}
	m_download_images.clear();
}

} // namespace Libs::Graphics
