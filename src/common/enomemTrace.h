#ifndef KYTY_COMMON_ENOMEM_TRACE_H_
#define KYTY_COMMON_ENOMEM_TRACE_H_

// Debug aid: a guest-visible ENOMEM is silent unless the guest logs it, and the guest often reacts
// to it with a null pointer (a later "Unhandled host exception ... address=0x0..0x40" in guest
// code). Every `return KERNEL_ERROR_ENOMEM` in the kernel layer goes through NoteEnomem(), which
// prints the first few occurrences with the function, line and thread to stderr. One relaxed
// atomic increment otherwise; the count is printed by the fatal handler's context via Count().

#include <cstdint>

namespace Common {

int      NoteEnomem(int error, const char* function, int line) noexcept;
uint64_t EnomemCount() noexcept;

} // namespace Common

#endif
