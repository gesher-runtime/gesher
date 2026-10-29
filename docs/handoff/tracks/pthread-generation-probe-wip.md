# Process pthread generation probe — scope and open work

This branch is an experimental CI-only supplier role. It is not an adopted
product provider and must not be enabled in production. The candidate adds a
non-reused key generation record, synchronized TLS slot generations, deferred
record recycling while cleanup holds a target, and ordinary-return callback
references. Key operations use key lock -> registry lock -> thread spin lock;
user destructors execute after releasing these locks. An experimental callback
module reference is acquired before the key lock and released after the last
ordinary callback returns and the key owner is deleted.

The tests compare Linux self-delete and concurrent delete/reuse behavior with
Windows, and exercise an actual unloadable callback DLL, same-slot reuse,
four destructor passes, and a forced wrapper detach window. Separate exact
production/fault and probe-fault roles preserve their own receipts and 338/+8
and TLS-clear controls. Their green status must be established from this exact
source; historical runs do not qualify it.

Open: cancellation, pthread_exit, arbitrary longjmp or other nonlocal callback
exits can abandon generation and target references. Safe release may require a
process-owned reaper that waits on the original kernel thread object before
unloading callback code. Thread-loader callback paths may run under the Windows
loader lock; no general lock-order or shutdown proof exists. Direct writes to
public `_pthread_key_dest` do not update generation records. Non-image/JIT
callbacks and DLL-only callback module ownership are not qualified. Complete
winpthread entrypoint routing, prebuilt/static third-party providers, both guest
ABIs, supplier adoption and the full Windows/package/e2e gates remain open.
No passing scoped test may be used to claim these contracts are closed.

## Standards and consumer scope, 2026-10-29

The Open Group's official Issue 7 and Issue 8 `pthread_exit` pages returned
HTTP 403 to this session. The primary [IEEE P1003.1 D3 / Open Group Issue 7
2007 draft](https://www.open-std.org/jtc1/sc22/open/n4217.pdf) states that
`pthread_exit` called from a destructor invoked by implicit or explicit
`pthread_exit` has undefined behavior. This is a draft, not a verified final
2024 standard. The [OpenBSD implementation manual](https://man.openbsd.org/OpenBSD-7.8/pthread_exit.3)
corroborates that narrow rule. It does not resolve cancellation or nonlocal
transfer in every context, the exported direct cleanup helper, or the stronger
Gesher obligations for foreign Win32 exit, clocks, cancellation and retirement.
Those requirements remain open; no tests or requirements are removed.

The [WG14 N1539 committee draft, 7.1.3](https://www.open-std.org/jtc1/SC22/wg14/www/docs/n1539.pdf)
reserves leading-underscore identifiers at file scope to the implementation.
That classification does not by itself resolve the semantics of use of a
vendor-declared export. The pinned winpthreads `pthread.h` declares writable
`_pthread_key_dest` with `WINPTHREAD_API`, without documenting an application
write protocol; its `pthread_cleanup_push/pop` macros explicitly dereference
`pthread_getclean`. A read-only scan of all 27 `.c`, `.h`, `.S` files in the
pinned `winpthreads/src`, `src/libgcc` and `include` directories found these
names only in the pinned `thread.c` and `pthread.h`. `git grep` found neither
name in Gesher `app`, `compat`, `cmake` or the top-level CMakeLists. These are
source findings, not evidence about prebuilt runtime or product binaries.

The probe checks the two module exports after completed key operations and
serializes library publication with module registration; it does not prove a
protocol for concurrent uncoordinated reads or writes. The existing writable
symbol and real implementation clients cannot be silently removed. Whether
third-party clients use its address or modify it is not established; arbitrary
writes are not assumed to be a required contract. The current-CI runtime
symbol inventory is bounded to its recorded toolchain roots, file formats and
two names; it cannot establish absent dynamic lookups, other PE consumers or
statically embedded suppliers. Producer and installed-product link/import
inventory remains necessary.
