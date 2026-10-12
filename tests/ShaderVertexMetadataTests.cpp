#include "graphics/shader/shaderVertexMetadata.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderVertexMetadataTests: failed: %s\n", text);
		std::abort();
	}
}

struct Fixture {
	std::array<uint16_t, static_cast<size_t>(AgcDirectResourceType::Last) + 1> offsets {};
	ShaderUserData                                                             user_data {};
	ShaderSemantic                                                             semantic {};
	ShaderMappedData                                                           mapped {};

	Fixture() {
		offsets.fill(AGC_ILLEGAL_DIRECT_OFFSET);
		offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)]     = 2;
		offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] = 4;
		user_data.direct_resource_offset = offsets.data();
		user_data.direct_resource_count  = static_cast<uint16_t>(offsets.size());
		mapped.user_data                 = &user_data;
		mapped.input_semantics           = &semantic;
		mapped.num_input_semantics       = 1;
	}
};

void CheckRejected(const ShaderMappedData& data, const char* text) {
	ShaderVertexMetadata output;
	std::array<ShaderSemantic, 7> prior_semantics {};
	output.vertex_buffer_reg = 37;
	output.vertex_attrib_reg = 41;
	output.input_semantics   = prior_semantics;
	std::string error;
	Check(!ShaderReadVertexMetadata(data, 64, output, &error), text);
	Check(!error.empty(), "metadata rejection omitted its diagnostic");
	Check(output.vertex_buffer_reg == 37 && output.vertex_attrib_reg == 41 &&
	          output.input_semantics.data() == prior_semantics.data() &&
	          output.input_semantics.size() == prior_semantics.size(),
	      "metadata rejection changed the prior output");
}

void TestValidAndInvalidMetadata() {
	Fixture              fixture;
	ShaderVertexMetadata output;
	std::string          error;
	Check(ShaderReadVertexMetadata(fixture.mapped, 64, output, &error),
	      "valid AGC vertex metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == 4 &&
	          output.input_semantics.size() == 1,
	      "valid AGC vertex metadata was decoded incorrectly");

	Fixture buffer_only;
	buffer_only.offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] =
	    AGC_ILLEGAL_DIRECT_OFFSET;
	buffer_only.mapped.input_semantics     = nullptr;
	buffer_only.mapped.num_input_semantics = 0;
	Check(ShaderReadVertexMetadata(buffer_only.mapped, 64, output, &error),
	      "vertex-buffer-only AGC metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == -1 &&
	          output.input_semantics.empty(),
	      "vertex-buffer-only AGC metadata was decoded incorrectly");

	Fixture attrib_only;
	attrib_only.offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)] =
	    AGC_ILLEGAL_DIRECT_OFFSET;
	CheckRejected(attrib_only.mapped, "vertex-attribute-only AGC metadata was accepted");

	auto missing_header      = fixture.mapped;
	missing_header.user_data = nullptr;
	CheckRejected(missing_header, "missing AGC user-data header was accepted");

	Fixture missing_offsets;
	missing_offsets.user_data.direct_resource_offset = nullptr;
	CheckRejected(missing_offsets.mapped, "missing direct-resource offsets were accepted");

	Fixture excessive_resources;
	excessive_resources.user_data.direct_resource_count =
	    static_cast<uint16_t>(excessive_resources.offsets.size() + 1);
	CheckRejected(excessive_resources.mapped, "excessive direct-resource count was accepted");

	Fixture excessive_semantics;
	excessive_semantics.mapped.num_input_semantics = ShaderVertexInputInfo::RES_MAX + 1;
	CheckRejected(excessive_semantics.mapped, "excessive vertex semantic count was accepted");

	Fixture excessive_register;
	excessive_register.offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)] =
	    63;
	CheckRejected(excessive_register.mapped, "out-of-domain vertex table SGPR was accepted");

	Fixture excessive_attrib_register;
	excessive_attrib_register
	    .offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] = 63;
	CheckRejected(excessive_attrib_register.mapped,
	              "out-of-domain vertex attribute table SGPR was accepted");

	Fixture missing_semantics;
	missing_semantics.mapped.input_semantics = nullptr;
	CheckRejected(missing_semantics.mapped, "missing vertex semantic array was accepted");
}

// ShaderReadGuest (--clean-guest-reads): a clean reader that accepts supplies the bytes; one that
// refuses, or none at all, leaves the plain copy.
struct FakeReaderState {
	bool     accept = false;
	uint32_t calls  = 0;
};

bool FakeReader(void* context, uint64_t, void* data, uint64_t size) {
	auto* state = static_cast<FakeReaderState*>(context);
	++state->calls;
	if (!state->accept) {
		return false;
	}
	std::memset(data, 0xcd, static_cast<size_t>(size));
	return true;
}

void TestCleanGuestReader() {
	const std::array<uint8_t, 8> source {1, 2, 3, 4, 5, 6, 7, 8};
	std::array<uint8_t, 8>       out {};

	ShaderSetCleanGuestReader(nullptr, nullptr);
	ShaderReadGuest(out.data(), source.data(), out.size());
	Check(out == source, "without a reader the copy is plain");
	Check(!ShaderTryReadGuestClean(out.data(), source.data(), out.size()),
	      "without a reader the clean path refuses");

	FakeReaderState state;
	ShaderSetCleanGuestReader(FakeReader, &state);
	out = {};
	ShaderReadGuest(out.data(), source.data(), out.size());
	Check(state.calls == 1 && out == source, "a refusing reader falls back to the plain copy");

	state.accept = true;
	out          = {};
	ShaderReadGuest(out.data(), source.data(), out.size());
	Check(state.calls == 2 && out[0] == 0xcd && out[7] == 0xcd,
	      "an accepting reader supplies the bytes");
	Check(ShaderTryReadGuestClean(out.data(), source.data(), out.size()),
	      "the clean path reports success");

	state.calls = 0;
	ShaderReadGuest(out.data(), source.data(), 0);
	Check(state.calls == 0, "a zero-size read never reaches the reader");

	ShaderSetCleanGuestReader(nullptr, nullptr);
}

} // namespace

int main() {
	TestValidAndInvalidMetadata();
	TestCleanGuestReader();
	std::puts("ShaderVertexMetadataTests: all cases passed");
	return 0;
}
