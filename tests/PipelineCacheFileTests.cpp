// The invalidation rules of the on-disk driver pipeline cache (pipelineCacheFile.h). With
// --pipeline-cache-key schema (the default) the git revision is not part of the signature, so a
// new emulator build keeps its cache; these tests pin everything that must still drop it.

#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics::PipelineCacheFile;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PipelineCacheFileTests: failed: %s\n", message);
		std::abort();
	}
}

DeviceIdentity Gpu() {
	DeviceIdentity device;
	device.vendor_id      = 0x10de;
	device.device_id      = 0x2208;
	device.driver_version = 0x9a8c0000;
	for (size_t i = 0; i < device.uuid.size(); i++) {
		device.uuid[i] = static_cast<uint8_t>(0xa0 + i);
	}
	return device;
}

std::vector<uint8_t> Payload(size_t size = 4096) {
	std::vector<uint8_t> payload(size);
	for (size_t i = 0; i < size; i++) {
		payload[i] = static_cast<uint8_t>((i * 131u + 7u) & 0xffu);
	}
	return payload;
}

LoadResult LoadFile(const std::vector<uint8_t>& file, const std::string& signature) {
	return Load(file, signature);
}

// The schema key is what lets a new build (another git revision, a dev or dirty build) keep the
// cache, so the revision must not reach it; the revision key must change with every revision.
void TestSchemaSignatureIgnoresRevision() {
	const auto device = Gpu();
	Check(Signature(KeyMode::Schema, "1111111", device) == Signature(KeyMode::Schema, "2222222", device),
	      "the schema signature does not depend on the git revision");
	Check(Signature(KeyMode::Revision, "1111111", device) != Signature(KeyMode::Revision, "2222222", device),
	      "the revision signature changes with the git revision");
	Check(Signature(KeyMode::Schema, "x", device).starts_with("KytyPCS:"), "schema signature prefix");
	Check(Signature(KeyMode::Revision, "x", device).starts_with("KytyPC1:"), "revision signature prefix");
	Check(Signature(KeyMode::Schema, "x", device).back() == '\n', "signature ends with a newline");
}

// Everything that identifies the device and driver must reach the signature, in both modes.
void TestSignatureTracksTheDevice() {
	for (const auto mode: {KeyMode::Schema, KeyMode::Revision}) {
		const auto base = Gpu();
		const auto sig  = Signature(mode, "abc", base);
		auto       vendor = base;
		vendor.vendor_id = 0x1002;
		auto device = base;
		device.device_id = 0x2204;
		auto driver = base;
		driver.driver_version += 1;
		auto uuid = base;
		uuid.uuid[15] ^= 1;
		Check(Signature(mode, "abc", vendor) != sig, "another vendor drops the cache");
		Check(Signature(mode, "abc", device) != sig, "another device drops the cache");
		Check(Signature(mode, "abc", driver) != sig, "a driver update drops the cache");
		Check(Signature(mode, "abc", uuid) != sig, "another pipelineCacheUUID drops the cache");
	}
}

void TestSchemaVersionBumpDropsOldFiles() {
	const auto device = Gpu();
	const auto v1     = Signature(KeyMode::Schema, "r", device, 1);
	const auto v2     = Signature(KeyMode::Schema, "r", device, 2);
	Check(v1 != v2, "the schema version is part of the schema signature");
	const auto file = Store(v1, Payload());
	Check(LoadFile(file, v1).status == LoadStatus::Valid, "same schema version loads");
	Check(LoadFile(file, v2).status == LoadStatus::SignatureMismatch, "a bumped schema version drops the file");
	Check(Signature(KeyMode::Schema, "r", device) == Signature(KeyMode::Schema, "r", device, kSchemaVersion),
	      "the default schema version is kSchemaVersion");
}

void TestRoundTrip() {
	const auto payload = Payload(100000);
	const auto sig     = Signature(KeyMode::Schema, "r", Gpu());
	const auto file    = Store(sig, payload);
	Check(file.size() == sig.size() + sizeof(uint64_t) + payload.size(), "file layout size");
	const auto loaded = LoadFile(file, sig);
	Check(loaded.status == LoadStatus::Valid, "a stored file loads");
	Check(loaded.payload.size() == payload.size() &&
	          std::equal(payload.begin(), payload.end(), loaded.payload.begin()),
	      "the payload comes back unchanged");
}

// A cache written by one emulator build is valid for another under the schema key, and dropped
// under the revision key; a driver update drops it under both.
void TestAcrossBuildsAndDrivers() {
	const auto device  = Gpu();
	const auto payload = Payload();
	const auto schema_file   = Store(Signature(KeyMode::Schema, "build-A", device), payload);
	const auto revision_file = Store(Signature(KeyMode::Revision, "build-A", device), payload);

	Check(LoadFile(schema_file, Signature(KeyMode::Schema, "build-B", device)).status == LoadStatus::Valid,
	      "schema key: a new emulator build keeps the cache");
	Check(LoadFile(revision_file, Signature(KeyMode::Revision, "build-B", device)).status ==
	          LoadStatus::SignatureMismatch,
	      "revision key: a new emulator build drops the cache");
	Check(LoadFile(revision_file, Signature(KeyMode::Revision, "build-A", device)).status == LoadStatus::Valid,
	      "revision key: the same build keeps it");

	auto newer_driver = device;
	newer_driver.driver_version += 1;
	Check(LoadFile(schema_file, Signature(KeyMode::Schema, "build-B", newer_driver)).status ==
	          LoadStatus::SignatureMismatch,
	      "schema key: a driver update drops the cache");
	auto other_gpu = device;
	other_gpu.device_id += 1;
	Check(LoadFile(schema_file, Signature(KeyMode::Schema, "build-A", other_gpu)).status ==
	          LoadStatus::SignatureMismatch,
	      "schema key: another GPU drops the cache");
}

// Switching --pipeline-cache-key must never read the other mode's file as its own.
void TestKeyModesNeverCross() {
	const auto device  = Gpu();
	const auto payload = Payload();
	const auto schema_sig   = Signature(KeyMode::Schema, "r", device);
	const auto revision_sig = Signature(KeyMode::Revision, "r", device);
	Check(LoadFile(Store(schema_sig, payload), revision_sig).status == LoadStatus::SignatureMismatch,
	      "a schema file is not a revision file");
	Check(LoadFile(Store(revision_sig, payload), schema_sig).status == LoadStatus::SignatureMismatch,
	      "a revision file is not a schema file");
}

void TestDamagedFilesAreDropped() {
	const auto sig  = Signature(KeyMode::Schema, "r", Gpu());
	const auto good = Store(sig, Payload());
	Check(LoadFile(good, sig).status == LoadStatus::Valid, "baseline");

	auto flipped_payload = good;
	flipped_payload[sig.size() + sizeof(uint64_t) + 100] ^= 0x40;
	Check(LoadFile(flipped_payload, sig).status == LoadStatus::PayloadCorrupt, "a flipped payload byte");

	auto flipped_hash = good;
	flipped_hash[sig.size() + 3] ^= 0x01;
	Check(LoadFile(flipped_hash, sig).status == LoadStatus::PayloadCorrupt, "a flipped hash byte");

	auto flipped_signature = good;
	flipped_signature[10] ^= 0x01;
	Check(LoadFile(flipped_signature, sig).status == LoadStatus::SignatureMismatch, "a flipped signature byte");

	auto truncated = good;
	truncated.pop_back();
	Check(LoadFile(truncated, sig).status == LoadStatus::PayloadCorrupt, "a file cut by one byte");

	auto extended = good;
	extended.push_back(0);
	Check(LoadFile(extended, sig).status == LoadStatus::PayloadCorrupt, "a file with a stray byte");

	const std::vector<uint8_t> prefix_only(good.begin(), good.begin() + static_cast<long>(sig.size() + 4));
	Check(LoadFile(prefix_only, sig).status == LoadStatus::TooSmall, "cut inside the hash");
	Check(LoadFile({}, sig).status == LoadStatus::TooSmall, "an empty file");
}

// With the revision out of the key, entries of superseded builds are never evicted: the cap
// bounds that. The span is never read (the size check comes first), so nothing is allocated.
void TestOversizedFileIsDropped() {
	const auto sig = Signature(KeyMode::Schema, "r", Gpu());
	const size_t size = sig.size() + sizeof(uint64_t) + static_cast<size_t>(kMaxPayloadBytes) + 1;
	const std::unique_ptr<uint8_t[]> memory(new uint8_t[size]);
	Check(Load(std::span<const uint8_t>(memory.get(), size), sig).status == LoadStatus::TooLarge,
	      "a payload over the cap is dropped");
	Check(Load(std::span<const uint8_t>(memory.get(), size - 1), sig).status != LoadStatus::TooLarge,
	      "a payload exactly at the cap is not dropped for size");
}

// Build gates: the revision key needs a known, clean Release revision; the schema key works in
// every build (dev, dirty, unknown revision).
void TestBuildGates() {
	Check(!DisabledReason(KeyMode::Revision, true, "abc1234", "abc1234def").has_value(),
	      "revision key: clean release build allowed");
	Check(DisabledReason(KeyMode::Revision, false, "abc1234", "abc1234def").has_value(),
	      "revision key: non-Release disabled");
	Check(DisabledReason(KeyMode::Revision, true, "unknown", "abc1234def").has_value(),
	      "revision key: unknown hash disabled");
	Check(DisabledReason(KeyMode::Revision, true, "abc1234", "unknown").has_value(),
	      "revision key: unknown revision disabled");
	Check(DisabledReason(KeyMode::Revision, true, "abc1234-dirty", "abc1234def").has_value(),
	      "revision key: dirty build disabled");
	Check(*DisabledReason(KeyMode::Revision, true, "abc1234-dirty", "x") == "dirty build",
	      "dirty reason text");
	for (const bool release: {true, false}) {
		for (const char* hash: {"abc1234", "unknown", "abc1234-dirty"}) {
			Check(!DisabledReason(KeyMode::Schema, release, hash, "unknown").has_value(),
			      "schema key: allowed in every build");
		}
	}
}

} // namespace

int main() {
	TestSchemaSignatureIgnoresRevision();
	TestSignatureTracksTheDevice();
	TestSchemaVersionBumpDropsOldFiles();
	TestRoundTrip();
	TestAcrossBuildsAndDrivers();
	TestKeyModesNeverCross();
	TestDamagedFilesAreDropped();
	TestOversizedFileIsDropped();
	TestBuildGates();
	std::printf("PipelineCacheFileTests: ok\n");
	return 0;
}
