# Shared restricted guest launch prerequisite

Worktree `C:/work/gesher-guest-production-launch`, branch
`wip/guest-production-launch`, base `f1550540186b4ae3f17a5e501a886ec1c4d5d613`.
Source-only pending corrected independent source peer and exact-source CI.

Integration explicitly assigned this bounded additive API and migrated fixture,
without editing the independently active Binder branch. This is not production
adoption and does not close guest isolation, authenticated Binder, secondary AOT,
CLI/cohost/service compatibility, integration/full/package/main/release gates.
Unrestricted same-user host processes remain trusted. Ultimately all guest code
must execute in separate restricted processes; this prerequisite does not provide
that routing or decide compatibility policy.

`compat/framework/restricted_launch.h` owns the actual child/process/thread and a
private one-process kill-on-close job. Start accepts an explicit provisioned
AppContainer SID, zero capabilities, explicit inherited resources and startup
metadata. It creates exactly once suspended, with atomic JOB_LIST membership,
then checks actual SID, low integrity, empty token capabilities and job before
returning. Publication must precede explicit Resume. The owner stays alive until
child death. Stop terminates only its private job and requires a signaled exact
process handle within five seconds. Failure never invokes unrestricted creation;
no breakaway flags or retries are accepted. Start/Resume return structured original-error and cleanup evidence; Stop returns
confirmation plus termination and wait errors. A failed stop retains exact handles
for retry. The destructor moves unconfirmed ownership and error evidence into a
fixed-capacity quarantine reserved before process creation, emits an OutputDebugString diagnostic, and never drops owned
handles. The supervisor can inspect PendingRetirements and RetryQuarantined; only
a signaled process allows handle retirement. This is not an orphan-free claim
when termination cannot be confirmed.
No profile creation, ACL grants or implicit network capability is performed.

The migrated fixture copies the actual disposable d549283d source, preserving
all ten strict denial/resource rows, same endpoint allowed/denied transition,
host-positive process/thread rights, trusted code/config unchanged, exact error5,
actual suspended token and owned cleanup gates. It now invokes this shared API,
checks forbidden flags fail before creation, terminates a real suspended child
without executing it, rejects repeated resume, and proves a live inheritable
event excluded from the handle allowlist cannot be signaled by the actual child.
Its CI-only receipt hashes both fixture and shared header, preserving pinned
compiler/archive/run/attempt/executable provenance and mutation controls. No
local native/Java/C# compilation or fixture execution is permitted or performed.

## Precise later SpawnOwned connection (not implemented)

Binder source baseline `92a00757`, services_zygote.cc SHA256
`278e8dd346145c54acbe44abe66a5e3ad7e2d7868d83a79ed3c1652e20e9f21c`.
That file is not present in this base in its newer SpawnOwned form. The sole
integrator owns its changes; this branch adds no disconnected public toggle.

1. Add an explicit reviewed restricted policy argument to private SpawnOwned;
   trusted bootstrap stays an explicitly selected distinct path. Profile/SID and
   executable/resource ACL ownership must be resolved before selecting it.
2. Build existing environment, reserved launch Context and intentional stdio
   allowlist as today. Call RestrictedLaunch::Start for the restricted branch
   with those inputs; never retry the generic CreateProcess path on failure.
   Restricted guest children must not receive private writer/broker handles.
3. Use the returned exact process handle and birth for existing private authority
   publication while suspended. Retain the RestrictedLaunch owner in Child, so
   its job outlives the SpawnOwned stack; no raw-handle double ownership. Adapt
   completion/revoke/rollback together to retire this owner once, with failed
   cleanup quarantined and never authorized. Keep trusted Child ownership explicit.
4. Only after affirmative broker publication call Resume; failure cancels exact
   authority and requires confirmed owned stop before successful retirement.
5. Migrate an actual guest production call site and test that exact path with
   fresh allowed resources plus cross-process denial/invalid policy/publication
   failure/job/token/handle controls on both Windows. The fixture in this branch
   exercises the reusable primitive directly and does not establish that path.

The integrator must source-peer this connection against the then-current Binder
code before CI. Profile provisioning, install-generation ACLs, APK/dalvikvm code
and runtime dependencies, logging/stdio, Binder endpoint resource access and
host/guest compatibility remain explicit subsequent work. Documentation or
Microsoft API semantics alone cannot qualify those boundaries.

## Initial source peer correction

The integrator blocked af9c3709 before push: Start/Resume and the destructor hid
failed cleanup confirmation. Corrected source exposes original and retirement
errors separately, retains failed owners for retry and quarantines unconfirmed
destructor cleanup. A private friend fixture injects resume failure, termination
denial and wait failure against a real suspended child. It checks exact error
evidence and retained process/job identity, restores real functions and proves
actual retirement; another real suspended owner exercises destructor quarantine,
pending evidence and successful retry. No runtime result is inferred from source.

The repeat peer additionally blocked unbounded mutex-held retry and destructor
allocation. Corrected ownership uses 64 fixed atomic slots, reserved before job or
process creation. Exhaustion rejects admission before creation. Destructor
handoff uses the reserved slot without allocation or locks. Retry skips busy slots
and shares one total budget capped at 1000ms across the pool; unknown cleanup
keeps exact handles. The fixture fills reservations and proves rejection, makes
C++ allocations fatal during actual failed-cleanup destruction, and simulates a
busy retry owner to require explicit unknown evidence while retaining ownership.
These are source-only controls pending compilation and actual CI.
