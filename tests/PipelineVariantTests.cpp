// Unit tests for the persisted variant store (--pipeline-variants-replay,
// src/graphics/host_gpu/renderer/pipeline/variantStore.*): record encoding, the file format, torn
// and corrupt tails, schema changes, de-duplication and the per-binary lookup. No Vulkan.

#include "graphics/host_gpu/renderer/pipeline/variantStore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Libs::Graphics::Variants;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PipelineVariantTests: failed: %s\n", message);
		std::abort();
	}
}

ComputeVariant MakeVariant(uint64_t hash, uint32_t size, uint32_t lds, uint32_t stride) {
	ComputeVariant v;
	v.code_hash        = hash;
	v.code_size_dwords = size;
	v.state.user_data_count    = 6;
	v.state.workgroup_register = 6;
	v.state.wave_size          = 64;
	v.state.float_mode         = 0xc0;
	v.state.host_subgroup_size = 32;
	v.state.thread_ids_num     = 3;
	v.state.lds_size_dwords    = lds;
	v.state.threads_num[0]     = 8;
	v.state.threads_num[1]     = 8;
	v.state.threads_num[2]     = 1;
	v.state.flags              = 0x9;
	v.spec.buffers.push_back({.packed_stride = stride, .zero_stride_oob = stride == 0 ? 1u : 0u});
	v.spec.buffers.push_back({.packed_stride = 16, .descriptor_format = 5, .descriptor_swizzle = 0x4567,
	                          .indirect_root = 1, .indirect_mapping_offset = 3,
	                          .indirect_search_iterations = 9});
	v.spec.images.push_back({.numeric_class = 2, .dimension = 3, .mip_count = 4, .flags = 1});
	return v;
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void TestEncodeRoundTrip() {
	const auto variant = MakeVariant(0x1122334455667788ull, 321, 512, 0);
	const auto bytes   = Encode(variant);
	ComputeVariant decoded;
	Check(Decode(bytes, decoded), "decode of an encoded variant");
	Check(decoded == variant, "round trip keeps every field");
	// Any truncation or extra byte is rejected.
	for (size_t cut = 0; cut < bytes.size(); cut++) {
		ComputeVariant partial;
		Check(!Decode(std::span<const uint8_t>(bytes).first(cut), partial), "truncated payload rejected");
	}
	auto longer = bytes;
	longer.push_back(0);
	ComputeVariant extra;
	Check(!Decode(longer, extra), "payload with trailing bytes rejected");
	// An absurd resource count is rejected before any allocation.
	auto huge = bytes;
	const size_t buffers_count_offset = 1 + 8 + 4 + 12 * 4;
	huge[buffers_count_offset + 1] = 0xff;
	ComputeVariant big;
	Check(!Decode(huge, big), "oversized resource count rejected");
}

void TestStoreFileRoundTrip(const std::filesystem::path& dir) {
	const auto path = dir / "roundtrip.variants";
	std::filesystem::remove(path);
	{
		VariantStore store;
		const auto   loaded = store.Load(path);
		Check(!loaded.file_found && loaded.records == 0, "missing file loads as empty");
		Check(store.OpenAppend(path), "create the file");
		Check(store.Add(MakeVariant(1, 100, 0, 0)), "first variant is new");
		Check(store.Add(MakeVariant(1, 100, 512, 0)), "same binary, other LDS, is new");
		Check(store.Add(MakeVariant(2, 100, 0, 16)), "other binary is new");
		Check(!store.Add(MakeVariant(1, 100, 0, 0)), "an equal variant is a duplicate");
		Check(store.Size() == 3, "three distinct variants");
	}
	VariantStore again;
	const auto   loaded = again.Load(path);
	Check(loaded.file_found && loaded.schema_ok && loaded.records == 3 && loaded.added == 3 &&
	          !loaded.torn_tail,
	      "reload sees the three records");
	Check(again.Find(1, 100).size() == 2 && again.Find(2, 100).size() == 1, "lookup by binary");
	Check(again.Find(1, 101).empty() && again.Find(3, 100).empty(), "lookup needs hash and size");
	Check(again.Contains(MakeVariant(2, 100, 0, 16)) && !again.Contains(MakeVariant(2, 100, 0, 32)),
	      "contains compares the whole variant");
	// Appending after a reload keeps the old records and de-duplicates against them.
	Check(again.OpenAppend(path), "reopen for append");
	Check(!again.Add(MakeVariant(1, 100, 0, 0)), "duplicate of a loaded record");
	Check(again.Add(MakeVariant(3, 7, 0, 4)), "new record after reload");
	again.Close();
	VariantStore third;
	Check(third.Load(path).records == 4, "four records after the second session");
}

void TestTornTail(const std::filesystem::path& dir) {
	const auto path = dir / "torn.variants";
	std::filesystem::remove(path);
	{
		VariantStore store;
		store.Load(path);
		Check(store.OpenAppend(path), "create");
		store.Add(MakeVariant(10, 50, 0, 0));
		store.Add(MakeVariant(11, 50, 0, 0));
	}
	auto bytes = ReadAll(path);
	const size_t good = bytes.size();
	// A crash while appending: half of a third record.
	const auto extra = Encode(MakeVariant(12, 50, 0, 0));
	std::vector<uint8_t> record = {static_cast<uint8_t>(extra.size()), static_cast<uint8_t>(extra.size() >> 8), 0, 0};
	record.insert(record.end(), extra.begin(), extra.begin() + static_cast<std::ptrdiff_t>(extra.size() / 2));
	bytes.insert(bytes.end(), record.begin(), record.end());
	WriteAll(path, bytes);

	VariantStore store;
	const auto   loaded = store.Load(path);
	Check(loaded.records == 2 && loaded.good_bytes == good && loaded.torn_tail,
	      "torn tail is reported and not read");
	Check(store.OpenAppend(path), "append after a torn tail");
	Check(std::filesystem::file_size(path) == good, "the torn bytes were truncated");
	Check(store.Add(MakeVariant(13, 50, 0, 0)), "append works");
	store.Close();
	VariantStore after;
	Check(after.Load(path).records == 3 && !after.Load(path).torn_tail,
	      "all three good records are readable after the repair");
}

void TestCorruptRecordEndsFile(const std::filesystem::path& dir) {
	const auto path = dir / "corrupt.variants";
	std::filesystem::remove(path);
	{
		VariantStore store;
		store.Load(path);
		store.OpenAppend(path);
		store.Add(MakeVariant(20, 50, 0, 0));
		store.Add(MakeVariant(21, 50, 0, 0));
		store.Add(MakeVariant(22, 50, 0, 0));
	}
	auto bytes = ReadAll(path);
	// Flip one byte inside the second record's payload.
	const size_t first_record = 12;
	const uint32_t first_size = bytes[first_record] | (bytes[first_record + 1] << 8);
	const size_t second_payload = first_record + 4 + first_size + 4 + 4;
	bytes[second_payload + 5] ^= 0x40;
	WriteAll(path, bytes);
	VariantStore store;
	const auto   loaded = store.Load(path);
	Check(loaded.records == 1 && loaded.torn_tail, "a record failing its checksum ends the readable file");
	Check(!store.Find(20, 50).empty() && store.Find(21, 50).empty() && store.Find(22, 50).empty(),
	      "only the records before the damage are used");
}

void TestSchemaAndGarbage(const std::filesystem::path& dir) {
	const auto path = dir / "schema.variants";
	{
		VariantStore store;
		store.Load(path);
		store.OpenAppend(path);
		store.Add(MakeVariant(30, 50, 0, 0));
	}
	auto bytes = ReadAll(path);
	bytes[8] = 99; // schema 99
	WriteAll(path, bytes);
	{
		VariantStore store;
		const auto   loaded = store.Load(path);
		Check(loaded.file_found && !loaded.schema_ok && loaded.records == 0 && store.Size() == 0,
		      "another schema is ignored");
		Check(store.OpenAppend(path), "the file is replaced");
		Check(store.Add(MakeVariant(31, 50, 0, 0)), "new schema record");
	}
	VariantStore replaced;
	const auto   loaded = replaced.Load(path);
	Check(loaded.schema_ok && loaded.records == 1 && !replaced.Find(31, 50).empty(),
	      "the replaced file has the current schema and only the new record");

	const auto junk = dir / "junk.variants";
	WriteAll(junk, std::vector<uint8_t>(100, 0x5a));
	VariantStore junk_store;
	Check(!junk_store.Load(junk).schema_ok && junk_store.Size() == 0, "garbage is not a variant file");
	Check(junk_store.OpenAppend(junk) && junk_store.Add(MakeVariant(32, 50, 0, 0)), "garbage is replaced");
}

} // namespace

int main() {
	const auto dir = std::filesystem::temp_directory_path() / "kyty_variant_tests";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	TestEncodeRoundTrip();
	TestStoreFileRoundTrip(dir);
	TestTornTail(dir);
	TestCorruptRecordEndsFile(dir);
	TestSchemaAndGarbage(dir);
	std::filesystem::remove_all(dir);
	std::printf("PipelineVariantTests: ok\n");
	return 0;
}
