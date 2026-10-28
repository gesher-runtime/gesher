# Binder Windows peer hardening: bounded, unqualified source

Newest follow-up: [child UID snapshot and registry primitives](binder-launch-registry-primitives-source.md),
source `109584a809f6a16dd1f5f44f2609e734a759f0f0`; P1 remains open.

Current follow-up: [package/install identity and suspended-child source](binder-package-uid-source.md),
source `69739417566b932cf0cd580fb6bb6c76ddeda1e3`. The text below records the
earlier transport foundation at `8404eb00`; its package/launch implementation
inventory is historical. UID admission remains blocked after the follow-up.

Disposition: SOURCE UNQUALIFIED, UID AUTHORITY STILL BLOCKED. This is a first
transport-hardening slice, not the complete P1 correction. Do not merge it into
primary, qualify secondary AOT with it, or infer package isolation from it.

Exclusive local worktree: `C:/work/gesher-binder-peer-identity-current`, branch
`wip/binder-peer-identity-current`. Qualified primary base:
`caeee6a84882b327c86fdc05590a6ace98d27133`. Source commit:
`8404eb005d8a4e0e9b9e023dc3fc86bc58116639`.

Source blobs:

| Path | Git blob |
| --- | --- |
| compat/binder/binderd.cc | 54d75425185b6231a527df23818e45822f0d7c0e |
| tests/windows/binder-peer-identity-fixture.ps1 | 229a0e5fab591d7ce38ad10a0020991939ad78c3 |

The incoming corrective review was read and its SHA256 independently matched
`ae56695aa0b2289035f7ef3a8ce70ac82ff479319f822dfda1c14b12fdf8c2dc` at
`C:/work/gesher-ci-review/aot3-secondary-security-peer-d4d81150.md`.
That secondary branch is not included in this worktree.

## Implemented contract

Every accepted proc/thread pipe obtains its client PID from Windows before
reading the hello. Query failure, inaccessible process, non-live process,
unrepresentable PID, or failed creation-time query closes the connection.
A mismatched claimed proc PID gets exactly status `-EPERM`, with no Proc/key.
The stored transaction PID comes from the Windows observation. Proc retains
the queried process handle and creation time. A thread requires the existing
proc/key AND the observed matching PID/creation time AND a still-unsignalled
retained process handle. A foreign process with a deliberately shared valid
key gets `-ESRCH`. Requests entering the existing dispatch lock after retained
process death get `-EBADF`. ProcRelease closes the retained handle under that
same lock. No message version, transaction serialization, buffer protocol,
copyout, SG ownership, or FD rollback algorithm changed.

This does not authenticate UID: the hello UID is still copied for compatibility.
Unknown/unregistered guests are still admitted by that existing behavior.
Consequently unknown-guest fail-closed UID admission and valid registered-package
controls are NOT implemented. The fixture deliberately reports this limit.
No test selector or claimed role was added to production.

## Exact missing authority and next source prerequisite

At this base `compat/posix/src/binder.cc:364-372` explicitly takes BinderUid from
getuid or `GESHER_BINDER_UID`. `SystemServer.java:106-109` chooses system UID
by setting that environment variable. Neither proves a launch authorization.
`PackageManagerService.java:252-259` returns the first package for getNameForUid
and all packages for getPackagesForUid. There is no per-package UID authority.
`ActivityTaskManagerService.java:250-258` ProcessStarter takes package/startSeq/
targetSdk and PendingProcess stores PID; `:663-671` publishes PID only after the
child starts. `SystemServer.java:197-218` passes command-line and environment to
generic Zygote.spawn. `Zygote.java:50-54` calls nativeSpawn; native
`services_zygote.cc:95-160` receives arbitrary command/environment and returns
the newly created PID while retaining its process handle locally. None sends
an authorized package/UID/retained child instance to binderd. The launcher
starts binderd, servicemanager, and system server through the same generic
spawn path (`Launcher.java:268-293`). No RuntimeController implementation is
present in the inspected `app`, `compat/framework`, or `tools/host` sources.

The missing primitive is a broker-owned launch registry with a trusted writer.
A complete next slice must establish the bootstrap trust root when the launcher
creates binderd, using a private inherited capability handle restricted by
PROC_THREAD_ATTRIBUTE_HANDLE_LIST; bootstrap must designate retained server
process instances, not names, public environment roles, or caller-supplied PIDs.
The broker must validate the control endpoint peer instance, accept registrations
only from the designated launcher/system-server capability holder, retain each
child process handle and birth, and record the server-assigned package/UID.
Generic public spawning must not confer registration authority. System server
must create app children suspended, register the exact retained child instance
and its server-selected identity, and resume only after broker acknowledgement;
failure must terminate its own suspended child. Registration changes/revocation,
system UID startup, servicemanager, launcher clients, and honest test VMs each
need explicit trusted policy. The binder hello then matches the peer instance
to this registry and derives UID from it; unknown/mismatch fails closed. This
coordinated bootstrap/registry is a required implementation, not provided here.
Do not substitute foreground package lookup or another client field for it.

## Windows API evidence and limits

Primary API references consulted:

- [GetNamedPipeClientProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getnamedpipeclientprocessid)
  supplies a pipe client PID, not a process handle, guest UID, or creation time.
- [GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)
  supplies creation time for a queried process handle.
- [Process handles and identifiers](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers)
  distinguishes retained handles from reusable numeric process IDs.
- [GetNamedPipeServerProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getnamedpipeserverprocessid)
  can help validate the broker on the client side once a trusted expected server
  instance exists; querying an untrusted device name alone creates no trust root.

The separate PID query/OpenProcess sequence cannot itself prove that an already
disconnected, buffered hello belongs to the process instance opened after PID
reuse. Capturing before hello and re-querying PID narrows this race; it does not
remove it. A trusted pre-existing launch-instance registry plus a live
challenge/response bound to that instance (with appropriate protocol revision)
is still required for a stronger initial-connection claim. No such claim is
made for this slice. Peer creation time is not read from the pipe kernel object.

The pipe identity describes the connecting process; a host able to duplicate or
inherit another process's pipe handle, inject code, read private capabilities,
or alter trusted binaries can delegate or steal that connection. Same-user host
processes are not an Android sandbox. Capability inheritance must be restricted
and descendants must not inherit registration rights. No resistance to a hostile
host user or kernel is claimed. Initial connection liveness, blocked read/wait
termination, and asynchronous process-exit revocation need separate review;
this slice checks liveness on hello and request dispatch, with existing pipe EOF
release behavior. FD duplication still uses existing PID-based helpers.

## Controlled fixture and verification

New standalone `tests/windows/binder-peer-identity-fixture.ps1` requires CI and
an explicit exact rebuilt binderd executable. It starts a uniquely named owned
broker and a real PowerShell child whose proc pipe stays open. The child proves
honest proc/thread handshakes and shares its valid id/key only with the controlled
attacker. Parent opens separate real pipes, claims the live child's PID with
UID 10000 and 1000, and requires `-EPERM` without returned keys. Parent then uses
the child's valid proc/key on its own transport and requires `-ESRCH`; its own
honest proc/thread succeeds. Closing its own proc must eventually invalidate its
key. Only the expected transient status zero is retried while EOF is processed;
other errors fail. The fixture never sends a WriteRead, transaction, buffer
claim, or finish frame. It needs an outer CI watchdog because synchronous pipe
reads are not individually deadline-bounded. This is NOT a registered-UID
positive or a standalone forged-own-PID/foreign-UID negative; those remain owed.

Performed: clean starting status, exact base/worktree verification, review hash
check, source inspection, git diff --check, and PowerShell AST parse with zero
parse errors. NOT performed: native/Java/C# compile/configure, fixtures, artifact
download, source clone, CI, push, merges, or runtime qualification. No excluded
ACK-loss relay/public WriteRead-prefix case was run or delegated. All qualified
ordinary SG/malformed/copyout/FD rollback nine contexts and Native broker thirteen
modes/contracts remain source-unchanged, including their drivers and workflows;
this is preservation by diff scope, not newly executed regression evidence.
AOT, Native, Exec, accepted patches, and primary integration sources are unchanged.

## Exact peer questions before any qualification

1. Does the retained-handle locking/close lifetime cover all thread admissions
   and request dispatch without a use-after-close or changed rollback behavior?
2. Is the initial PID/OpenProcess reuse race an explicit blocker for promoting
   even this limited slice, and which live challenge/launch-registry binding is
   required for the project's actual host threat model?
3. What trusted client/launcher/system-server/servicemanager policies permit
   strict unknown-instance refusal without accepting self-asserted system UID?
4. Can a single private inherited capability bootstrap server designation and
   delegated child registration without leaking it through generic spawn?
5. How will authoritative package UID assignment, suspended-child publication,
   revocation, process-exit handling, foreign-own-PID UID negatives, legitimate
   registered controls, and exact rebuilt runtime regression be implemented?
