# Short guest file reads: cause of a guest crash class, fix, diagnosis

## Symptom

A guest loader thread (level or asset loading) faults a few seconds after it read a file: a read of
address 0 or a small offset from a null member, or a loop over garbage. The faulting code reads data
that came from a file. The rate is low (about one run in four on the title where it was found), it
comes in clusters in time, and it does not correlate with free memory, swap or disk traffic.

## Cause

`FileSystem::KernelRead/KernelPread` call `Memory::InvalidateMemory(buf, n)` so the GPU write
tracker unprotects the destination, then read the file into guest memory with `fread`. The kernel
cannot take a page fault in a `read()` copy: when the GPU thread re-protects a tracked page of the
destination while the read is in flight (a buffer upload re-tracks pages it has just made
CPU-modified), `read()` stops at that page and `fread` returns the short count with the error flag
set. Nothing retried the rest, so the HLE call returned success with fewer bytes than asked and the
tail of the guest buffer kept its old contents (zeros). A guest that asks for a whole file and does
not loop on short reads parses a truncated file, finds zeroed records and passes a null name or
pointer down its load path; the fault follows milliseconds to seconds later.

Evidence is in `bench/RESULTS.md`, section "crash #2 root cause" (not in this tree: it names the
title): a crash run with `--debug-hle-trace` shows, on the faulting thread 2.1 s before the fault, a
read of a 4.6 MB file that returned 1072 bytes, ending exactly on a page boundary of the buffer
address; it is the only short read in that thread's window.

## Fix

`SysFileRead` (Linux): on a short fread with the error flag set (and not at EOF) the rest is read
into a per-thread 1 MiB host buffer and copied with ordinary stores, which take the page fault and go
through the GPU fault handler like any guest write. Ported from the Jetsku/KytyPS5 fork (c2fde8064,
Jetsku; the Windows build already did this for `ERROR_NOACCESS`). `SysFileReadFallbackCount()` counts
the events; the first 16 are logged with file and byte counts.

## Switches

| env | effect |
|---|---|
| `KYTY_SYSFILE_NO_READ_FALLBACK=1` | return the short count as before the fallback existed (A/B on one binary) |
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
* The fault injection truncates the fread (it does not protect a page), so it exercises the guest-
  visible effect and the host-buffer copy, not the GPU fault handler; that path is covered by the
  `sys_file_read` test and by the natural fallbacks in every game run.
