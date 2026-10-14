#ifndef KYTY_COMMON_VERIFY_RATION_H_
#define KYTY_COMMON_VERIFY_RATION_H_

// Rations the in-binary verifiers that drain the GPU (--debug-known-verify): a drain waits for the
// whole GPU backlog (about 9 ms in the level), so a verifier that ran on a count of reads instead of
// on time paid it every frame and hid the very gain it was checking (readback-gate, section 13).
// At most one verification per kind per `seconds`. Thread safe, lock free. Unit tested in
// tests/KnownStoresTests.cpp.

#include <atomic>
#include <cstdint>

namespace Common {

class VerifyRation {
public:
	// True when this caller should verify now (and claims the slot); false otherwise. seconds == 0:
	// never.
	[[nodiscard]] bool Due(int64_t now_ns, uint32_t seconds) noexcept {
		if (seconds == 0) {
			return false;
		}
		auto last = m_last_ns.load(std::memory_order_relaxed);
		while (last == kNever || now_ns - last >= static_cast<int64_t>(seconds) * 1'000'000'000) {
			if (m_last_ns.compare_exchange_weak(last, now_ns, std::memory_order_relaxed)) {
				return true;
			}
		}
		return false;
	}

private:
	static constexpr int64_t kNever = INT64_MIN;
	std::atomic<int64_t>     m_last_ns {kNever};
};

} // namespace Common

#endif // KYTY_COMMON_VERIFY_RATION_H_
