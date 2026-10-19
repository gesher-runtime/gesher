# 0005 — gesher-base: a POSIX layer instead of `#ifdef _WIN32` in AOSP

**Status:** accepted, 2026-10-20

## Context
The ART runtime uses Linux interfaces in hundreds of places: `mmap`, signals
and `ucontext_t`, `dlopen`, `sysconf`, `/proc`, `futex`, ELF headers. AOSP's
own Windows support is a handful of `#ifdef _WIN32` branches that stub these
out, which is enough for `dexdump`, not for a runtime.

## Decision
Implement the interfaces once, in **gesher-base** (`compat/posix/`), and let
AOSP code use its existing Linux code paths.

- `compat/posix/include/` is first on the include path. Headers either add to
  mingw's (`#include_next`, e.g. `unistd.h`, `sys/types.h`, `signal.h`) or
  provide what Windows lacks (`sys/mman.h`, `dlfcn.h`, `sys/ucontext.h`...).
- Types and constants use **Linux values and layouts** (`pid_t` is 32-bit,
  `siginfo_t` and `ucontext_t` are the x86-64 Linux structs), because AOSP
  code and Android native libraries assume them.
- Implementations live in `compat/posix/src/`. Where Windows truly has no
  equivalent (`fork`, `userfaultfd`, mount namespaces) the call fails with
  `ENOSYS`/`EINVAL`, and ART's existing fallbacks take over.
- Third-party headers must be permissively licensed (`elf.h` is from musl,
  MIT).

As a consequence AOSP's Windows-specific files are replaced by their Unix
versions where possible (`mem_map_unix.cc` instead of `mem_map_windows.cc`).

## Also decided: Windows x64 is LP64 for ART
ART uses `__LP64__` to mean "64-bit pointers". Windows x64 is LLP64 and does
not define it, so ART silently compiled its 32-bit code paths (32-bit monitor
IDs, `posix_memalign`). Defining `__LP64__` globally is not safe — mingw's
winsock headers change `u_long` under it — so patch 0002 rewrites ART's checks
to `defined(__LP64__) || defined(_WIN64)`.

Remaining LLP64 risk: code that assumes `long` is 64-bit (it is 32-bit on
Windows). The compiler catches most of it as truncation warnings; an audit is
on the roadmap.
