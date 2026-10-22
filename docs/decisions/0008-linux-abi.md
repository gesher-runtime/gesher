# 0008 — Linux errno numbering in gesher-base

**Status:** accepted, 2026-10-20

## Context
libcore hard-codes Linux's errno values in Java: `android.system.OsConstants`
sets `EAGAIN = 11`, `ENOTEMPTY = 39` and so on in a static initializer, and
compares them with the values native code throws in `ErrnoException`. The
Windows CRT and winpthreads use their own numbers. Values 1-34 match Linux;
the rest differ (`ENOTEMPTY` is 41, `ETIMEDOUT` is 138), and some collide
with a different Linux error (CRT 110 is `EHOSTUNREACH`, Linux 110 is
`ETIMEDOUT`).

## Decision
gesher-base's `<errno.h>` defines Linux's values, so all code built against
it, AOSP and gesher-base alike, speaks Linux's numbering.

Because values collide, a CRT value cannot be recognised later; it is
translated exactly once, where it enters, by `gesher_errno_from_crt`
(`compat/posix/src/errno.cc`, table taken from mingw-w64's `<errno.h>`):

- gesher-base's own CRT calls go through `gesher::CrtResult`.
- `<pthread.h>` wraps the winpthreads calls whose results can differ:
  `pthread_join`, `pthread_mutex_lock`/`trylock`/`timedlock`,
  `pthread_cond_timedwait`, `pthread_rwlock_timedrdlock`/`timedwrlock`.
- `<unistd.h>` routes `rmdir` to gesher-base. Through `_dosmaperr` the CRT's
  file functions only produce values 1-34, except `ENOTEMPTY` from `rmdir`.
- `<string.h>` routes `strerror` and `strerror_s` to Linux's (glibc's)
  messages and provides `strerror_r` (GNU) and `__xpg_strerror_r` (XSI).
  The CRT's `strerror` is dllimported, so it cannot be overridden at link
  time; these are macros, like `rand`.

## Known gaps
- libc++ and winpthreads are prebuilt with the CRT's numbering: the
  `std::errc` values that differ from Linux's do not match the errors
  libc++ itself reports (`std::system_error` from `std::thread`, for
  example).
- `sem_timedwait` and the other winpthreads calls not listed above, and CRT
  functions that set `EILSEQ` (42) or `EDEADLK` (36), are not translated.
- libstdc++'s `<cstring>` `#undef`s `strerror`; the build uses libc++, which
  does not.
