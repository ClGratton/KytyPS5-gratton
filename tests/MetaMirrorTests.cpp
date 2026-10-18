#include "graphics/host_gpu/renderer/cache/metaMirror.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <thread>
#include <vector>

namespace {

using Libs::Graphics::MetaMirror;
using Found = MetaMirror::Found;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "MetaMirrorTests: failed: %s\n", message);
		std::abort();
	}
}

Found Look(const MetaMirror& m, uint64_t address, uint32_t* mask) { return m.Lookup(address, mask); }

void TestBasic() {
	MetaMirror m;
	uint32_t   mask = 0;
	Check(Look(m, 0x100000, &mask) == Found::Absent, "empty table: absent");
	Check(Look(m, 0, &mask) == Found::Unknown, "address 0 is not a key");
	Check(m.Set(0x100000, 0xffffffffu), "insert");
	Check(Look(m, 0x100000, &mask) == Found::Present && mask == 0xffffffffu, "present with its mask");
	Check(Look(m, 0x100100, &mask) == Found::Absent, "another address is absent");
	Check(m.Set(0x100000, 0x6u), "update in place");
	Check(Look(m, 0x100000, &mask) == Found::Present && mask == 6u, "updated mask");
	Check(m.Used() == 1, "one entry");
	m.Erase(0x100000);
	Check(Look(m, 0x100000, &mask) == Found::Absent && m.Used() == 0, "erased");
	m.Erase(0x100000); // erasing again is harmless
	Check(Look(m, 0x100000, &mask) == Found::Absent, "still absent");
}

// A table with many colliding keys: every key stays findable through inserts and erases (backward-shift deletion).
void TestAgainstMap() {
	MetaMirror                      m;
	std::map<uint64_t, uint32_t>    ref;
	std::mt19937_64                 rng(12345);
	for (int step = 0; step < 200000; step++) {
		// A small address pool, many of them in the same 16-byte units so they collide often.
		const uint64_t address = 0x10000 + (rng() % 900) * 0x10 + (rng() % 3 == 0 ? 0x100000 : 0);
		switch (rng() % 4) {
			case 0:
			case 1: {
				const uint32_t mask = static_cast<uint32_t>(rng());
				Check(m.Set(address, mask), "set");
				ref[address] = mask;
				break;
			}
			case 2:
				m.Erase(address);
				ref.erase(address);
				break;
			default: break;
		}
		if (step % 7 == 0) {
			uint32_t   mask  = 0;
			const auto found = Look(m, address, &mask);
			const auto it    = ref.find(address);
			if (it == ref.end()) {
				Check(found == Found::Absent, "absent matches the map");
			} else {
				Check(found == Found::Present && mask == it->second, "present matches the map");
			}
		}
	}
	size_t seen = 0;
	m.ForEach([&](uint64_t address, uint32_t mask) {
		const auto it = ref.find(address);
		Check(it != ref.end() && it->second == mask, "table content is in the map");
		seen++;
	});
	Check(seen == ref.size() && m.Used() == ref.size(), "table content equals the map");
	for (const auto& [address, mask]: ref) {
		uint32_t   got = 0;
		Check(Look(m, address, &got) == Found::Present && got == mask, "every mapped key is found");
	}
}

void TestFull() {
	MetaMirror m;
	for (uint64_t i = 0; i < MetaMirror::kMaxEntries; i++) {
		Check(m.Set(0x1000000 + i * 0x40, static_cast<uint32_t>(i)), "fits");
	}
	Check(m.Complete(), "complete while it fits");
	Check(!m.Set(0x9000000, 1), "the entry past the limit is refused");
	uint32_t mask = 0;
	Check(!m.Complete() && Look(m, 0x1000000, &mask) == Found::Unknown, "an incomplete table answers unknown");
}

// One writer inserts and erases other keys (moving entries) and flips a stable key's mask between two values while readers
// look the stable keys up: they are never absent, and always have one of the two masks.
void TestConcurrentReaders() {
	MetaMirror         m;
	std::atomic<bool>  stop {false};
	std::atomic<uint64_t> wrong {0}, unknown {0}, asked {0};
	std::vector<uint64_t> stable;
	for (int i = 0; i < 64; i++) {
		stable.push_back(0x2000000 + static_cast<uint64_t>(i) * 0x230);
		m.Set(stable.back(), 0xA);
	}
	std::vector<std::thread> readers;
	for (int t = 0; t < 3; t++) {
		readers.emplace_back([&, t] {
			uint64_t i = static_cast<uint64_t>(t);
			while (!stop.load()) {
				uint32_t   mask  = 0;
				const auto found = m.Lookup(stable[i++ % stable.size()], &mask);
				asked.fetch_add(1);
				if (found == Found::Unknown) {
					unknown.fetch_add(1);
				} else if (found != Found::Present || (mask != 0xA && mask != 0xB)) {
					wrong.fetch_add(1);
				}
			}
		});
	}
	std::mt19937_64 rng(7);
	for (int i = 0; i < 100000; i++) {
		const uint64_t churn = 0x3000000 + (rng() % 400) * 0x230;
		if ((i & 1) != 0) {
			m.Set(churn, 1);
		} else {
			m.Erase(churn);
		}
		m.Set(stable[static_cast<size_t>(i) % stable.size()], (i & 2) != 0 ? 0xB : 0xA);
	}
	stop.store(true);
	for (auto& r: readers) r.join();
	Check(wrong.load() == 0, "a stable key is never absent and never has a mask it never had");
	Check(asked.load() > 0, "readers ran");
	std::printf("MetaMirrorTests: %llu lookups, %llu unknown\n", static_cast<unsigned long long>(asked.load()),
	            static_cast<unsigned long long>(unknown.load()));
}

} // namespace

int main() {
	TestBasic();
	TestAgainstMap();
	TestFull();
	TestConcurrentReaders();
	std::printf("MetaMirrorTests: all passed\n");
	return 0;
}
