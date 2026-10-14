// Guest retirement tracker (src/common/guestRetirement.h): when may the guest have reused the
// memory of a submission? Only after a completion it can observe, never merely because the GPU
// thread started the submission.

#include "common/guestRetirement.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "GuestRetirementTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	using Common::GuestRetirement;

	{
		GuestRetirement r;
		Check(!r.IsRetired(0, 1), "nothing published: nothing retired");
		r.NoteStarted(0, 10);
		Check(!r.IsRetired(0, 10), "started is not retired (stall mode: the guest still holds the memory)");
		Check(!r.IsRetired(0, 9), "an earlier item is not retired by a start either");
		r.NoteSignal(0);
		Check(r.IsRetired(0, 10), "a signal retires what the queue has started");
		Check(r.IsRetired(0, 9), "and everything before it");
		Check(!r.IsRetired(0, 11), "but not the next submission");
		Check(!r.IsRetired(0, 0), "sequence 0 is unnumbered: never retired");
	}

	{
		// A signal covers what was started at that time, no more.
		GuestRetirement r;
		r.NoteStarted(0, 5);
		r.NoteSignal(0);
		r.NoteStarted(0, 6);
		Check(r.IsRetired(0, 5), "5 retired");
		Check(!r.IsRetired(0, 6), "6 started after the signal: not retired");
		r.NoteCompleted(0, 6);
		Check(!r.IsRetired(0, 6), "completion alone is not a visible effect");
		r.NoteSignal(0);
		Check(r.IsRetired(0, 6), "the next signal retires it");
	}

	{
		// A signal with nothing started publishes nothing.
		GuestRetirement r;
		r.NoteSignal(3);
		Check(!r.IsRetired(3, 1), "no started submission: no retirement");
		Check(r.SignalCalls() == 1, "the call is counted");
	}

	{
		// Queues are independent unless the other queue's work has finished and a later signal covers it.
		GuestRetirement r;
		r.NoteStarted(1, 20); // compute
		r.NoteStarted(0, 21); // graphics
		r.NoteSignal(0);
		Check(r.IsRetired(0, 21), "graphics retired by its own signal");
		Check(!r.IsRetired(1, 20), "compute item started, not finished: a graphics signal says nothing about it");
		r.NoteCompleted(1, 20);
		Check(r.IsRetired(1, 20), "compute finished and a graphics signal covers its sequence: the label may wait for it");
		r.NoteStarted(1, 30);
		r.NoteCompleted(1, 30);
		Check(!r.IsRetired(1, 30), "a finished compute item newer than any signal is not retired");
	}

	{
		// Monotonic: older values never lower a mark.
		GuestRetirement r;
		r.NoteStarted(0, 50);
		r.NoteStarted(0, 40);
		Check(r.Started(0) == 50, "started is a maximum");
		r.NoteSignal(0);
		r.NoteStarted(0, 60);
		r.NoteSignal(0);
		Check(r.Signalled(0) == 60, "signalled follows started");
		r.NoteCompleted(0, 60);
		r.NoteCompleted(0, 55);
		Check(r.Completed(0) == 60, "completed is a maximum");
	}

	{
		// Queue ids beyond the table share the last slot: more retirement, never less, no overflow.
		GuestRetirement r;
		r.NoteStarted(1000, 7);
		r.NoteSignal(1000);
		Check(r.IsRetired(1000, 7), "out-of-range queue is tracked");
		Check(r.IsRetired(GuestRetirement::kMaxQueues - 1, 7), "in the shared slot");
	}

	{
		// One publisher, one reader: a reader never sees a retirement go back, and every sequence
		// is retired once the publisher is done.
		GuestRetirement   r;
		std::atomic<bool> stop {false};
		std::atomic<bool> regressed {false};
		std::thread       reader([&] {
            uint64_t seen = 0;
            while (!stop.load()) {
                uint64_t retired = 0;
                for (uint64_t seq = 1; seq <= 2000; seq++) {
                    if (r.IsRetired(0, seq)) {
                        retired = seq;
                    }
                }
                if (retired < seen) {
                    regressed = true;
                }
                seen = retired;
            }
        });
		for (uint64_t seq = 1; seq <= 2000; seq++) {
			r.NoteStarted(0, seq);
			if (seq % 3 == 0) {
				r.NoteSignal(0);
			}
			r.NoteCompleted(0, seq);
		}
		r.NoteSignal(0);
		stop = true;
		reader.join();
		Check(!regressed.load(), "retirement never goes back");
		Check(r.IsRetired(0, 2000), "everything retired at the end");
	}

	{
		Check(&GuestRetirement::Global() == &GuestRetirement::Global(), "one global instance");
	}

	std::printf("GuestRetirementTests: ok\n");
	return 0;
}
