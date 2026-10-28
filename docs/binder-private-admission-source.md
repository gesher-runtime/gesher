# Active dedicated-device admission: unqualified source checkpoint

Disposition: SOURCE UNQUALIFIED. This connects an initial owned persistent-device
authority path; it is not a runtime acceptance, independent peer pass, global CLI
closure, secondary AOT authorization, or Android sandbox claim. P1 stays open
until independent exact-source review and newly rebuilt component/full evidence.

Exclusive tree: `C:/work/gesher-binder-peer-identity-current`. Starting clean HEAD:
`b1a4ea63599c2283a5b0b62c890f4bc029dd3a61`; previous primitives source:
`109584a809f6a16dd1f5f44f2609e734a759f0f0`. Active source checkpoint:
`cc13e24c288f550bb9134b3062bf6b886061e38c` (implementation commit
`36db0782d6838499915edcac3ef2a1c7ba124955`, then fixture job containment). Prior bounded peer report
`C:/work/gesher-ci-review/binder-launch-registry-peer-high-109584a8.md` matches
its JSON SHA256 `d3e0724c9ecc8dfd4f034d0870e7bf38e0ac339f89e3bb83c0c114e1ca0e6fb9`.

## Actual process model and authority entry

Launcher.startDevice originally performed three generic spawns. It now calls
Zygote.startDevice/nativeStartDevice/zygote::StartDevice once. The native root
creates a private duplex pipe, starts binderd with only its broker endpoint,
registers the actual launcher process, and creates servicemanager and SystemServer
suspended. Actual CreateProcess HANDLE duplicates and creation times are published
over that endpoint, acknowledged before ResumeThread. Explicit reserved bootstrap
records are launcher UID2000, servicemanager UID1000, SystemServer UID1000.

The same native operation hands the client endpoint and a retained actual broker
HANDLE only to its dedicated SystemServer child, acknowledges handoff against that
server's registered instance before resume, and closes the original launcher's
copies. App/generic spawn never inherits these handles. The private pipe's random
setup name is never published; only actual inherited endpoints authorize requests.
The zero-CRT-descriptor startup context is now version2, 40 bytes. Its numeric
handle locators grant nothing in another process's handle table. No capability
enters argv, environment, state/ready receipt or logs. Handle inheritance uses an
explicit allowlist; writer clears inherited flags before subsequent spawn.

Dedicated SystemServer parses APK metadata and launches separate
android.app.GesherAppProcess VMs; it does not execute their application classes.
It requires its inherited writer before publishing services and fails if it
cannot configure owned app starts. Main's compatibility path still cohosts
Services.installInProcess(Services.create(...)) and GesherAppDriver.launch in
one PID. Neither Main nor Services.create obtains writer capability. Calling the
public native methods without the inherited channel cannot register an existing
device client. Creating a separate device is not authority over another broker.

The original host's Java wrapper selects honest wire UID2000 only after its
actual record exists. Its legacy getuid fallback can still be10000; this host
self-description difference is outside the package-child UID guarantee and must
be reviewed before broader host UID qualification. Package children retain the
immutable per-child snapshot used by Os.getuid/geteuid/Process.myUid/BinderUid.

## Connected package, transport and revocation path

PM launchGate captures immutable PackageIdentity through ATM ProcessStarter to
SystemServer, Zygote.spawnPackage, JNI and SpawnOwned. The owned suspended child's
actual retained HANDLE, birth, package, install UUID bytes and UID are sent to the
broker's sole private control worker. That worker accepts app publication only
after handoff, bounded to app UID10000..19999 and nonreserved packages with a
nonzero generation. Generation authorization comes from the trusted PM writer,
not a client message or an independent broker PM database. Generic child spawn
gets a self snapshot only when requested and never receives publication rights.
Publication or resume failure cancels the exact instance, requests termination
and retains its HANDLE in quarantine if termination cannot be confirmed.

Wire version3 adds birth to proc hello and introduces a framed challenge before
both proc and thread admission. GetNamedPipeClientProcessId indexes an existing
record; admission no longer opens a claimed/observed PID. A fresh event is
duplicated into that record's retained process with EVENT_MODIFY_STATE. The same
connection must acknowledge after the live child signals it. Numeric PID reuse
cannot signal an event in another instance. Unknown PID, wrong UID or birth,
foreign proc/key, revoked install and dead instances fail closed. The hello cannot
register, relabel or revive a record. Proc.uid comes from the registry record and
the transaction sender_euid comes from Proc.uid. FD transfer/cleanup now also use
the retained process handles instead of reopening numeric PIDs; existing SG/FD
staging/rollback logic and assertions are retained.

Private messages and challenge callbacks use byte-mode PIPE_NOWAIT transfers,
partial cursors and one five-second deadline. An ambiguous private stream is
poisoned, never reused to resume an unacknowledged child. The control worker
monitors the retained authority and active process leases. Exact package/generation
revoke invalidates leases before native termination; cancellation tombstones the
exact PID/birth. Worker cleanup runs under broker g_lock through ProcReleaseLocked,
wakes/releases blocked Binder state and removes nodes/refs/buffers. Dispatch and
ThreadRead check registry and owner liveness under g_lock, including bounded
100ms wake checks; authority loss closes the broker. Old live instances cannot
be registered under a reinstall generation. UID/install mapping still supports
user0 only; unknown packages, shared UID and multiuser remain unsupported.

ATM attach now takes launchGate, waits for pending PID publication, and validates
pending sequence, Binder caller UID, exact owned PID, current install UUID/UID,
retained native owned-instance liveness and absence of an already assigned process
BEFORE consuming pending state. Dedicated mode cannot assign the first waiting
package to an unsolicited process. Compatibility local service mode retains its
non-authoritative in-process behavior.

## Concrete compatibility gap and deferred checks

Fresh unrelated Launcher VMs still read the public state receipt/device name and
call ServiceManager/GesherService directly. They have no owned registration and
now fail admission. No public compatibility fallback was added. Reusing a running
device from a fresh CLI, toast activator and old direct SystemServer/broker/probe
launch paths need an authorized host-command/bootstrap adaptation. The initial
original Launcher -> dedicated server -> registered package path is connected;
global CLI support is unfinished. Legacy raw SG/Native harnesses that start
binderd directly also require private harness bootstrap before a fresh full run.
Do not weaken admission or remove any of their existing assertions to do that.

All original SG/malformed/FD contexts, Native13 assertions, their drivers/workflows
and Exec/AOT sources stayed unchanged in this source diff. No excluded b52 ACK-loss
or public WriteRead-prefix relay case was introduced or run. This preservation is
source scope, not regression evidence. Old accepted artifacts qualify none of
these startup/wire/admission changes.

Peer review must inspect root-to-writer handoff, restricted inheritance and
endpoint ownership; the now40-byte reserved CRT startup ABI/native load timing;
partial/nonblocking pipe semantics and deadlines; process HANDLE rights and
failure/quarantine/cancel paths; same-connection live proof versus PID reuse;
g_lock/registry ordering, blocked-read cleanup and install/attach lock order;
server-selected generation validation; and host-command admission policy.
The existing64-proof cumulative lease limit and broker-lifetime tombstone memory
bound remain policy/liveness limitations. Client authentication of the broker's
public server endpoint, legacy numeric job adoption and broader fixture/source
manifest dependency coverage also require explicit review before qualification.
Same-user host injection, handle/memory theft and binary/store replacement remain
outside this boundary; this establishes no Windows app sandbox.

## Real-process fixture source, not executed evidence

`binder_private_admission_fixture` is an EXCLUDE_FROM_ALL CMake target, linked to
the production services_zygote implementation, with its own exception-enabled
fixture translation. It takes exact rebuilt binderd/servicemanager paths and an
existing fresh owned directory; a future outer CI watchdog is required. It uses
production StartDevice for actual root/system bootstrap, actual production
registered package Spawn, and a genuinely unregistered generic child. Child-local
proc/thread controls reject own forged UID/system UID, wrong birth and PID. Its
private writer transfers an actual separately created suspended child's HANDLE
before resume, rejects wrong private birth/system-UID publication, proves a wrong
generation cannot revoke the live record, revokes an accepted connection with a
pending read, refuses live-instance reinstall relabel and an actual terminated
instance, and checks broker exit after authority death. Fixture argv chooses only
local assertion behavior; it never grants a wire/public role or writer authority.
No proc key, endpoint locator or control capability is written to receipts/logs.

PackageIdentityFixture now starts a private dedicated writer VM and the real
servicemanager, keeps the existing real two-package install/update/reload/uninstall/
reinstall and user0 assertions, and publishes a remote Binder UID receiver. Real
A and B child VMs assert PM UID/self UID and remote Binder.getCallingUid before and
after deliberately conflicting GESHER_UID/GESHER_BINDER_UID mutation. The writer
fixture is selected by its inherited capability, never a public role flag. This
is fixture source only; rebuilt Java/native integration and actual framework
lifecycle/ATM negative controls remain owed.

Performed only source reads, edits, Git diff/source-scope checks and local commits.
No native/Java/C# compile/configure, fixture/test execution, CI, push, GitHub write,
artifact download, clone, cleanup, other-tree edits or subagent spawning occurred.

## Exact source blobs

| Path | Blob |
| --- | --- |
| app/java/com/gesher/app/Launcher.java | 97e4c9a2ed205653078deddf1f9ade32b9fba753 |
| app/java/com/gesher/server/ActivityTaskManagerService.java | 496b900b69b324fd6a45658a7e08e04ec0e29c7b |
| app/java/com/gesher/server/SystemServer.java | 2f032a7bd4128932c400ebbc856c1848fae60fd1 |
| app/java/com/gesher/server/Zygote.java | 874275e900f6b32abcd9acbd19711fe061cce26e |
| cmake/aosp/servicemanager.cmake | 9a5e82b08e6821ada0716ae550987d19d74c093a |
| compat/binder/binderd.cc | 8d89572ff03ead07cc3cf8b124ae072f27fec858 |
| compat/binder/launch_control.h | 7c1be3f6e5fc0b2a8dcf9ee5a985554aa5669d7f |
| compat/binder/launch_registry.h | a143d574c354614be2e0f69c7603ad384a406b60 |
| compat/framework/services_natives.cc | d9c4f46b08a2117f38f89eed618bf83882319e46 |
| compat/framework/services_zygote.cc | 242e471d6b483cdd60baae276b79cbe76cf3ea2b |
| compat/framework/services_zygote.h | 90f8d3dab4ca68abd5a8b191bd89ef19b59f8de1 |
| compat/posix/src/binder.cc | 91982ec527f1567b72f396c9c914c5d25d7b37f5 |
| compat/posix/src/binder_wire.h | 13a0d985396998ea9caff67706909f7e6232fa28 |
| compat/posix/src/launch_context.h | b6fe600aaae6d2f2f60be4294278c2412cd5ad67 |
| tests/dex-package-identity/com/gesher/server/PackageIdentityFixture.java | 12f27da0381eb02efd87603bff2de0ba70029855 |
| tests/dex-package-identity/com/gesher/server/PackageUidChild.java | 3df906657f3bf81302647a60b646ae9def837626 |
| tests/windows/binder-private-admission-fixture.cc | 76f86c72abf8b13cf859df2f7e12be8bc4e3c474 |
