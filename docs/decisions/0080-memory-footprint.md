# 0080 — Memory footprint: commit only what is used, release what the GC frees

**Status:** accepted, 2026-11-13. Extends ADR 0036 (placeholder mappings) and
ADR 0047 (arm64 translation).

## Context
Every dalvikvm.exe (the system server, each app process, each launcher)
charged about 3 GiB of commit at start and peaked at about 600 MB of working
set, even an idle one. A device with a few apps reached tens of GB of commit
(a machine with 32 GB of RAM at 50 of 57 GB committed), and arm64 apps used
far more working set than their Java heap explains. Measured per allocation
(VirtualQuery and QueryWorkingSet over live processes):

1. **PROT_NONE reservations were committed.** gesher-base's anonymous mmap
   committed every mapping, but ART reserves address space with large
   PROT_NONE mappings that cost nothing on Linux: the CMC from-space (heap
   capacity), the LinearAlloc shadow map (1 GiB), and the slack of every
   aligned map (`MapAnonymousAligned` maps size + alignment, so 2 GiB
   transiently for the LinearAlloc pool).
2. **LinearAlloc maps were 1 GiB, read-write.** Committed in full for a few
   MB of ArtMethods; the 1 GiB size exists for Linux's mremap page-table move,
   which Windows does not have.
3. **The CMC fallback copied the whole moving space on every GC.** Without
   userfaultfd, ART compacts stop-the-world after
   `mremap(MREMAP_DONTUNMAP)` of the moving space to the from-space. On
   Windows mremap copies, and it copied the whole capacity (256 MB), touching
   every page of both spaces. The from-space was then never released
   (`CompactionPhase`, which releases it, runs only with userfaultfd): 240 MB
   of the system server's 400 MB working set was a stale from-space.
4. **ART's ZeroMemory was memset on Windows.** `kMadviseZeroes` was false
   off Linux, so each release of GC memory (card table, bitmaps, mark stacks,
   arena pools, freed pages) wrote zeros over it and kept it resident.
5. **dynarmic's A64 fast dispatch table was 16 MiB per Jit** (one Jit per
   guest thread, one more per nested guest call), cleared on creation and so
   resident in full: 32 Jits in WhatsApp were 512 MB. Each Jit also committed
   16 MiB of code cache up front.

## Decision
- **gesher-base mmap** (`compat/posix/src/mman.cc`): anonymous PROT_NONE
  pages are reserved, not committed (placeholders are replaced by a reserved
  allocation). mprotect commits them (zero-filled) when they become
  accessible, and leaves reserved pages of ours reserved under PROT_NONE.
  Thread stacks are not ours, so mprotect(PROT_NONE) still commits their
  pages: GrowStack stops at committed pages (ADR 0036). mremap keeps reserved
  source pages reserved at the destination.
- **madvise(MADV_DONTNEED) is safe against concurrent access.** It gives
  private pages back by decommitting and recommitting them; Linux's is
  atomic, ours had a window in which another thread's access faulted (ART
  clears card-table ranges while mutators run). The range is published while
  decommitted, an epoch counts publishes and withdrawals, and the vectored
  handler (`fault_signals.cc`) retries an access violation once the page is
  back. Accesses from kernel mode (a syscall's buffer) in that window still
  fail with EFAULT; ART does not do I/O on the memory it zeroes. A failed
  recommit (the system commit limit) is retried, then reported.
- **ART** (`patches/art/0780-windows-memory-footprint.patch`):
  - the CMC fallback moves only the moving space's pre-compact extent
    (`End()` before `UpdateMovingSpaceBlackAllocations()`, which resets it to
    the post-compact end) and zero-releases the rest; after compacting, the
    from-space is mapped PROT_NONE again (reserved only), as it was before the
    first GC;
  - `kMadviseZeroes` is true under GESHER and ZeroMemory releases with
    madvise, as on Linux;
  - LinearAlloc maps are 32 MiB on Windows (the 32-bit size; `AddMap` adds
    more as needed).
- **dynarmic** (`compat/arm64/dll/patch-dynarmic-fpsr.py`, hash-checked like
  the FPSR corrections): the A64 fast dispatch table has 64Ki entries (1 MiB,
  A32's size), and the code cache commits 4 MiB up front (dynarmic already
  commits 1 MiB ahead before compiling each block).

## Measurement
An isolated device (its own install copy and `LOCALAPPDATA`), the same three
apps (SolitaireCG, Waze - arm64, OsmAnd - x86_64) with hidden windows, sampled
after 180 s; the system server, three app processes and three launchers:

| | before | after |
|---|---|---|
| working set, total | 2,732 MB | 1,710 MB (-37%) |
| private working set | 2,150 MB | 1,234 MB (-43%) |
| commit | 23,842 MB | 6,830 MB (-71%) |
| peak working set, sum | 6,167 MB | 2,052 MB (-67%) |
| system server commit / peak | 3,063 / 736 MB | 789 / 261 MB |
| Waze (arm64) private / commit | 1,555 / 5,499 MB | 803 / 2,115 MB |
| an idle launcher, commit / peak | 3,013 / 590 MB | 735 / 70 MB |

All apps stayed up; the system server ran 58 GCs and Waze 7 in that window.
`probes/posix/mman_probe` checks the reservations (section 6), and a stress
test of madvise racing reads, writes and locked RMWs from other threads (up
to 80 zeroing threads, tens of millions of accesses) runs without a fault.

## Consequences
- A SIGSEGV in a PROT_NONE mapping reports SEGV_MAPERR (the pages are
  reserved), where Linux reports SEGV_ACCERR.
- madvise(MADV_DONTNEED) of a writable file view still writes zeros (no
  release); ART's consumers of `kMadviseZeroes` all use private anonymous
  memory.
- Not addressed here: each launched app keeps a launcher dalvikvm alive for
  its lifetime (about 70 MB of working set each after this change), and the
  guest stacks of arm64 threads are committed (8 MiB each).
