#include "common/slotVector.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Common::SlotId;
using Common::SlotVector;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SlotVectorTests: failed: %s\n", message);
		std::abort();
	}
}

int g_alive = 0;

struct Tracked {
	explicit Tracked(int v, std::string t = {}): value(v), text(std::move(t)) { ++g_alive; }
	Tracked(const Tracked&)            = delete;
	Tracked& operator=(const Tracked&) = delete;
	~Tracked() { --g_alive; }
	int         value;
	std::string text;
	char        padding[1500] = {}; // an Image-sized object
};

void TestInsertEraseReuse() {
	{
		SlotVector<Tracked> slots;
		const auto a = slots.insert(1, "one");
		const auto b = slots.insert(2, "two");
		const auto c = slots.insert(3, "three");
		Check(slots.size() == 3 && g_alive == 3, "three live values");
		Check(slots[a].value == 1 && slots[b].text == "two" && slots[c].value == 3, "values come back");
		Check(slots.try_get(b) == &slots[b], "try_get and operator[] agree");

		slots.erase(b);
		Check(g_alive == 2 && slots.size() == 2, "erase destroys the value");
		Check(slots.try_get(b) == nullptr && !slots.is_allocated(b), "a stale id is rejected");
		Check(slots.try_get(a) != nullptr && slots.try_get(c) != nullptr, "the others survive");

		const auto d = slots.insert(4, "four");
		Check(d.index == b.index && d.generation != b.generation, "a reused slot gets a new generation");
		Check(slots.try_get(b) == nullptr && slots[d].value == 4, "the old id still fails after the slot is reused");
		Check(slots.capacity() == 3, "the freed slot was reused, not a new one");

		int sum = 0;
		slots.ForEach([&](SlotId, Tracked& value) { sum += value.value; });
		Check(sum == 1 + 3 + 4, "ForEach visits exactly the live values");
		const auto& const_slots = slots;
		int         count       = 0;
		const_slots.ForEach([&](SlotId, const Tracked&) { ++count; });
		Check(count == 3, "const ForEach");

		Check(!slots.is_allocated(SlotId {}), "the invalid id is not allocated");
		Check(!slots.is_allocated(SlotId {999, 1}), "an index past the end is not allocated");
	}
	Check(g_alive == 0, "the destructor of the vector leaked a value");
}

void TestManySlots() {
	SlotVector<Tracked> slots;
	std::vector<SlotId> ids;
	for (int i = 0; i < 5000; ++i) {
		ids.push_back(slots.insert(i));
	}
	for (int i = 0; i < 5000; i += 3) {
		slots.erase(ids[static_cast<size_t>(i)]);
	}
	int live = 0;
	for (int i = 0; i < 5000; ++i) {
		const bool expected = i % 3 != 0;
		const auto id       = ids[static_cast<size_t>(i)];
		Check(slots.is_allocated(id) == expected, "liveness after the erasures");
		if (expected) {
			Check(slots[id].value == i, "a live value changed");
			++live;
		}
	}
	Check(slots.size() == static_cast<size_t>(live), "size after the erasures");
	const int freed = (5000 + 2) / 3; // the indices 0, 3, 6, ...
	for (int i = 0; i < freed; ++i) {
		(void)slots.insert(-i);
	}
	Check(slots.capacity() == 5000, "freed slots were not reused first");
	(void)slots.insert(-1);
	Check(slots.capacity() == 5001, "a new slot is made once the freed ones are used");
}

} // namespace

int main() {
	TestInsertEraseReuse();
	TestManySlots();
	Check(g_alive == 0, "values leaked");
	std::printf("SlotVectorTests: ok\n");
	return 0;
}
