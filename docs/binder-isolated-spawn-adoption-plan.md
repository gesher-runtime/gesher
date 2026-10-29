# Production restricted SpawnOwned adoption checkpoint

## Implemented candidate, 2026-10-29 (not compiled or qualified)

Component40853406582 attempt1 at exact2f65e7fe compiled successfully with the
pinned compiler; both Windows strict archive/source/compiler receipts passed.
Both actual lowbox nested create/reopen/read/delete/exit/retirement rows and
actual OW-private-name serve-denial/noninherit rows passed before both runners
failed the ancestor/final rename assertion (`win32=0`). The short-circuit check
did not identify which rename succeeded, so no individual pin control or full
component is accepted. Raw log:
`C:/work/gesher-ci-review/binder-policy-40853406582.log`.

The corrected source requests actual read/list access on ancestor/file pins,
rather than metadata-only access, and records every rename result separately.
It preserves exact error32 requirements and restores any unexpected successful
rename before rejecting the test. The metadata-sharing explanation is a source
hypothesis pending changed-source CI. No unchanged retry is permitted.
Readonly resource preflight now also rejects an explicit low integrity label or
a mandatory label lacking NO_WRITE_UP on every exact readonly object. Actual
low-label and missing-writeup asset counterexamples require confirmed rollback
and all policy slots empty; medium/NW then supplies the positive launch control.

The exclusive `wip/binder-isolated-spawn` tree now implements the first actual
installed route: persistent SystemServer's installed `com.gesher.lifecycle`
entry selects restricted creation in the existing SpawnOwned function. Other
packages and nonpersistent legacy routes remain explicitly unqualified for guest
isolation. The selection comes from the trusted server's installed process
starter, not an environment switch. PM entry identity must be the exact current
object; the native writer owns a UID/generation/profile/resource map and rejects
missing/mismatched policy rather than falling back to CreateProcess.

The shared header is byte-identical to qualified `558d69f9` (helper component
40843729511). Host provisioning creates a fresh generation-specific profile,
pins the current executable, grants runtime DLL/JAR/DAT files and explicit
runtime/APK asset trees read/execute, and grants only the app's data directory
read/write at low integrity. It rejects relative/reparse path components and
overlapping read/data trees. It pins each ancestor and final resource with
OPEN_REPARSE_POINT and no DELETE sharing, checks handle attributes, and retains
those handles until retirement; grant, revoke and label operations use the
retained final handles. Existing profiles are refused. ACL/label/profile
retirement is checked; failed cleanup retains its owner in a reserved fixed
slot for retry. A stale profile after an abnormal server death currently causes
fail-closed provisioning, not silent adoption. Normal persistent-device shutdown
now blocks new installed admissions, revokes and retires all installed policies,
and fails explicitly if exact child/resource retirement is unconfirmed.
The initial recursive-inheritance candidate was rejected at source peer because
enumeration cannot prevent namespace insertion after the snapshot. The revised
policy enumerates existing directories by retained handles, opens one-component
child names with NtCreateFile/RootDirectory/OPEN_REPARSE_POINT, and rejects
reparse and multiply linked files. Host ACL/label changes address each retained
object individually. Directory handles use MAXIMUM_ALLOWED, for which the
[documented SetSecurityInfo contract](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo)
suppresses propagation to existing children. New guest-created app-data objects
receive intentional inheritance; the host never rescans their paths at cleanup.
Mutable descendants retain DELETE sharing so guests can rename/delete their own
data. This API behavior requires actual adversarial CI proof, not documentation
alone. Injected cleanup failure retains the policy and blocks future admission.

The separate `installed-guest-policy.yml` component prepares actual Windows
profile tests for subtree reparse rejection, unchanged outside target ACL/label,
ancestor/directory/final-file rename error32 while pinned, failed cleanup
retention, removal of the hostile entry, confirmed retry, and successful rename
after all owned handles are released, a new reparse inserted after enumeration
whose target ACL/label remains unchanged through real grant/revoke, and an
actual restricted child that creates/reopens/reads/deletes nested data. Its actual compiler, archive, member,
source/header and current-run/attempt receipt derives from the qualified helper
fixture contract. It is not a replacement for actual APK/Binder full-CI proof.

Child retains the RestrictedLaunch owner, its policy and actual process/birth.
Start checks actual SID, low integrity, zero capabilities and atomic private-job
membership while suspended. Private publication checks the actual token again;
only affirmative publication precedes Resume. Failed launch/resume cancels the
exact lease and retains unconfirmed process/policy ownership. Kill/revoke/exit
use retained instances; no restricted process handle is double-closed.

The broker creates a separate per-install public endpoint with OW plus the exact
profile SID, low mandatory label, no FILE_CREATE_PIPE_INSTANCE guest permission,
and the unchanged actual-process challenge/lease/FD protocol. The private writer
channel and default host public endpoint have explicit protected OW-only ACLs.
Guest Binder clients request explicit data access without the pipe-instance bit
aliased by GENERIC_WRITE. The guest cannot publish a launch on any public pipe.
The reserved startup record now carries the actual expected broker PID under
ABI version3; restricted guests require it, and each actual Binder client
connection rejects a different named-pipe server PID. The real guest startup
also attempts CreateNamedPipe on both its SID endpoint and the host endpoint,
requiring exact access-denied errors rather than relying on the mask alone.
The resource component also creates the actual OW-only bootstrap Channel,
observes both endpoint handles non-inheritable, and has its real lowbox child
attempt CreateNamedPipe on that exact private name. The private channel's
one-instance constraint also remains in force; that denial alone does not
attribute a unique ACL cause or prove a new client could otherwise connect.

The actual installed guest startup now checks its real lowbox/zero-capability
token, fresh own-data write/read/delete, exact access-denied errors opening the
host executable for write, the actual package registry for read and write, and
the host Binder endpoint. It logs observations only after all succeed. Existing
services test requires this marker before real Binder attach, Application
onCreate, resource lookup, onResume, teardown and exit zero. This is a required
two-Windows full-CI contract, not evidence already obtained.

Independent source peer must precede push/CI. No local native/JVM/C# build or
fixture was performed. Original registry negative tests, strict current-run
source/attempt/compiler/archive/member receipts, matched host/DLL runtime,
both Windows whole-suite zero, package and both e2e remain required. Further
forced-failure production coverage, abnormal-server profile recovery and all
other guest/CLI/cohost/native harness migrations remain open. No integration,
main/PR/tag/release operation is authorized to this worker.

## Historical dependency analysis (superseded by candidate above)

Source-only analysis against Binder `92a007578729b65a581a5495ebdca01fea3e82f4`
in the exclusive `wip/binder-isolated-spawn` tree. No production change, CI,
qualified import, or boundary closure is claimed.

The concrete guest route is `SystemServer.java` app-process creation through
`Zygote.spawnPackage`, `services_natives.cc::Zygote_spawn`, and
`services_zygote.cc::SpawnOwned`. The three `StartDevice` calls are trusted
bootstrap actors and must stay explicitly distinct. An environment switch,
command argument or public native policy setter must not select host authority.

The reusable prerequisite at `2496db0d64a527649d67f833955f7b85dc2bd15b`
failed component `40787855371` attempt 1 on both Windows runners, stage 59
before the positive IPC observation. The suspended token/job and bounded
admission/retirement controls passed, but successful isolated child execution
and original denial/resource controls have not been established. Adoption is
paused while the helper's exact child exit and raw pipe diagnostics are added.

## Exact production dependency

The helper accepts an already provisioned SID and accessible executable/resources;
it intentionally performs no profile creation or ACL grants. Current Binder
`CreateNamedPipeW` supplies null security attributes. Current app spawning passes
a command line and install identity, but has no trusted install-owned resource
policy supplying a profile/SID, executable dependencies, APK/runtime read access,
private data write access and a compatible Binder endpoint ACL. Redirecting this
route to an unprovisioned profile would break the actual guest path and would
not qualify isolation. Same-user ACLs cannot establish guest/host separation.

Before connection, define install-generation-owned profile provisioning and
retirement under trusted package authority. Pin executable and required runtime
resources explicitly, allow only intended read/execute or app-data write rights,
and provide the exact private Binder endpoint access for that SID. Never grant
guest access to trusted code/config writes, the writer channel or host commands.
The caller cannot supply arbitrary paths, UIDs, profiles or grants. Revoke and
uninstall must retire exact children before deleting resources/profile; unknown
retirement must retain ownership and report failure.

## Coupled code changes after the prerequisite

1. Add a private owned policy value to SpawnOwned; select it only from the
   trusted package/install route. Preserve explicit trusted bootstrap behavior.
2. Keep existing environment, reserved UID Context and intentional stdio setup.
   Restricted children inherit no bootstrap writer/broker capability. Start
   RestrictedLaunch once with explicit executable/SID and the allowlist; reject
   breakaway and never enter generic CreateProcess after restricted failure.
3. Change Child ownership with process creation: retain the RestrictedLaunch
   owner alongside the exact process/birth/package/install record. Existing raw
   trusted process handles remain separately owned; never close a borrowed
   restricted process handle. Update ExitStatus, Kill, revoke and rollback
   together. Do not transfer the private job out of its owner.
4. Preserve affirmative private publication before Resume, using the actual
   retained handle and birth. Preserve UID/package/generation validation,
   writer leases, cancellation and all existing FD staging/rollback rules.
   Failed publication or resume cancels exact authority, stops the owned job,
   and reports original and retirement errors. Unconfirmed cleanup remains in
   the helper's reserved quarantine and is never considered successful launch.
5. CI must compile actual services_zygote and exercise the actual package call
   route on both Windows versions with profile/resource/Binder positive access,
   trusted resource unchanged, exact access-denied controls, publication/resume
   failure, no writer inheritance, exact lifetime/revoke and cleanup controls.
   Require fresh source/run/attempt/compiler/archive receipts and independent
   source peer before push. A direct helper fixture cannot substitute for this.

CLI/cohost/Main/service/native harness admission, delayed challenge/revoke,
concurrent FD staging, secondary AOT, broad full/package/e2e and release remain
open gates. No assertions are removed and no unrestricted fallback is proposed.
