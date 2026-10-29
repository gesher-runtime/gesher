# Native3 foreign pthread CPU clocks: source checkpoint

Base: accepted integration f1550540186b4ae3f17a5e501a886ec1c4d5d613.
Exclusive tree C:/work/gesher-pthread-foreign-cpu-clock, branch
wip/pthread-foreign-cpu-clock. Existing ADR0075; test90 is reserved.
No local native/Java/C# compilation or execution. Narrow Linux observation
run 40790238879 / attempt 1 at 9f748617e951aaccad3608ddd8b630ce06e1448e succeeded;
its actual job log reports post-join gettime=-1 errno22, getres=-1 errno22.
It does not compile or exercise the Windows source. The observed EINVAL
results are now asserted in subsequent Linux source and guest resolution
controls; those new assertions have not run yet.
Actual Linux observation log: job 123173374206, retained at
C:/work/gesher-ci-review/diagnostic-job-123173374206.log. Its exact source,
run and attempt must be bound into the later qualification supplier.

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

An owned component workflow is now prepared, not dispatched. Its preflight
pins the complete allowed non-doc delta from f155 without editing historical
full guards. It rebuilds the production standalone native host, bionic core
fixture and matching ARM64 translator with the same verified llvm-mingw archive.
Generated compiler records, actual C++ commands/flags and binary driver hashes
bind both native roles; two real NDK caller binaries reuse the same CPU
contracts and record their actual wrapper/clang identities. The publisher's
r27c archive SHA1/size are checked and its SHA256 is recorded. A typed manifest
binds all current source and exact payload members to source/run/attempt, with
strict actual ZIP API receipts before Windows execution. Historical supplier
guards are unchanged. This is a standalone component prerequisite; it does
not replace ART/APK contexts, original whole suites, package or e2e proof.

NDK publisher identity reference:
https://github.com/android/ndk/wiki/Home/24fe2d7ee3591346e0e8ae615977a15c0a4fba40

Linux reference CI recorded actual post-join gettime/getres EINVAL. Its
observation is not a Windows proof.
Dedicated test90 now has source-only independent brackets against owned Win32
thread handles, 257 real concurrent external self identities, 256-slot capacity
and EAGAIN/output controls, invalid IDs, 1024 acquisitions with no handle-count
growth, and actual target exits plus handle-count reaping. All remain unbuilt
and unrun; detached lifetimes and fault-injected resource paths still need
additional controls. Null external-registry entries on handle
duplication failure retain an EAGAIN marker; their cleanup needs a bounded
failure-path policy before acceptance. No scope checkbox is closed.

The native host pthread_getcpuclockid implementation is deliberately unchanged
and remains incorrect for foreign threads. Do not treat guest improvement as
closure of the public host API. A borrowed winpthreads pthread_gethandle result
is unsafe: its internal pointer lookup releases the registry lock before the
caller can duplicate the handle, and close sites are outside that lookup lock.
No ENOTSUP or ESRCH substitution for valid host threads qualifies as a fix.

## Required host supplier patch

CI pins LLVM_MINGW_TAG=20261006. Read-only GitHub source inspection resolved
annotated tag 1719710a305c261491784d4c68f77e2b50b27904 to llvm-mingw
commit e1878a59f8bf3e1a73afe28f1b83168b1d029ed8; its build-mingw-w64.sh
pins mingw-w64 a3d93999ef0521681d45e445c39964a7af0f593f. This identifies
supplier source, not the downloaded archive's build identity. At that exact
revision, detached create sets tv->h=0 and closes its original thread handle.
The new authority must therefore retain a distinct handle during creation and
self attachment, not merely add a locked getter. The exact pinned thread.c
also closes handles outside its registry lookup lock in TLS detach, native
exit and join; each close/replace site needs a synchronized lifecycle audit.
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

Exact source references:
https://github.com/mstorsjo/llvm-mingw/blob/e1878a59f8bf3e1a73afe28f1b83168b1d029ed8/build-mingw-w64.sh
https://github.com/mingw-w64/mingw-w64/blob/a3d93999ef0521681d45e445c39964a7af0f593f/mingw-w64-libraries/winpthreads/src/thread.c
