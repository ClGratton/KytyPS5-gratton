#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_

#include "common/abi.h"
#include "common/common.h"
#include "kernel/pthread.h"

#include <deque>
#include <memory>

namespace Libs::LibKernel::EventQueue {

constexpr int16_t KERNEL_EVFILT_TIMER     = -7;
constexpr int16_t KERNEL_EVFILT_READ      = -1;
constexpr int16_t KERNEL_EVFILT_WRITE     = -2;
constexpr int16_t KERNEL_EVFILT_USER      = -11;
constexpr int16_t KERNEL_EVFILT_FILE      = -4;
constexpr int16_t KERNEL_EVFILT_GRAPHICS  = -14;
constexpr int16_t KERNEL_EVFILT_VIDEO_OUT = -13;
constexpr int16_t KERNEL_EVFILT_HRTIMER   = -15;
constexpr int16_t KERNEL_EVFILT_AMPR      = -25;

class KernelEqueuePrivate;
struct KernelEqueueEvent;

using KernelEqueue    = int64_t;
using KernelEqueueRef = std::shared_ptr<KernelEqueuePrivate>;

constexpr KernelEqueue KERNEL_EQUEUE_INVALID = 0;

using trigger_func_t = void (*)(KernelEqueueEvent* event, void* trigger_data);
using reset_func_t   = void (*)(KernelEqueueEvent* event);
using delete_func_t  = void (*)(KernelEqueue eq, KernelEqueueEvent* event);

struct KernelEvent {
	uintptr_t ident  = 0;
	int16_t   filter = 0;
	uint16_t  flags  = 0;
	uint32_t  fflags = 0;
	intptr_t  data   = 0;
	void*     udata  = nullptr;
};

struct KernelFilter {
	void*                 data = nullptr;
	std::shared_ptr<void> owner;
	trigger_func_t        trigger_func      = nullptr;
	reset_func_t          reset_func        = nullptr;
	delete_func_t         delete_event_func = nullptr;
};

struct KernelEqueueEvent {
	bool                    triggered   = false;
	uint64_t                deadline_ns = 0;
	uint64_t                interval_ns = 0;
	KernelEvent             event;
	KernelFilter            filter;
	std::deque<KernelEvent> pending_events;
};

[[nodiscard]] KernelEqueueRef KernelPinEqueue(KernelEqueue eq);

// A filter whose triggers carry a payload (a GPU interrupt's context id): every trigger is delivered
// as its own event with its own payload, in order. The first one becomes the current event, later
// ones wait in pending_events until the guest has consumed the earlier ones. The queue is bounded:
// when the guest has not waited for max_pending triggers the oldest is dropped (counted, logged).
// Called with the queue mutex held, from a filter's trigger_func.
constexpr size_t KERNEL_PENDING_EVENTS_MAX = 65536;
void KernelQueueTrigger(KernelEqueueEvent* event, intptr_t data,
                        size_t max_pending = KERNEL_PENDING_EVENTS_MAX);

// Number of pending events dropped because the guest was not waiting (diagnostics, --mem-stats).
uint64_t KernelDroppedPendingCount();

int KYTY_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event);
int KYTY_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter,
                                     void* trigger_data);
int KYTY_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter);

int KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name);
int KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq);
int KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out,
                                   const KernelUseconds* timo);
int KYTY_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int id);
int KYTY_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int id);
int KYTY_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int id, void* udata);
int KYTY_SYSV_ABI KernelTriggerUserEventForAll(int id, void* udata);
int KYTY_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int id);
int KYTY_SYSV_ABI KernelAddTimerEvent(KernelEqueue eq, int id, KernelUseconds usec,
                                      void* udata);
int KYTY_SYSV_ABI KernelAddHRTimerEvent(KernelEqueue eq, int id, const KernelTimespec* ts,
                                        void* udata);
int KYTY_SYSV_ABI KernelDeleteHRTimerEvent(KernelEqueue eq, int id);
int KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, int id, void* udata);
int KYTY_SYSV_ABI KernelAddAmprSystemEvent(KernelEqueue eq, int id, void* udata);
int KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, int id);
int KYTY_SYSV_ABI KernelDeleteAmprSystemEvent(KernelEqueue eq, int id);

intptr_t KYTY_SYSV_ABI  KernelGetEventData(const KernelEvent* ev);
intptr_t KYTY_SYSV_ABI  KernelGetEventFflags(const KernelEvent* ev);
int KYTY_SYSV_ABI       KernelGetEventFilter(const KernelEvent* ev);
uintptr_t KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev);
void* KYTY_SYSV_ABI     KernelGetEventUserData(const KernelEvent* ev);
int KYTY_SYSV_ABI       KernelGetEventError(const KernelEvent* ev);

} // namespace Libs::LibKernel::EventQueue

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_ */
