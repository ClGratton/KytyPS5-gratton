#include "common/abi.h"
#include "libs/errno.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Only the metadata provider is replaced: PlayGoGetChunkId and PlayGoOpen are the production
// guest APIs.
namespace Loader {
uint32_t g_test_chunks = 0;
bool SystemContentGetChunksNum(uint32_t* count) {
	*count = g_test_chunks;
	return true;
}
} // namespace Loader

namespace Libs::PlayGo {
int KYTY_SYSV_ABI PlayGoOpen(int* out_handle, const void* param);
int KYTY_SYSV_ABI PlayGoGetChunkId(int handle, uint16_t* out_chunk_id_list,
                                   uint32_t number_of_entries, uint32_t* out_entries);
} // namespace Libs::PlayGo

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PlayGoApiTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	using namespace Libs;
	using namespace Libs::PlayGo;
	Loader::g_test_chunks = 3;
	int handle            = 0;
	Check(PlayGoOpen(&handle, nullptr) == OK, "open failed");

	// Count pass of the two-pass enumerate pattern: a null list returns the total, whatever the
	// capacity the caller passes.
	uint32_t count = 0xffffffffu;
	Check(PlayGoGetChunkId(handle, nullptr, 0, &count) == OK && count == 3u,
	      "count pass with capacity 0 did not return the total");
	count = 0xffffffffu;
	Check(PlayGoGetChunkId(handle, nullptr, 1, &count) == OK && count == 3u,
	      "count pass with capacity 1 did not return the total");

	// Fill pass.
	std::vector<uint16_t> ids(4, 0xffff);
	Check(PlayGoGetChunkId(handle, ids.data(), 4, &count) == OK && count == 3u,
	      "fill pass did not return the total");
	Check(ids[0] == 0 && ids[1] == 1 && ids[2] == 2 && ids[3] == 0xffff,
	      "fill pass wrote the wrong ids or past the chunks");
	uint16_t two[] = {0xffff, 0xffff, 0xbeef};
	Check(PlayGoGetChunkId(handle, two, 2, &count) == OK && count == 2u &&
	          two[0] == 0 && two[1] == 1 && two[2] == 0xbeef,
	      "capacity-limited fill overflowed or miscounted");
	Check(PlayGoGetChunkId(handle, nullptr, 0, nullptr) == PLAYGO_ERROR_BAD_POINTER,
	      "null out_entries accepted");
	std::puts("PlayGoApiTests: all cases passed");
	return 0;
}
