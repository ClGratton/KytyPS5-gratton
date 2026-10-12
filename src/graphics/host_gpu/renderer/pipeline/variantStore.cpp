#include "graphics/host_gpu/renderer/pipeline/variantStore.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace Libs::Graphics::Variants {

namespace {

constexpr char     kMagic[8]   = {'K', 'Y', 'T', 'Y', 'V', 'A', 'R', '1'};
constexpr size_t   kHeaderSize = sizeof(kMagic) + sizeof(uint32_t);
constexpr uint8_t  kKindCompute = 1;
constexpr uint8_t  kKindGraphics = 2;
constexpr uint32_t kMaxStaticWords = 1024;
constexpr uint32_t kMaxBlob = 1u << 17;
constexpr uint32_t kMaxPayload  = 1u << 20;

class Writer {
public:
	void U8(uint8_t value) { m_bytes.push_back(value); }
	void U32(uint32_t value) {
		for (uint32_t i = 0; i < 4; i++) m_bytes.push_back(static_cast<uint8_t>(value >> (8u * i)));
	}
	void U64(uint64_t value) {
		U32(static_cast<uint32_t>(value));
		U32(static_cast<uint32_t>(value >> 32u));
	}
	std::vector<uint8_t> Take() { return std::move(m_bytes); }

private:
	std::vector<uint8_t> m_bytes;
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t> bytes): m_bytes(bytes) {}
	bool U8(uint8_t& value) {
		if (m_pos + 1 > m_bytes.size()) return false;
		value = m_bytes[m_pos++];
		return true;
	}
	bool U32(uint32_t& value) {
		if (m_pos + 4 > m_bytes.size()) return false;
		value = 0;
		for (uint32_t i = 0; i < 4; i++) value |= static_cast<uint32_t>(m_bytes[m_pos + i]) << (8u * i);
		m_pos += 4;
		return true;
	}
	bool U64(uint64_t& value) {
		uint32_t lo = 0, hi = 0;
		if (!U32(lo) || !U32(hi)) return false;
		value = lo | (static_cast<uint64_t>(hi) << 32u);
		return true;
	}
	[[nodiscard]] bool AtEnd() const { return m_pos == m_bytes.size(); }

private:
	std::span<const uint8_t> m_bytes;
	size_t                   m_pos = 0;
};

} // namespace

uint32_t Checksum(std::span<const uint8_t> bytes) {
	uint32_t hash = 2166136261u; // FNV-1a
	for (const auto byte: bytes) {
		hash ^= byte;
		hash *= 16777619u;
	}
	return hash;
}

std::vector<uint8_t> Encode(const ComputeVariant& variant) {
	Writer out;
	out.U8(kKindCompute);
	out.U64(variant.code_hash);
	out.U32(variant.code_size_dwords);
	const auto& st = variant.state;
	for (const uint32_t value: {st.user_data_count, st.workgroup_register, st.wave_size, st.float_mode,
	                            st.host_subgroup_size, st.thread_ids_num, st.lds_size_dwords,
	                            st.scratch_size_dwords, st.threads_num[0], st.threads_num[1],
	                            st.threads_num[2], st.flags}) {
		out.U32(value);
	}
	out.U32(static_cast<uint32_t>(variant.spec.buffers.size()));
	for (const auto& b: variant.spec.buffers) {
		for (const uint32_t value: {b.packed_stride, b.descriptor_format, b.descriptor_swizzle,
		                            b.zero_stride_oob, b.indirect_root, b.indirect_mapping_offset,
		                            b.indirect_search_iterations}) {
			out.U32(value);
		}
	}
	out.U32(static_cast<uint32_t>(variant.spec.images.size()));
	for (const auto& i: variant.spec.images) {
		for (const uint32_t value: {i.numeric_class, i.dimension, i.mip_count, i.conversion_format,
		                            i.shader_swizzle, i.indirect_root, i.indirect_mapping_offset,
		                            i.indirect_search_iterations, i.flags}) {
			out.U32(value);
		}
	}
	out.U32(variant.first_use_ms);
	return out.Take();
}

bool Decode(std::span<const uint8_t> payload, ComputeVariant& variant) {
	Reader   in(payload);
	uint8_t  kind = 0;
	if (!in.U8(kind) || kind != kKindCompute) return false;
	ComputeVariant result;
	if (!in.U64(result.code_hash) || !in.U32(result.code_size_dwords)) return false;
	auto& st = result.state;
	for (uint32_t* field: {&st.user_data_count, &st.workgroup_register, &st.wave_size, &st.float_mode,
	                       &st.host_subgroup_size, &st.thread_ids_num, &st.lds_size_dwords,
	                       &st.scratch_size_dwords, &st.threads_num[0], &st.threads_num[1],
	                       &st.threads_num[2], &st.flags}) {
		if (!in.U32(*field)) return false;
	}
	uint32_t count = 0;
	if (!in.U32(count) || count > kMaxResources) return false;
	result.spec.buffers.resize(count);
	for (auto& b: result.spec.buffers) {
		for (uint32_t* field: {&b.packed_stride, &b.descriptor_format, &b.descriptor_swizzle,
		                       &b.zero_stride_oob, &b.indirect_root, &b.indirect_mapping_offset,
		                       &b.indirect_search_iterations}) {
			if (!in.U32(*field)) return false;
		}
	}
	if (!in.U32(count) || count > kMaxResources) return false;
	result.spec.images.resize(count);
	for (auto& i: result.spec.images) {
		for (uint32_t* field: {&i.numeric_class, &i.dimension, &i.mip_count, &i.conversion_format,
		                       &i.shader_swizzle, &i.indirect_root, &i.indirect_mapping_offset,
		                       &i.indirect_search_iterations, &i.flags}) {
			if (!in.U32(*field)) return false;
		}
	}
	// Optional trailing field (files written before it existed end here).
	if (!in.AtEnd()) {
		if (!in.U32(result.first_use_ms) || !in.AtEnd()) return false;
	}
	variant = std::move(result);
	return true;
}

namespace {

void WriteSpec(Writer& out, const SpecRecord& spec) {
	out.U32(static_cast<uint32_t>(spec.buffers.size()));
	for (const auto& b: spec.buffers) {
		for (const uint32_t value: {b.packed_stride, b.descriptor_format, b.descriptor_swizzle,
		                            b.zero_stride_oob, b.indirect_root, b.indirect_mapping_offset,
		                            b.indirect_search_iterations}) {
			out.U32(value);
		}
	}
	out.U32(static_cast<uint32_t>(spec.images.size()));
	for (const auto& i: spec.images) {
		for (const uint32_t value: {i.numeric_class, i.dimension, i.mip_count, i.conversion_format,
		                            i.shader_swizzle, i.indirect_root, i.indirect_mapping_offset,
		                            i.indirect_search_iterations, i.flags}) {
			out.U32(value);
		}
	}
}

bool ReadSpec(Reader& in, SpecRecord& spec) {
	uint32_t count = 0;
	if (!in.U32(count) || count > kMaxResources) return false;
	spec.buffers.resize(count);
	for (auto& b: spec.buffers) {
		for (uint32_t* field: {&b.packed_stride, &b.descriptor_format, &b.descriptor_swizzle,
		                       &b.zero_stride_oob, &b.indirect_root, &b.indirect_mapping_offset,
		                       &b.indirect_search_iterations}) {
			if (!in.U32(*field)) return false;
		}
	}
	if (!in.U32(count) || count > kMaxResources) return false;
	spec.images.resize(count);
	for (auto& i: spec.images) {
		for (uint32_t* field: {&i.numeric_class, &i.dimension, &i.mip_count, &i.conversion_format,
		                       &i.shader_swizzle, &i.indirect_root, &i.indirect_mapping_offset,
		                       &i.indirect_search_iterations, &i.flags}) {
			if (!in.U32(*field)) return false;
		}
	}
	return true;
}

void WriteBytes(Writer& out, const std::vector<uint8_t>& bytes) {
	out.U32(static_cast<uint32_t>(bytes.size()));
	for (const auto byte: bytes) out.U8(byte);
}

bool ReadBytes(Reader& in, std::vector<uint8_t>& bytes) {
	uint32_t size = 0;
	if (!in.U32(size) || size > kMaxBlob) return false;
	bytes.resize(size);
	for (auto& byte: bytes) {
		if (!in.U8(byte)) return false;
	}
	return true;
}

void WriteProgram(Writer& out, const ProgramVariant& p) {
	out.U8(p.stage);
	out.U64(p.code_hash);
	out.U32(p.code_size_dwords);
	out.U32(p.user_data_count);
	out.U32(p.push_data_cursor);
	out.U32(static_cast<uint32_t>(p.static_state.size()));
	for (const auto word: p.static_state) out.U32(word);
	WriteSpec(out, p.spec);
	WriteBytes(out, p.input);
	out.U32(p.input_layout);
	out.U64(p.back_hash);
	out.U32(p.back_size_dwords);
	out.U64(p.front_hash);
}

bool ReadProgram(Reader& in, ProgramVariant& p) {
	uint32_t words = 0;
	if (!in.U8(p.stage) || !in.U64(p.code_hash) || !in.U32(p.code_size_dwords) ||
	    !in.U32(p.user_data_count) || !in.U32(p.push_data_cursor) || !in.U32(words) ||
	    words > kMaxStaticWords) {
		return false;
	}
	p.static_state.resize(words);
	for (auto& word: p.static_state) {
		if (!in.U32(word)) return false;
	}
	return ReadSpec(in, p.spec) && ReadBytes(in, p.input) && in.U32(p.input_layout) &&
	       in.U64(p.back_hash) && in.U32(p.back_size_dwords) && in.U64(p.front_hash);
}

} // namespace

std::vector<uint8_t> Encode(const GraphicsVariant& variant) {
	Writer out;
	out.U8(kKindGraphics);
	out.U8(variant.has_ps ? 1 : 0);
	WriteProgram(out, variant.vs);
	if (variant.has_ps) WriteProgram(out, variant.ps);
	WriteBytes(out, variant.key);
	out.U32(variant.first_use_ms);
	return out.Take();
}

bool Decode(std::span<const uint8_t> payload, GraphicsVariant& variant) {
	Reader  in(payload);
	uint8_t kind = 0, has_ps = 0;
	if (!in.U8(kind) || kind != kKindGraphics || !in.U8(has_ps) || has_ps > 1) return false;
	GraphicsVariant result;
	result.has_ps = has_ps != 0;
	if (!ReadProgram(in, result.vs)) return false;
	if (result.has_ps && !ReadProgram(in, result.ps)) return false;
	if (!ReadBytes(in, result.key)) return false;
	if (!in.AtEnd()) {
		if (!in.U32(result.first_use_ms) || !in.AtEnd()) return false;
	}
	variant = std::move(result);
	return true;
}

VariantStore::~VariantStore() {
	Close();
}

bool VariantStore::AddLocked(const ComputeVariant& variant) {
	auto& list = m_by_binary[variant.BinaryKey()];
	if (std::find(list.begin(), list.end(), variant) != list.end()) {
		return false;
	}
	list.push_back(variant);
	m_size++;
	return true;
}

VariantStore::LoadResult VariantStore::Load(const std::filesystem::path& path) {
	std::lock_guard lock(m_mutex);
	LoadResult      result;
	m_by_binary.clear();
	m_graphics.clear();
	m_size       = 0;
	m_path       = path;
	m_good_bytes = 0;
	m_schema_ok  = true;

	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return result;
	}
	result.file_found = true;
	const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (data.size() < kHeaderSize || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
		result.schema_ok = false;
		m_schema_ok      = false;
		return result;
	}
	uint32_t schema = 0;
	{
		Reader header(std::span<const uint8_t>(data).subspan(sizeof(kMagic), 4));
		(void)header.U32(schema);
	}
	if (schema != kSchema) {
		result.schema_ok = false;
		m_schema_ok      = false;
		return result;
	}
	size_t pos = kHeaderSize;
	for (;;) {
		Reader   size_reader(std::span<const uint8_t>(data).subspan(pos));
		uint32_t payload_size = 0;
		if (!size_reader.U32(payload_size) || payload_size == 0 || payload_size > kMaxPayload ||
		    pos + 4 + payload_size + 4 > data.size()) {
			break;
		}
		const auto payload = std::span<const uint8_t>(data).subspan(pos + 4, payload_size);
		Reader     sum_reader(std::span<const uint8_t>(data).subspan(pos + 4 + payload_size, 4));
		uint32_t   sum = 0;
		(void)sum_reader.U32(sum);
		bool added = false;
		if (sum != Checksum(payload)) break;
		if (payload[0] == kKindGraphics) {
			GraphicsVariant variant;
			if (!Decode(payload, variant)) break;
			added = AddLocked(variant);
		} else {
			ComputeVariant variant;
			if (!Decode(payload, variant)) break;
			added = AddLocked(variant);
		}
		result.records++;
		if (added) result.added++;
		pos += 4 + payload_size + 4;
	}
	result.good_bytes = pos;
	result.torn_tail  = pos < data.size();
	m_good_bytes      = pos;
	return result;
}

bool VariantStore::OpenAppend(const std::filesystem::path& path) {
	std::lock_guard lock(m_mutex);
	if (m_file != nullptr) {
		return true;
	}
	if (path != m_path) {
		// Load() was not called for this file: start from an empty store, keep nothing on disk.
		m_path       = path;
		m_good_bytes = 0;
		m_schema_ok  = false;
	}
	std::error_code ec;
	if (m_schema_ok && m_good_bytes >= kHeaderSize && std::filesystem::exists(path, ec)) {
		// Drop a torn tail so the next record does not land behind garbage.
		if (std::filesystem::file_size(path, ec) != m_good_bytes) {
			std::filesystem::resize_file(path, m_good_bytes, ec);
			if (ec) return false;
		}
		m_file = std::fopen(path.string().c_str(), "ab");
		return m_file != nullptr;
	}
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), ec);
	}
	m_file = std::fopen(path.string().c_str(), "wb");
	if (m_file == nullptr) {
		return false;
	}
	std::fwrite(kMagic, 1, sizeof(kMagic), m_file);
	Writer header;
	header.U32(kSchema);
	const auto bytes = header.Take();
	std::fwrite(bytes.data(), 1, bytes.size(), m_file);
	std::fflush(m_file);
	m_good_bytes = kHeaderSize;
	m_schema_ok  = true;
	// An older file with another schema was replaced: forget what it held (nothing was loaded).
	return true;
}

void VariantStore::Close() {
	std::lock_guard lock(m_mutex);
	if (m_file != nullptr) {
		std::fclose(m_file);
		m_file = nullptr;
	}
}

bool VariantStore::AddLocked(const GraphicsVariant& variant) {
	if (std::find(m_graphics.begin(), m_graphics.end(), variant) != m_graphics.end()) {
		return false;
	}
	m_graphics.push_back(variant);
	m_size++;
	return true;
}

// One write call per record: a crash leaves at most one torn record at the end.
bool VariantStore::AppendLocked(const std::vector<uint8_t>& payload) {
	if (m_file == nullptr) return false;
	Writer record;
	record.U32(static_cast<uint32_t>(payload.size()));
	auto bytes = record.Take();
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	Writer sum;
	sum.U32(Checksum(payload));
	const auto tail = sum.Take();
	bytes.insert(bytes.end(), tail.begin(), tail.end());
	std::fwrite(bytes.data(), 1, bytes.size(), m_file);
	std::fflush(m_file);
	m_good_bytes += bytes.size();
	return true;
}

bool VariantStore::Add(const ComputeVariant& variant) {
	std::lock_guard lock(m_mutex);
	if (!AddLocked(variant)) {
		return false;
	}
	AppendLocked(Encode(variant));
	return true;
}

bool VariantStore::Add(const GraphicsVariant& variant) {
	std::lock_guard lock(m_mutex);
	if (!AddLocked(variant)) {
		return false;
	}
	AppendLocked(Encode(variant));
	return true;
}

bool VariantStore::Contains(const GraphicsVariant& variant) const {
	std::lock_guard lock(m_mutex);
	return std::find(m_graphics.begin(), m_graphics.end(), variant) != m_graphics.end();
}

std::vector<GraphicsVariant> VariantStore::Graphics() const {
	std::lock_guard lock(m_mutex);
	return m_graphics;
}

size_t VariantStore::GraphicsSize() const {
	std::lock_guard lock(m_mutex);
	return m_graphics.size();
}

bool VariantStore::Contains(const ComputeVariant& variant) const {
	std::lock_guard lock(m_mutex);
	const auto      iter = m_by_binary.find(variant.BinaryKey());
	return iter != m_by_binary.end() &&
	       std::find(iter->second.begin(), iter->second.end(), variant) != iter->second.end();
}

std::vector<ComputeVariant> VariantStore::Find(uint64_t code_hash, uint32_t code_size_dwords) const {
	std::lock_guard lock(m_mutex);
	std::vector<ComputeVariant> result;
	const auto iter = m_by_binary.find(ComputeVariant::BinaryKey(code_hash, code_size_dwords));
	if (iter == m_by_binary.end()) {
		return result;
	}
	for (const auto& variant: iter->second) {
		if (variant.code_hash == code_hash && variant.code_size_dwords == code_size_dwords) {
			result.push_back(variant);
		}
	}
	return result;
}

size_t VariantStore::Size() const {
	std::lock_guard lock(m_mutex);
	return m_size;
}

} // namespace Libs::Graphics::Variants
