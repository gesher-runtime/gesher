# Pinned winpthreads retained-authority prototype (not adopted)

Upstream source: mingw-w64 a3d93999ef0521681d45e445c39964a7af0f593f,
the version selected by llvm-mingw release 20261006. The adjacent patch and
input SHA256 manifest are source-only. `git apply --check` passed against
the exact three upstream source files in an isolated temporary directory.
No compiler, supplier replacement or native runtime fixture ran locally.
This is an unreviewed prototype, not host API qualification.

## Authority and lifetime

The new API is `gesher_winpthread_dup_cpu_handle_v1(pthread_t, void**,
unsigned long long*)`, declared with the existing WINPTHREAD_API mechanism
and GESHER_WINPTHREAD_CPU_AUTHORITY_API=1. It uses the Windows/MS host ABI,
not the bionic guest System V ABI. On success the caller owns a real query/
synchronize handle and a generation; on failure both outputs are unchanged.
Unknown/dead targets return ESRCH, bad output arguments EINVAL, duplication
resource failure EAGAIN. There is no borrowed handle or pointer cast.

The private record gains cpu_handle and cpu_generation. The original native
thread handle becomes its retained authority; the legacy `h` field is a
bookkeeping alias. Creation publishes that authority before ResumeThread,
including detached creation, which previously discarded its only handle.
Self attachment publishes the existing duplicated self handle, so Win32-
created threads and std::thread/native_handle do not need header wrappers
or a replacement registry that falsely rejects valid pthread identities.

Seven original handle-close sites (five `t/tv->h`, detached-create `thrd`,
and detach `dw`) release only their bookkeeping alias when it matches the
authority. Physical CloseHandle for that authority moves to
push_pthread_mem, under the same recursive registry lock that unregisters
the record and zeros it for reuse. The new API holds that lock across
lookup, signaled-state check and DuplicateHandle. Pool reuse therefore
cannot close or repurpose its source handle during acquisition. Other
temporary handles keep their existing close behavior. No extra duplicate
is required during creation, so this prototype avoids a new suspended-
thread rollback path when DuplicateHandle is exhausted.

Generation allocation shares the registry lock, starts at one and never
recycles: unsigned wrap to zero permanently prevents further registration.
The future Gesher clock registry must key cached target clocks by this
generation, bound its retained handles and never recycle issued clock IDs.
Saved clocks must read their duplicated kernel object, not reopen a TID or
trust a reused numeric HANDLE/pthread_t value. The underlying pthread ID
allocator still has its upstream theoretical wrap behavior; a numerically
reused stale public pthread_t cannot encode its former incarnation. That
does not authorize a saved clock to alias the new incarnation.

The lifetime check uses WaitForSingleObject(handle,0)==WAIT_TIMEOUT.
GetExitCodeThread/STILL_ACTIVE259 is not a liveness oracle:259 can also be
a real thread exit code. The existing current/process fixed clocks must
remain untouched when the host clock API is later connected.

## ABI, export and supplier boundary

Public pthread_t stays opaque and unchanged. The private record layout
changes, so rebuild the entire winpthreads supplier, not only thread.o or
one DLL. Verify the v1 symbol in both the static archive and DLL exports
and verify its host calling convention. Old headers/libraries must fail
compile/link or load rather than silently falling back to current-thread
clocks. Keep a distinct supplier receipt/version for this patched build;
the upstream `-version-info 1:0:0` alone cannot authenticate it.

CI must bind exact upstream revision, all input hashes, patch digest,
actual compiler driver and commands, configure/build flags, complete static
archive/DLL/import-library/header member digests, source/run/attempt and
explicit patched-supplier role. Rebuild every consuming host and translator
from the same reviewed source/supplier set, then audit the actual roles.
Do not replace the production llvm-mingw installation or import this patch
into integration before independent source/lifecycle review and CI proof.

## Required real host controls

- A busy explicit pthread target, busy std::thread/native_handle target,
  detached target and CreateThread self-attached target must each have a
  distinct measured clock while the caller sleeps. Compare samples with
  independent GetThreadTimes brackets from an owned Win32 handle.
- Unknown integer pthread IDs, dead targets, null output arguments and
  retired clocks must preserve outputs and return the specified errors.
  Do not substitute ENOTSUP/ESRCH for a valid untracked host target.
- Hold real targets alive across repeated acquisitions and concurrent
  detach/join/cancel/exit. Verify signaled exit, pool/generation reuse and
  saved-clock non-aliasing, including a real final exit status of259.
- Verify handle-count growth bounds, deduplication, actual reclamation,
  duplicate-resource failure and generation exhaustion with explicitly
  separate fault-test role; those controls must retain genuine targets.
- Keep the original fixed current/process clock controls, both Windows
  runner suites, both guest ABIs, actual ART/APK, package and e2e gates.

The prototype has not connected compat/posix/src/pthread.cc to this API.
That public host implementation remains incorrect for foreign threads.
