# Package child snapshot and launch registry primitives: incomplete authority

Disposition: SOURCE UNQUALIFIED; P1 PRIVATE BOOTSTRAP/TRANSPORT UID AUTHORITY
REMAINS OPEN. This checkpoint corrects the reported P2 in source and adds concrete
native registry/lifetime primitives. It does not complete the requested P1. No
private control channel is currently created, inherited or consumed; the new
registry header is deliberately not wired into binderd. Existing unknown-client
compatibility admission and wire-UID trust therefore remain security blockers.

Exclusive tree: `C:/work/gesher-binder-peer-identity-current`;
starting clean HEAD `cd58db7be791f6646483cfccadc095769c2120fa`, previous package
source `69739417566b932cf0cd580fb6bb6c76ddeda1e3`. Source checkpoint:
`109584a809f6a16dd1f5f44f2609e734a759f0f0`.

## Implemented behavior

SystemServer's ProcessStarter now passes immutable PackageIdentity to
Zygote.spawnPackage. JNI passes UID, package and install incarnation separately
from command/environment. The actual per-child STARTUPINFO.lpReserved2 contains
a copied, versioned UID snapshot with a zero CRT descriptor-count prefix. The
posix module captures this startup snapshot when loaded; getuid/geteuid and the
honest Binder shim prefer it over every GESHER_UID/GESHER_BINDER_UID value.
Guest setenv cannot change it. No parent environment mutation occurs. Generic
spawns preserve their legacy behavior and receive no package snapshot. This
snapshot is inspectable self-description and contains no capability/secret;
arbitrary callers can construct one, so it grants NO registration authority.
Exact Windows CRT reserved-startup ABI compatibility still requires rebuilt
native/VM verification. The bounded supported package UID range is unchanged.

The PM launchGate serializes the whole Installer install/uninstall mutation with
ATM capture, native spawn and PID publication. forceStop takes the same gate,
so a captured pid0 launch cannot resume after an install change's stop phase.
Order is launchGate, ATM monitor, PM monitor. Installer's monitor precedes the
gate; callbacks/other code must not acquire Installer under launchGate. The gate
currently covers expensive dexopt/stop waits; runtime liveness and lock-order
review are still owed. This is local launch serialization, not broker revocation.

Native children retain package/incarnation beside their actual CreateProcess
HANDLE and birth. Exact-incarnation revoke terminates matching retained owned
instances without reopening PIDs. forceStop invokes it before its prior process
handling. Failed spawn rollback waits 5 seconds for death; unconfirmed termination
retains the HANDLE in quarantine rather than silently discarding the instance.
Quarantine is reaped on subsequent revoke and otherwise retained until parent
exit. No broker admission rights exist in this local map.

Installer now distinguishes genuinely absent packages.list (NOFOLLOW_LINKS) from
existing nonregular/unreadable/unknown state. The latter fails before boot cleanup
can classify installed code directories as orphaned. This adds no ACL, fsync,
cross-process locking or hostile-host store protection.

## Implemented registry primitive, not connected protocol

compat/binder/launch_registry.h owns broker-relative process HANDLE duplicates,
obtains PID/birth from those actual handles, retains immutable package/16-byte
install incarnation/UID, and refuses duplicate live records, wrong UID/birth,
unknown PID and dead instances. Lookup never calls OpenProcess. Revoke marks
retained shared leases invalid and tombstones the exact PID/birth, so an old live
process cannot be relabelled under a reinstall generation. PID reuse with a new
birth remains distinct. AuthorityDied invalidates all leases and future publish.
There is no public wire selector; Publish is an in-process trusted-worker API.
Its caller must validate install authorization and supply a real transferred
HANDLE. No such caller exists at this checkpoint. System UID designation is
intentionally unsupported by this app-only primitive (10000..19999).

Challenge creates a fresh private event and duplicates EVENT_MODIFY_STATE into
the retained registered process instance. The same accepted transport must send
that child-local numeric HANDLE locator and receive acknowledgement, while the
child signals the private event. A buffered hello from a dead PID cannot signal
an event installed in the new instance. The locator is not a secret or authority
in another process's handle table. The broker waits for process death/event with
a 5-second deadline; supplied send/ack callbacks MUST enforce that deadline too.
The child cooperatively closes its local event duplicate. Broker never remotely
closes a possibly reused child handle slot. Uncooperative clients can retain up
to 64 duplicates per record; proof retries are then refused. Tombstones consume
memory for broker lifetime. These bounds need policy review.

## Exact integration still required

Launcher.startDevice still calls generic Zygote.spawn for binderd, servicemanager
and system server. services_zygote.cc still resumes after local bookkeeping,
without broker acknowledgement. binderd.cc ServeProc still assigns UID from the
hello and ServeThread has no registry/challenge binding. binder_wire.h remains
version 2 with no challenge frames. No independent control worker invokes
Registry::Publish/Revoke/AuthorityDied, and no startup root establishes who may
invoke them. The new header alone establishes zero transport authority.

The missing concrete prerequisite is a private owned launcher-to-broker endpoint,
restricted inherited HANDLE allowlists and trusted bootstrap transferring actual
retained launcher/servicemanager/system-server instances. System server must get
exclusive authorized writer capability; app/generic descendants must not. Only
that worker may publish server-selected package/incarnation/UID and transfer the
actual suspended child's HANDLE, acknowledge before ResumeThread, and revoke on
install mutation/cancellation/authority death. Admission and dispatch must bind
registry leases under the broker's existing g_lock, including blocked transaction
wakeup/cleanup. A correct connection protocol must carry the event challenge and
client signal/ack across both proc and thread admission, with unknown records
refused. It must explicitly authorize existing launcher CLI clients and fixture
VMs; names, env roles, public PIDs and client hello cannot do this. A process
loading untrusted app code cannot retain writer capability merely because it
also hosts a trusted service. No permission barrier prevents implementing this;
it is unfinished coordinated implementation in this checkpoint.

## Fixture source and verification limits

PackageIdentityFixture now launches actual fresh VM children for A and B with
the same PM/ApplicationInfo UID and a deliberately conflicting child environment
(GESHER_UID=19999, GESHER_BINDER_UID=1000). PackageUidChild asserts Os.getuid,
Os.geteuid and Process.myUid before and after mutating both environment fields.
The parent requires exit0 and a UID-specific log marker. It does not exercise a
remote Binder receiver, so honest registered sender UID remains owed.

binder-launch-registry-fixture.cc creates a real suspended child with an explicit
two-handle inheritance list, publishes its actual retained instance before
resume, and exercises the private event proof. Distinct assertions cover wrong
own UID/system UID, wrong birth, unregistered real parent, wrong incarnation
revocation, uninstall tombstone/reinstall revival refusal, live-authority death
and real terminated-instance refusal with honest live controls. This directly
tests the primitive API, NOT the missing private bootstrap or Binder transport.
Its anonymous-pipe callbacks are synchronous and require an outer CI watchdog;
they are not production bounded transport implementations. No frame asserts a
fixture role or registers from client hello. Full transport controls through the
eventual real private writer path remain required.

Performed only source inspection, diff checks and local Git checkpointing. No
native/Java/C# compile/configure, fixture/runtime, CI, push, GitHub write, artifact
download, clone, cleanup, external tree edits or subagent spawning occurred.
Original nine ordinary SG/malformed/FD rollback contexts and Native broker13
assertions/drivers/workflows are unchanged by diff scope. Exec/AOT source is
unchanged. No assertion deletion or excluded b52 ACK-loss/public WriteRead-prefix
relay case was introduced or run. Old acceptance artifacts qualify none of this.

Peer questions: validate CRT startup blob consumption and native load timing;
review launchGate lock order and stop/dexopt liveness; inspect retained HANDLE
quarantine/termination/error ownership; evaluate event proof's live transport
binding and retry/handle bounds; inspect tombstone/generation semantics. Then
review the still-missing private writer root, system client policy and g_lock
revocation integration before any claim of fail-closed UID admission. Same-user
host memory/handle theft, injection and binary/store replacement remain outside
the threat boundary; no Android OS sandbox is established.

## Source blobs

| Path | Blob |
| --- | --- |
| app/java/com/gesher/server/ActivityTaskManagerService.java | 41b5bc58b5d851055e5ce101faa0659375d725ca |
| app/java/com/gesher/server/Installer.java | f8411a9ad8e6799fc208230f7e54052584fc1c21 |
| app/java/com/gesher/server/PackageManagerService.java | 3f4df89081cc647609e4ce213f79d25d6caea17f |
| app/java/com/gesher/server/SystemServer.java | 8b74d5cae95e40eb6989cf5a579902daca752ab8 |
| app/java/com/gesher/server/Zygote.java | f796dca1caadded4f8b7f87de73608b8501f0f34 |
| compat/binder/launch_registry.h | 16d3a63096820a06f9ae03b4441f78b398150716 |
| compat/framework/services_natives.cc | 00ccb70f6200579714e0b904a591e382ec0f9856 |
| compat/framework/services_zygote.cc | 6eb642c19b566aae33519d7443c9f55b8bb2c009 |
| compat/framework/services_zygote.h | 4dea286e94c95197f1f5faba821002ddb6ab5992 |
| compat/posix/src/binder.cc | f80e506310c786dfbf5a07301aa5783f0f9985a6 |
| compat/posix/src/launch_context.h | eab9181a78c07bcaf7ec99ac067602419bc945f6 |
| compat/posix/src/unistd.cc | 01161b91d516fdceee62f61d501c7ad19d2ec596 |
| tests/dex-package-identity/com/gesher/server/PackageIdentityFixture.java | cd08cc2d6e4db24c19b23004bb35e9632eee4682 |
| tests/dex-package-identity/com/gesher/server/PackageUidChild.java | f5435ea7fe9e4abfcc463ac840720105db53f4ca |
| tests/windows/binder-launch-registry-fixture.cc | 4860f9355f288ec97d1b9e0eb8ab7ab57db04327 |
