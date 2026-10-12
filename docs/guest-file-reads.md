# Short guest file reads: cause of the LevelTask crash, fix, diagnosis

## Symptom

Astro Bot, about 85-125 s into the gameplay route (the hub level load), the emulator exits with
code 65 and a fault report from the guest thread `LevelTask_ATQT`. Two families of signatures,
all in the level loader (a big function around eboot+0x7050000 and its callers):

* a read of address 0 (`cmp byte [r12],0`, r12 = 0) in a path-name helper whose name argument is a
  null `const char*` taken from a record table;
* a read of 0x28 (`mov rax,[rbx+8]; movzx ecx,[rax+0x28]`), a header pointer that is null;
* a hash loop over garbage (rax = 0xff33ffff, rsi/rdi random) in a callee of the same loader.

All three read data that came from a level file. Hit rate about 1 run in 4-6 on the stutter-next
build, in clusters (none for an hour, then two in a row).

## Cause

`FileSystem::KernelRead/KernelPread` call `Memory::InvalidateMemory(buf, n)` so the GPU write
tracker unprotects the destination, then read the file into guest memory with `fread`. The kernel
cannot take a page fault in a `read()` copy: when the GPU thread re-protects a tracked page of the
destination while the read is in flight (a buffer upload re-tracks pages it has just made
CPU-modified), `read()` stops at that page and `fread` returns the short count with the error flag
set. Nothing re-tried the rest, so the HLE call returned success with fewer bytes than asked and
the tail of the guest buffer kept its old contents (zeros). A game that asks for a whole file and
does not loop on short reads parses a truncated level, finds zeroed table entries and passes a null
name down the load path, usually some milliseconds to seconds later.

Evidence (bench/RESULTS.md, "2026-10-12: crash #2 root cause"): a crash run with
`--debug-hle-trace` shows, 2.1 s before the guest fault on the same thread,
`KernelRead(fd 3, buf 0x105c7e3bd0, nbytes 0x465750) = 0x430`, a 4.6 MB file under
`levels/hub_crashsite_tutorial/cinematics/` returning 1072 bytes, which ends exactly at the page
boundary 0x105c7e4000; no other read on that thread in the 1024-entry window was short. With the
fallback logged, every run has 1-6 such short reads (textures, animations, sounds, cinematics).

## Fix

K02 (`SysFileRead`, Linux): on a short fread with the error flag set (and not at EOF) the rest is
read into a per-thread 1 MiB host buffer and copied with ordinary stores, which take the page
fault and go through the GPU fault handler like any guest write. Ported from the Jetsku/KytyPS5 fork
(c2fde8064, Jetsku; the Windows build already did this for `ERROR_NOACCESS`). `SysFileReadFallbackCount()`
counts the events; the first 16 are logged with file and byte counts.

## Switches

| env | effect |
|---|---|
| `KYTY_SYSFILE_NO_READ_FALLBACK=1` | return the short count as before K02 (A/B on one binary) |
| `KYTY_SYSFILE_CHAOS_EFAULT=<permille>` | that share of reads of more than two pages stops at the next page boundary of the destination with EFAULT, as the kernel copy does. 5 (0.5 %) is enough to make a build without the fallback crash within seconds of the first level load |
| `--debug-hle-trace true` | per-thread ring of the last HLE calls in the fault report; reads show `= bytes` next to the requested size (docs/hle-trace-notes.md) |

How to recognise the bug in a new crash report: a `KernelRead`/`KernelPread` of a file with the result
smaller than the requested size and the file not at its end, on the faulting thread shortly before
the fault. The result usually ends on a 4 KiB boundary of the buffer address.

## Known limits

* Sockets: `Recv` still copies straight into guest memory; a TCP receive is allowed to be short, so
  the guest has to handle it anyway.
* The race itself (the GPU thread re-tracking pages under an in-flight read) is not closed; the
  fallback makes the read complete regardless.
