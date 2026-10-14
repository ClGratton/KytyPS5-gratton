#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGELOOKUPMEMO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGELOOKUPMEMO_H_

// --image-lookup-memo: a direct-mapped memo in front of TextureCache::FindImage.
//
// FindImage walks the page table over the whole guest range of the requested image (one owner list
// per 16 KiB page, so a few thousand pages for a render-target sized texture), tests every
// overlapping image with SameBacking and, if nothing matches, runs the overlap resolution. A draw
// resolves the same few hundred textures again and again, and the answer only changes when the set
// of registered images changes (an image registered or unregistered), or when the fields of a
// registered image that the lookup reads change (only TextureCache::AssociateStencil does that).
// The cache keeps a generation number for exactly those events; an entry is valid while its
// generation is the current one.
//
// What is remembered: a lookup that was decided by SameBacking alone, with exactly one registered
// image matching, no stencil-association shortcut taken and no overlap resolution (view mip/layer
// untouched). Such a result is independent of the order of the candidates, so it is the result of
// every later lookup with the same request while the generation holds. Everything else is looked up
// in full, every time. The key holds every field of the request that the lookup reads (the
// SameBacking fields, the guest format and the binding type that the stencil shortcut tests); it is
// compared in full, so a hash collision can only cost a miss.
//
// Idea from BryanKAdams/BryKytyPS5 fad08c098 ("remember unique FindImage matches until the image
// set changes", GPL-2.0), re-implemented as a separate class with a stricter key and a verifier
// (KYTY_IMAGE_LOOKUP_VERIFY=1 runs the full lookup on every hit and compares).
//
// Pure data structure, no locking: the caller holds TextureCache::m_lock. Unit tested in
// tests/ImageLookupMemoTests.cpp.

#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "common/slotVector.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics {

struct ImageLookupKey {
	GuestRange             data;
	vk::Extent3D           extent;
	ImageSubresources      resources;
	uint32_t               samples         = 0;
	uint32_t               bytes_per_block = 0;
	Prospero::TileMode     tile_mode       = Prospero::TileMode::kLinear;
	vk::Format             pixel_format    = vk::Format::eUndefined;
	Prospero::BufferFormat guest_format    = Prospero::BufferFormat::kInvalid;
	Prospero::ImageType    type            = Prospero::ImageType::kColor2D;
	uint8_t                binding_type    = 0; // TextureCache::BindingType
	bool                   bgra16          = false;
	bool                   exact_format    = false;

	[[nodiscard]] static ImageLookupKey Make(const ImageInfo& info, bool exact_format,
	                                         uint8_t binding_type) noexcept {
		ImageLookupKey key;
		key.data            = info.data;
		key.extent          = info.extent;
		key.resources       = info.resources;
		key.samples         = info.samples;
		key.bytes_per_block = info.bytes_per_block;
		key.tile_mode       = info.tile_mode;
		key.pixel_format    = info.pixel_format;
		key.guest_format    = info.guest_format;
		key.type            = info.type;
		key.binding_type    = binding_type;
		key.bgra16          = info.bgra16;
		key.exact_format    = exact_format;
		return key;
	}

	[[nodiscard]] bool operator==(const ImageLookupKey& rhs) const noexcept {
		return data == rhs.data && extent == rhs.extent && resources == rhs.resources &&
		       samples == rhs.samples && bytes_per_block == rhs.bytes_per_block &&
		       tile_mode == rhs.tile_mode && pixel_format == rhs.pixel_format &&
		       guest_format == rhs.guest_format && type == rhs.type &&
		       binding_type == rhs.binding_type && bgra16 == rhs.bgra16 &&
		       exact_format == rhs.exact_format;
	}

	[[nodiscard]] uint64_t Hash() const noexcept {
		auto h = data.address ^ (data.size * 0x9e3779b97f4a7c15ull);
		h ^= (static_cast<uint64_t>(pixel_format) << 1u) ^
		     (static_cast<uint64_t>(extent.width) * 0xc2b2ae3d27d4eb4full) ^
		     (static_cast<uint64_t>(extent.height) << 40u) ^ (exact_format ? 1u : 0u);
		h ^= h >> 29u;
		h *= 0xbf58476d1ce4e5b9ull;
		h ^= h >> 32u;
		return h;
	}
};

class ImageLookupMemo {
public:
	static constexpr size_t Slots = 1024;

	struct Stats {
		uint64_t hits    = 0;
		uint64_t misses  = 0;
		uint64_t records = 0;
	};

	// The image remembered for `key` under `generation`, or false.
	[[nodiscard]] bool Find(const ImageLookupKey& key, uint64_t generation, Common::SlotId& id) {
		const auto& entry = m_entries[key.Hash() % Slots];
		if (entry.generation == generation && generation != 0 && entry.key == key) {
			id = entry.id;
			++m_stats.hits;
			return true;
		}
		++m_stats.misses;
		return false;
	}

	// Remembers `id` as the unique SameBacking match of `key`, valid for `generation`.
	void Record(const ImageLookupKey& key, uint64_t generation, Common::SlotId id) {
		auto& entry      = m_entries[key.Hash() % Slots];
		entry.generation = generation;
		entry.key        = key;
		entry.id         = id;
		++m_stats.records;
	}

	void Clear() {
		for (auto& entry: m_entries) {
			entry.generation = 0;
		}
	}

	[[nodiscard]] const Stats& GetStats() const { return m_stats; }

private:
	struct Entry {
		uint64_t       generation = 0; // 0: empty
		ImageLookupKey key;
		Common::SlotId id;
	};
	std::array<Entry, Slots> m_entries {};
	Stats                    m_stats;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGELOOKUPMEMO_H_
