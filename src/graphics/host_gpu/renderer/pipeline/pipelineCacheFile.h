#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECACHEFILE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECACHEFILE_H_

// The on-disk driver VkPipelineCache file (`_PipelineCache/<TITLE_ID>.bin`): its signature, its
// validation and the build gates. Pure functions so that the invalidation rules are unit tested
// (tests/PipelineCacheFileTests.cpp); PipelineCache::InitializeDriverCache and Save() use them.
//
// File layout: signature text | 8-byte XXH3-64 of the payload | payload (the driver's blob).
//
// What keeps a stale cache from being reused:
//  * The signature names the device and driver (vendor, device, driver version,
//    pipelineCacheUUID); a different GPU or driver, or a different layout (the schema version),
//    gives a different signature and the file is dropped.
//  * The payload hash catches truncation and corruption.
//  * The driver validates its own blob header and addresses each entry by a hash of the whole
//    pipeline creation input (SPIR-V, state, flags), so a build that changes how pipelines are
//    made, or the SPIR-V it emits, only misses: an entry is never matched to a pipeline it was
//    not created for. That is why the git revision need not be in the signature (key = schema).
//  * A payload beyond kMaxPayloadBytes is dropped: with the revision out of the key, entries of
//    superseded builds are never evicted, and a cap bounds that growth.

#include <fmt/format.h>
#include <xxhash.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::PipelineCacheFile {

enum class KeyMode { Revision, Schema };

// Bump when the file layout or the meaning of the signature changes. Changes to the emulator's
// pipeline creation do not need a bump (see above).
constexpr uint32_t kSchemaVersion = 1;

// Beyond this the file is discarded and rebuilt (Astro Bot's cache is about 25 MiB).
constexpr uint64_t kMaxPayloadBytes = 512ull << 20;

struct DeviceIdentity {
	uint32_t                vendor_id      = 0;
	uint32_t                device_id      = 0;
	uint32_t                driver_version = 0;
	std::array<uint8_t, 16> uuid {};
};

[[nodiscard]] inline std::string Signature(KeyMode mode, std::string_view git_revision,
                                           const DeviceIdentity& device,
                                           uint32_t schema_version = kSchemaVersion) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(device.uuid.size() * 2, '0');
	for (size_t i = 0; i < device.uuid.size(); i++) {
		uuid[i * 2]     = hex[device.uuid[i] >> 4u];
		uuid[i * 2 + 1] = hex[device.uuid[i] & 0xfu];
	}
	if (mode == KeyMode::Schema) {
		return fmt::format("KytyPCS:{}:{:08x}:{:08x}:{:08x}:{}\n", schema_version,
		                   device.vendor_id, device.device_id, device.driver_version, uuid);
	}
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", git_revision, device.vendor_id,
	                   device.device_id, device.driver_version, uuid);
}

// Why the cache is not used at all in this build (nullopt = allowed). Schema keys do not involve
// the revision, so dev, dirty and unknown-revision builds may use and share the cache; the
// revision key needs a known, clean Release revision because every commit drops the file.
[[nodiscard]] inline std::optional<std::string_view>
DisabledReason(KeyMode mode, bool release_build, std::string_view git_hash,
               std::string_view git_revision) {
	if (mode == KeyMode::Schema) {
		return std::nullopt;
	}
	if (!release_build) {
		return "non-Release build";
	}
	if (git_hash == "unknown" || git_revision == "unknown") {
		return "unknown git revision";
	}
	if (git_hash.ends_with("-dirty")) {
		return "dirty build";
	}
	return std::nullopt;
}

enum class LoadStatus {
	Valid,
	TooSmall,          // shorter than signature + hash
	TooLarge,          // payload beyond kMaxPayloadBytes
	SignatureMismatch, // other device, driver, key mode, revision or schema version
	PayloadCorrupt,    // hash mismatch: truncated or damaged
};

struct LoadResult {
	LoadStatus                status = LoadStatus::TooSmall;
	std::span<const uint8_t>  payload;
};

[[nodiscard]] inline const char* ToString(LoadStatus status) {
	switch (status) {
		case LoadStatus::Valid: return "valid";
		case LoadStatus::TooSmall: return "invalid file size";
		case LoadStatus::TooLarge: return "cache larger than the cap";
		case LoadStatus::SignatureMismatch: return "driver, emulator, or schema mismatch";
		case LoadStatus::PayloadCorrupt: return "payload hash mismatch";
	}
	return "unknown";
}

[[nodiscard]] inline LoadResult Load(std::span<const uint8_t> file, std::string_view signature) {
	if (file.size() < signature.size() + sizeof(uint64_t)) {
		return {LoadStatus::TooSmall, {}};
	}
	const auto payload_size = file.size() - signature.size() - sizeof(uint64_t);
	if (payload_size > kMaxPayloadBytes) {
		return {LoadStatus::TooLarge, {}};
	}
	if (std::memcmp(file.data(), signature.data(), signature.size()) != 0) {
		return {LoadStatus::SignatureMismatch, {}};
	}
	uint64_t stored_hash = 0;
	std::memcpy(&stored_hash, file.data() + signature.size(), sizeof(stored_hash));
	const auto payload = file.subspan(signature.size() + sizeof(uint64_t));
	if (XXH3_64bits(payload.data(), payload.size()) != stored_hash) {
		return {LoadStatus::PayloadCorrupt, {}};
	}
	return {LoadStatus::Valid, payload};
}

// signature | hash | payload, ready to write.
[[nodiscard]] inline std::vector<uint8_t> Store(std::string_view signature,
                                                std::span<const uint8_t> payload) {
	std::vector<uint8_t> file;
	file.reserve(signature.size() + sizeof(uint64_t) + payload.size());
	file.insert(file.end(), signature.begin(), signature.end());
	const uint64_t hash = XXH3_64bits(payload.data(), payload.size());
	const auto*    hash_bytes = reinterpret_cast<const uint8_t*>(&hash);
	file.insert(file.end(), hash_bytes, hash_bytes + sizeof(hash));
	file.insert(file.end(), payload.begin(), payload.end());
	return file;
}

} // namespace Libs::Graphics::PipelineCacheFile

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECACHEFILE_H_ */
