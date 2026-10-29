# Host pthread CPU authority adoption

Source lane: `wip/host-pthread-authority`, exclusive tree
`C:\work\gesher-host-pthread-authority`, based on a0c47c29 (the 3eaa8b7c
bionic source plus later source-only diagnostics and prototype documentation).
The old `gesher-pthread-foreign-cpu-clock` worktree is preserved without edits.

## Status and next steps

Initial component run40801790244/attempt1 at exact
67e4f27486f6a63a8becb1c518c02b6b6afac02f passed its compile and both Windows
jobs. Actual pthread/std-thread/detached-create/live-detach/Win32-self-attach
target brackets, busy/idle/dedup, retired-clock/invalid/fixed clocks,
exit259 and dynamic-export checks passed. Handle counts were133→135 on2022
and142→144 on2025. This is bounded initial evidence only.

The next source adds actual loaded DLL path plus SHA256 verification against
the sealed member (and a wrong-digest process rejection), a recorded actual C
client compiler command,256 live target slots plus257th EAGAIN preservation,
saved-clock non-aliasing,64 four-reader acquisition/query races against real
detach/join/exit, actual pthread cancellation, and1024 rapid detached exits.
Preparing those controls exposed a wrapper race: an owned supplier duplicate
could wait for the clock lock while its target exited, then mint a fresh ID
after reaping its old slot. The wrapper now checks owned-object liveness and
deduplicates before reaping, then checks liveness again before new insertion.
These changes require a fresh source-bound run; the initial green does not
verify them. Resource-duplication/generation/clock-ID fault roles and the
whole host/translator/guest/package/e2e requirements remain open.

Earlier failures remain part of the record. Run40800633035 at exact
358bb280 failed during whole supplier DLL link: upstream's GCC-only
`-Wc,-no-pthread` reached Clang as an unused argument under `-Werror`.
All supplier C objects, including the changed record, compiled; no DLL,
host or Windows result was qualified. The correction overrides only the
upstream link variable to omit that GCC argument, preserving `-Werror`.
Its DLL import audit parses a nonempty case-insensitive DLL inventory, rejects
unknown/duplicate imports (including the supplier itself in mixed case), and
retains negative controls. The pinned Makefile link variables are checked
exactly before the single GCC-only argument is omitted; all other original
upstream link flags are retained.
There is no canonical full, package or e2e qualification yet.
Run40801261120 at f4da8fb6 compiled/linked/installed the complete supplier,
passed symbol/export/import/layout receipt creation, and compiled actual
`compat/posix/src/pthread.cc` with `-Werror`. Its host fixture failed to compile
because this compiler's libc++ Win32 backend returns a native HANDLE, not a
pthread_t. The fixture correction keeps the worker's real `pthread_self()`
identity for the API, duplicates the live std::thread native HANDLE as an
independent oracle, and compares live TID plus creation timestamp and CPU
brackets. There is no handle-to-pthread cast and no Windows result yet.
Do not mark the host foreign-thread contract closed. Existing diagnostic
407989 (a0c47c29) fails import sealing before Windows and is not host evidence.
No local C/C++/Java/C# compilation or native execution occurred.

The prototype in `native3-foreign-cpu-clock-prototype` is preserved as history.
The implementation patch lives in `tools/host-pthread-authority/supplier.patch`.
Independent source review found and corrected its detached-create race:
after `ResumeThread`, the detached target may retire and recycle its record.
The creator now makes no record access after resume. The original native
handle remains owned by `cpu_handle`, closed only by locked pool retirement.
The versioned duplicate API takes that same lock across registry lookup,
liveness check and duplication. Supplier generations never recycle.

The host POSIX wrapper requires the patched header at compilation and the
new supplier symbol at link time. It keys 256 retained clock slots by supplier
generation, deduplicates acquisitions, reaps signaled handles, and never
reissues a clock ID. Queries read the retained kernel object; neither a TID
lookup nor a borrowed unlocked pthread handle supplies authority. The new
`time.h` overlay routes rebuilt source callers to the registry and preserves
the original supplier for fixed clocks. Prebuilt/direct symbol callers that
did not include the overlay are not qualified by this change.

The shared setup action builds a whole pinned winpthreads supplier in a
separate SHA256-verified compiler extraction; it never overwrites the cached
llvm-mingw tree. Static and DLL/import/header/notice digests, exact source
files, compiler driver identities, build logs and DLL exports are recorded.
This changes the shared build boundary and therefore requires fresh matching
canonical host, translator, package and e2e evidence before adoption. Existing
component workflows that extract their own compiler are not automatically
qualified by the shared action. No supplier library may be copied alone into
an older package or translator set.

The new narrow workflow is explicitly *initial controls*: genuine pthread,
std::thread, detached-create/live-detach and Win32 self-attached targets,
independent GetThreadTimes brackets, busy/idle behavior, dead saved clocks,
pool reuse, actual exit259, fixed clocks, invalid identities and handle-count
bounds. A separate dynamic client pairs with the recorded DLL and queries
the actual v1 export. Typed member/source/run/attempt and actual ZIP receipts
are required before execution on both Windows versions.

Before dispatch: finish exact source/build peer review and repair findings.
After the initial run: preserve actual raw proof and add separate fault-role
supplier/host controls for duplicate failure, generation and clock-ID
exhaustion, all 256 slots, concurrent acquisition/exit/detach/cancel, rapid
detached retirement, saved-clock non-aliasing across repeated reuse, and
stronger handle-count stress. Do not infer these controls from initial tests.
Then rebuild all host components and the matching translator, retain both
guest ABIs and ART/APK, original fixed clocks, package notices and e2e gates.
Obtain independent peer review before every push/CI, as requested.

Shared edits: `.github/actions/setup-build/action.yml` and the additive host
test target in `CMakeLists.txt`. Test90 remains assigned to the existing CPU
clock track; this narrow executable has no new suite number or ADR allocation.
