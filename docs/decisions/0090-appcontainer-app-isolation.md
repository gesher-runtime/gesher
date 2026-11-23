# 0090: Per-package AppContainer isolation of app processes

Status: proposed, 2026-11-23. Milestone M1 (proof of concept) implemented behind
`GESHER_SANDBOX=appcontainer`, off by default. Tracking issue: #314. The
implementation map and the proof of concept's results are in
[docs/sandbox.md](../sandbox.md).

## Context

Android isolates every application in a kernel sandbox: its own Linux UID,
SELinux domain and file permissions, with permissions enforced by the kernel
(the `inet` group for INTERNET) or by system services that learn the caller's
identity from the kernel (`Binder.getCallingUid()`).

Gesher runs every app process with the Windows user's full token
([docs/microg.md](../microg.md), "Gesher does not place APK processes in a
Windows OS sandbox"). Android's permission checks run in Gesher's Java services
and in the compatibility code inside the app's own process, so they constrain an
application that uses Android's APIs, but not hostile code:

- Paths are not confined (ADR 0010): `/` is the current drive, and Windows and
  UNC paths are accepted as they are.
- Native code (x86-64 directly, arm64 through the translator) can call any
  Win32 API.
- INTERNET is enforced by `GESHER_INET=0` in the process's own environment
  (`compat/posix/include/gesher/inet.h`); Winsock called directly ignores it.
- binderd takes the caller's pid and UID from the client's hello message
  (`ServeProc`), and the client takes its UID from `GESHER_BINDER_UID` in its
  environment, so a process can claim to be the system server (UID 1000).
- binderd's pipe has the default DACL.

Users reasonably expect an `.apk` to be sandboxed as it is on a phone. It is not:
it is as trusted as a downloaded `.exe`.

## Decision

1. **Say so now.** The README, the installed README and a page of the installer
   state that apps are not isolated from Windows and that an `.apk` deserves the
   trust of an `.exe`.
2. **Isolate with AppContainers.** Each Android package runs in a Windows
   AppContainer of its own, created per user without elevation
   (`CreateAppContainerProfile`). The kernel then refuses the process every
   object whose ACL names neither its container SID, one of its capability SIDs
   nor ALL APPLICATION PACKAGES: the user's files, `HKCU`, other processes and
   shares. (ALL APPLICATION PACKAGES can read much of Windows, Program Files and
   HKLM; LPAC, milestone M4, opts out of it.) It stays in the
   user's session and desktop, so windows, Direct3D and WASAPI keep working, as
   they do for Store applications. No VM (a project principle), no separate
   Windows accounts.
3. **The system server is the broker.** It creates the profile, grants the
   runtime's files to a custom `gesherRuntimeFiles` capability (read/execute) and
   the package's own code, data, profile and log files to the container SID, and
   starts the process with only the capabilities the package's Android
   permissions map to (INTERNET: `internetClient` and
   `privateNetworkClientServer`). Devices stay behind the system server's
   services, as in AOSP.
4. **binderd admits containers through a capability.** With the sandbox on, its
   pipe has an explicit descriptor: SYSTEM and the user, the custom
   `gesherBinderClient` capability for reading and writing but not for creating
   pipe instances (so no container can serve the device), and a Low mandatory label
   (AppContainer processes run at Low integrity). No other AppContainer on the
   machine (a Store app) can reach it. The label also admits the user's own Low
   integrity processes, so binderd treats those as restricted too.
5. **Caller identity comes from the kernel, in steps.** M1 already refuses the
   hello of a restricted client (in an AppContainer, or below Medium integrity)
   that claims a UID below `FIRST_APPLICATION_UID` or a pid other than its own
   (`GetNamedPipeClientProcessId`). M2 maps the
   container SID to the package's UID and ignores the hello's UID altogether.
6. **Run M1 in a development test layout** (`GESHER_SANDBOX=appcontainer`).
   Installed launches use the ordinary runtime during M1. Expose the feature
   when the milestones in docs/sandbox.md pass, with a per-app opt-out and
   diagnostics for denied access.

The Android-visible contract is unchanged: applications see the same APIs,
permissions, results and errors. The change adds the enforcement Android has:
a sandbox per app, permissions enforced below the app, and a calling UID the
operating system vouches for. Where the sandbox refuses something Android would
allow (a file an app may open on Android), that is a defect of the grants to fix,
not a new contract.

## Consequences

- Until M2, the sandbox is **not a security boundary**: a contained process can
  still claim another application's UID to binderd, and with the switch on the
  user's own Low integrity processes could serve an instance of binderd's pipe
  (docs/sandbox.md; M2 hands contained processes a connection at launch instead).
  It does not run apps yet
  either: objects that app processes share by name (`Local\` sections, events
  and mutexes) are separate per container, among the blockers docs/sandbox.md
  lists.
- ACLs are added to the runtime's and the package's directories (explicit,
  inherited ACEs). They are idempotent and only name Gesher's capabilities and
  containers; the user's own access is unchanged. They are not removed when the
  switch is turned off (a capability ACE grants nothing to a process without
  that capability); removing them is part of M5's rollout.
- Each package gets a profile under `%LOCALAPPDATA%\Packages`, removed on
  uninstall (`DeleteAppContainerProfile`).

## Alternatives considered

- **Restricted token and Low integrity alone**: mandatory integrity control only
  stops writes up by default, so code still reads every file.
- **A Windows account per app**: account and password management, and a
  different desktop session story; AppContainers give a SID per app without one.
- **A VM (as WSA did)**: against the "no VM" principle.
- **Win32 app isolation (MSIX)**: made for one statically packaged app; Gesher
  needs dynamic containers per APK, so it uses the underlying API.
