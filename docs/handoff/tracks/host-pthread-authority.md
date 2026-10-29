# Host pthread CPU authority adoption

Source lane: `wip/host-pthread-authority`, exclusive tree
`C:\work\gesher-host-pthread-authority`, based on a0c47c29 (the 3eaa8b7c
bionic source plus later source-only diagnostics and prototype documentation).
The old `gesher-pthread-foreign-cpu-clock` worktree is preserved without edits.

## Status and next steps

Retirement diagnostics 40909237252/027d736d and 40911094059/2f2dd0b6
remain red on both Windows under the original +8 process handle bound.
The first observed two new Events after contention, then one Thread and one
Event after reuse; observed handle values and types do not establish ownership.
The second compared the surviving Thread's TID and creation time with all
338 target identities: it is the still-running controller thread on both
Windows, with no target match. Its supplier origin and the Event ownership
remain unproven pending further correlation. The intervening 40910469594
failed at -Werror before Windows and is retained as a compile failure.
There is no retirement fix or product qualification from these diagnostics.
Run 40912359945/06a525f6 remained red +10 on both Windows. The retained
Thread and Event handle values match the controller's supplier bookkeeping
values; the Thread also has the controller's actual TID. Net handle count did
not change while reading those values. The source-only successor avoids
registering a previously foreign join caller merely to check self-join, and
adds self-join/tryjoin and foreign tryjoin controls; peer and CI are required.
Run 40914415494/e9fb58ed is red overall: static and dynamic normal retirement
passed the unchanged +8 bound with 338 destructors on both Windows, but the
strict TLS-clear-failure output validator rejected diagnostic inventory text
before its expected abort marker. The next source suppresses inventory only
in the failure-mode process, preserving that validator and every normal check.
TLS/init failure and concurrency boundaries still require actual proof.
Changed-source component 40915541993 at
986b23949b26cadd0dbcea072b7c621b84cb13b1 passed all three jobs. On both
Windows, static and dynamic normal cases each retained the original +8 bound,
reported 338 destructors and zero fixture-owned handles, and passed contention,
unlocked destructor, live join, and 256/reuse controls. The observed final
counts were 60/68 and 61/69 on 2022, 64/72 and 65/73 on 2025. Both static
and DLL TLS-clear-failure cases returned the expected exit 3 and exact abort
marker. The controls include self join/tryjoin and foreign live tryjoin.
This is bounded component evidence only; init-failure injection, independent
actual audit, combined clock/provider behavior, product and full CI remain open.

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

Run40803071353 at e10fbed7 compiled successfully but failed the original
handle-reclamation bound on both Windows. Native stdout was buffered on
abort, so preceding stages do not yet have admissible raw PASS evidence.
The next diagnostic keeps that bound unchanged, makes stdout unbuffered,
prints per-stage actual handle counts, and counts every fixture-owned oracle
handle through checked CloseHandle calls. No leak cause or correction is
claimed before those actual counts are available. The dynamic pairing
client was not reached in that failed workflow step.

Diagnostic40803889036 at40228099 reproduces the same failure on both Windows.
Actual fixture-owned handles are zero at every retirement. Capacity returns
to stress-entry135/144;64 race rounds add32 (one per detached round), cancel
adds zero, and1024 rapid detached exits add exactly1024 (1191/1200 final).
Independent read-only peer identified source ownership loss: cleanup always
lazily allocates the spin_keys Event, but the wrapper's detached branch went
directly from p_clock destruction to pool memset without replace_spin_keys.
The new supplier patch uses the same retirement helper as join/TLS before
pool retirement. The wrapper already holds the registry lock, excluding
concurrent key-deletion traversal; cleanup has returned after unlocking the
spin lock, and no external API borrows that private spin-key lock. Event is
the source-predicted type, not a measured object-type observation. Actual
changed-source counts must verify the fix under the original bound.

The separate fault provider adds its control API only via a second patch,
applied only to a private copied compiler with role `fault`. The production
DLL/static supplier prove its test-control symbol absent; production POSIX
object proves the host exhaustion symbol absent and option OFF. The fault
object proves option ON and its symbol present. The real-target fixture
checks512 duplicate failures with untouched outputs/recovery, cached success
and a new-target failure at host ID exhaustion, permanent generation
exhaustion rejecting new registration, and real joins/reclaimed handles.
There is no production runtime environment fault switch. This variant and
the detached correction are source-prepared and require peer plus fresh CI.
Externally attached threads calling pthread_exit after detach have additional
manual retirement paths identified by the peer; those require their own
real controls and remain open. Whole provider/direct-prebuilt ABI routes are
listed in `host-pthread-authority-adoption-plan.md`.

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
