# Native3 foreign pthread CPU clocks: source checkpoint

Base: accepted integration f1550540186b4ae3f17a5e501a886ec1c4d5d613.
Exclusive tree C:/work/gesher-pthread-foreign-cpu-clock, branch
wip/pthread-foreign-cpu-clock. Existing ADR0075; test90 is reserved.
No local native/Java/C# compilation or execution. No CI result yet.

## Bounded source change

The bionic entry point resolves a real target through the pthread registry
under its existing lifecycle lock. Host/ART-created callers that obtain a
bionic self identity retain a kernel handle too. Clock acquisition duplicates
the target handle while the registry lock excludes bionic detach/join/exit
cleanup. Read operations use GetThreadTimes on that retained object.
The current-thread/process fixed clocks and their existing proof are unchanged.

At most 256 acquired clocks retain handles. Repeated acquisition for one live
identity shares a slot. Every clock operation and new external self identity
reaps signaled entries and closes handles. External self identities additionally
retain one handle per live external thread; terminated entries are reaped on
the next operation. IDs decrement from -65536 through INT_MIN in int64_t,
never wrap or recycle, and exhaustion returns EAGAIN. Unknown/dead pthreads
return ESRCH; retired/unknown clock reads currently return EINVAL. Outputs are
written only on success. No Windows TID is reopened. Kernel object identity
therefore survives join/detach and handle/TID allocator reuse.

The new NDK native2 part exercises a real busy target and a sleeping caller,
1024 repeated acquisitions, resolution, invalid/dead identities, unchanged
output on errors and 64 replacement targets. Both ABI ART and all existing
native2 APK contexts require its actual success record. This is source-only:
there is no accepted target-vs-caller measurement or lifecycle proof yet.
The original Windows/package/e2e assertions remain in force.

## Open work and proof limits

Linux reference CI records actual post-join gettime/getres errors before the
compatibility decision is accepted. Its observation is not a Windows proof.
Resource capacity/exhaustion, detached and external-self concurrency, invalid
negative IDs, native Win32 independent target brackets and handle-count reaping
still need dedicated test90 controls. Null external-registry entries on handle
duplication failure retain an EAGAIN marker; their cleanup needs a bounded
failure-path policy before acceptance. No scope checkbox is closed.

The native host pthread_getcpuclockid implementation is deliberately unchanged
and remains incorrect for foreign threads. Do not treat guest improvement as
closure of the public host API. A borrowed winpthreads pthread_gethandle result
is unsafe: its internal pointer lookup releases the registry lock before the
caller can duplicate the handle, and close sites are outside that lookup lock.
No ENOTSUP or ESRCH substitution for valid host threads qualifies as a fix.

## Required host supplier patch

CI pins LLVM_MINGW_TAG=20261006. Resolve that release's exact mingw-w64 source
revision from the supplier build metadata before authoring the patch; a current
upstream source inspection is only a design reference, not proof of the pin.
Add an exported atomic duplicate API inside winpthreads, returning a retained
query/synchronize handle and a nonreused thread generation. The lookup,
validation and duplication must hold the same lock as every thread-handle
close/replace, pool return and identity unregister operation, including TLS
detach, cancellation, join, detached exit and create failure. Merely locking
__pth_gpointer_locked or copying its returned handle is insufficient.

Compile that patched supplier in CI; retain source revision/patch digest,
compiler identity and exact library/DLL member hashes in supplier receipts.
Host and all runtime dependants must consume that same supplier. Only then
connect the host clock registry, preserving fixed current/process clocks and
testing valid std::thread/native_handle, explicit pthreads and external Win32
self-attached threads. Review dead-clock Linux results and bounded handle
counts, then independent source peer before Windows CI. Full qualification
requires exact source/attempt/compiler/ZIP role/member/digest receipts, both
original whole Windows suites zero, installed package23 and both e2e zero.

Upstream design reference (not the pinned supplier):
https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-libraries/winpthreads/src/thread.c
