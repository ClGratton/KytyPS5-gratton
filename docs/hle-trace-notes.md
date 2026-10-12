# HLE call trace (`--debug-hle-trace`)

A debug aid for guest crashes that are the end of a chain started earlier: the guest reacted to an
HLE call that failed or returned less than it asked for, and crashed seconds later. The fatal fault
report used to show only the faulting instruction. With the flag on it also shows what each thread
was doing.

## What it records

For every HLE function the guest can import (`SymbolType::Func`), except the hot calls listed
below, the calling thread's ring gets one entry:

| field | meaning |
|---|---|
| time | `CLOCK_MONOTONIC`, printed as milliseconds before the dump |
| name | `dbg_name/NID`, for example `FileSystem::KernelRead/Cg4srZ6TKbU` |
| args | the first four integer arguments (raw registers) |
| caller | return address, printed as `module+offset` |
| result | set by the HLE functions that call `Common::HleTrace::Ret(x)` (all file and memory calls) |
| note | up to 59 characters: the path of `KernelOpen`/`KernelStat`/..., the file name tail on reads |

Rings hold 1024 entries per thread. A guest thread that makes no HLE call does not get a ring.

## Fault report

On an unhandled host exception the report prints, in this order:

1. the faulting thread, `pc` and the return addresses on the frame-pointer chain as `module+offset`
   (resolve in the decrypted main module with `objdump -D -b binary -mi386:x86-64`; the file offset
   of a SELF text segment is in its segment table);
2. the registers, 96 bytes of code around the pc and the top of the stack (as before);
3. the faulting thread's last 96 traced calls;
4. the last 400 calls of all other threads on one timeline, with thread names.

## How it works

`Libs::InitAll` registers the HLE functions in the `SymbolDatabase`. With the flag on,
`SymbolDatabase::WrapFunctionsForTrace()` rebinds each function symbol to a 32-byte thunk:

```
mov r11, <Record*>        ; {name, real function}
jmp [rip+0] ; dq KytyHleTraceTrampoline
```

The shared trampoline saves rdi, rsi, rdx, rcx, r8, r9, rax, r11 and xmm0-7, calls
`KytyHleTraceLog(record, saved)` and restores everything, then `jmp *8(%r11)`. The callee sees the
caller's registers and stack unchanged, so stack arguments, varargs (rax) and functions that never
return (thread exit, context switches, setjmp) behave exactly as without the thunk. There is no
shadow stack. Results are therefore not captured generically: functions that want theirs recorded
call `Common::HleTrace::Ret()` / `NoteString()`; with the flag off those are one predictable branch.

Hot calls stay bound directly, because hundreds of thousands per second would push everything else
out of a ring within a millisecond: pthread mutex lock/unlock/trylock, `Getthreadid`, `PthreadSelf`,
`pthread_getspecific`/`setspecific`, `KernelGetProcessTime*`, TSC and clock reads. Replace the list
with `KYTY_HLE_TRACE_SKIP=substr,substr,...` (empty = trace everything; expect about 600k calls per
second from one level-loading thread).

Cost with the flag on: about 40 ns per traced call. Linux x86-64 only; elsewhere the flag is
accepted and ignored.

## Known limits

* Only the first four arguments are kept, and pointers are not dereferenced (a string argument is
  visible only where the HLE function notes it).
* Results of functions other than the file and memory calls are not recorded.
* The dump runs in the fault handler of the crashing thread; other threads keep running and may
  overwrite entries while they are copied (best effort, no locks, no allocation).

## Verification

* `hle_trace` (tests/HleTraceTests.cpp): argument and stack-argument passing through a thunk with
  ten integer and two floating-point arguments, double results, result and note capture, per-thread
  rings, merged timeline, hot-call skip list.
* A crash trace and the A/B runs that used it: `bench/RESULTS.md`, section "crash #2 root cause".
