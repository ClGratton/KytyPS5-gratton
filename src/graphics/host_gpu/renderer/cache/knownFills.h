#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNFILLS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNFILLS_H_

// Pure bookkeeping for --known-fills (K22): which guest ranges were last written, on the GPU, by
// one uniform dword fill, and with which value. Unit tested in tests/KnownFillsTests.cpp; the
// buffer cache owns one instance and a mutex around it.
//
// Titles clear color-surface metadata (DCC/CMask keys) with compute shaders that the recompiler
// already recognises as uniform buffer fills. The emulator then needs the value on the CPU to turn
// the clear into an image clear, and read the metadata back for it: a full GPU drain several times
// a frame. A range whose last writer was such a fill has a known value, so no readback is needed.
// Any other write (another GPU write, a CPU write, a different fill) forgets the overlapped part.
//
// Ported from Hamza Shakir's commit 9cfd0569f (in ogeidexe/KytyPS5), GPL-2.0; re-implemented as a
// separate pure class with tests.

#include <cstdint>
#include <iterator>
#include <map>

namespace Libs::Graphics {

class KnownFills {
public:
	// [address, address + size) was just filled with `value` (one dword repeated).
	void Record(uint64_t address, uint64_t size, uint32_t value) {
		if (size == 0) {
			return;
		}
		Forget(address, size);
		m_fills[address] = Fill {address + size, value};
	}

	// Some other write touched [address, address + size): the overlapped part is unknown again,
	// the rest of a longer fill stays known.
	void Forget(uint64_t address, uint64_t size) {
		if (size == 0 || m_fills.empty()) {
			return;
		}
		const uint64_t end = address + size;
		auto           it  = m_fills.lower_bound(address);
		if (it != m_fills.begin() && std::prev(it)->second.end > address) {
			--it;
		}
		while (it != m_fills.end() && it->first < end) {
			const uint64_t start = it->first;
			const Fill     fill  = it->second;
			it                   = m_fills.erase(it);
			if (start < address) {
				m_fills.emplace(start, Fill {address, fill.value});
			}
			if (fill.end > end) {
				it = m_fills.emplace(end, Fill {fill.end, fill.value}).first;
				++it;
			}
		}
	}

	// True when the whole of [address, address + size) was last written by one fill; `value` is it.
	[[nodiscard]] bool TryGet(uint64_t address, uint64_t size, uint32_t* value) const {
		if (size == 0 || m_fills.empty()) {
			return false;
		}
		auto it = m_fills.upper_bound(address);
		if (it == m_fills.begin()) {
			return false;
		}
		--it;
		if (it->second.end < address + size) {
			return false;
		}
		*value = it->second.value;
		return true;
	}

	[[nodiscard]] size_t Count() const { return m_fills.size(); }
	void                 Clear() { m_fills.clear(); }

private:
	struct Fill {
		uint64_t end   = 0;
		uint32_t value = 0;
	};
	std::map<uint64_t, Fill> m_fills; // by start address; ranges never overlap
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNFILLS_H_
