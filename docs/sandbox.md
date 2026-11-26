# App isolation (AppContainer)

**Today Android apps in Gesher are not isolated from Windows.** Each app process
runs with the Windows user's full token. Android's permissions govern what an
app does through Android's APIs; they do not stop its code, native code above
all, from reading and changing the user's files, using the network or starting
programs. Treat an `.apk` like a downloaded `.exe`: install only apps you trust.
The README, the installed README and the installer say so.

This document is the implementation map for isolating every package in a
Windows AppContainer of its own ([ADR 0090](decisions/0090-appcontainer-app-isolation.md),
issue #314), and the record of what each milestone established.

## Trying it

M1 is a developer experiment for a test layout. The installed launcher
starts the ordinary runtime, even if its parent process set `GESHER_SANDBOX`.
The fixture scripts set `GESHER_SANDBOX=appcontainer` on their own test device
so the system server starts its app processes in their packages' AppContainers.
See the M1 results below for the launch dependencies the experiment exposes.

- `tests/windows/tracks/53-sandbox.ps1` (CI): binderd in sandbox mode and the
  production launch path with real AppContainer tokens
  (`tests/sandbox/appcontainer-probe.cc`).
- `tests/windows/sandbox/AppContainerPoc.ps1 -Dir <test layout>`: a device with the
  sandbox on installs and launches `lifecycle.apk`, records the app process's
  token and prints the logs. `-NoSandbox` runs the same as a baseline.

The CI probe also holds one AppContainer process open while the broker changes
shared storage from modify to read-only to scoped, and removes own-directory
grants at uninstall. It checks fresh opens of existing and host-created shared
files, denial of other packages' private directories, the package's own
directories, Windows owner cleanup, and readability of successive atomically
replaced policy files. This tests Windows ACLs and the native grant functions;
it does not establish a complete Android app launch in M1.

## Design

| Process | Token |
|---|---|
| binderd, servicemanager | the user's (broker) |
| system server | the user's (broker: devices, account store, shortcuts, HKCU) |
| app processes | the package's AppContainer |
| `gesher-exec` children of an app | inherit its AppContainer |
| dex2oat, profman | the user's today; a container in M4 |
| Control Center, `gesher-launch`, scripts | the user's |

- **Profile name**: `Gesher.App.` + the first 40 hex digits of SHA-256 of the
  device's canonical data root, a NUL and the package name
  (`CreateAppContainerProfile` allows 64 characters of a restricted set; the root
  keeps two devices of one user apart). Created at the first launch, deleted at uninstall while the switch is
  on. Grants stay when the switch is turned off (see the ADR's consequences).
- **Files** (explicit, inherited ACEs; idempotent, an ACE already present on the
  root is not written or propagated again, so a propagation that stopped part way,
  or a file renamed into the tree with an explicit ACL, is not repaired until M2):

  | Path | Trustee | Access |
  |---|---|---|
  | the runtime's directory, `ANDROID_ROOT`, `ANDROID_ART_ROOT`, `ANDROID_I18N_ROOT`, `ANDROID_TZDATA_ROOT`, the boot class path's directories | capability `gesherRuntimeFiles` | read, execute |
  | the package's code directory (`data/app/<pkg>-n`) | the package's container | read, execute |
  | its CE and DE data directories, its current profile directory | the package's container | modify |
  | its `.logcat` file | the package's container | modify |
  | `TEMP`/`TMP` | the container's own folder (`GetAppContainerFolderPath`) | (owner) |

- **Capabilities**: `gesherRuntimeFiles` and `gesherBinderClient` always;
  INTERNET adds `internetClient` and `privateNetworkClientServer` (Android's
  INTERNET covers the local network too). Nothing else yet: devices stay with
  the system server's services.
- **External storage** (when this experiment is enabled): policy publication
  updates the package's live storage ACEs, including downgrades and revocation.
  Shared grants inherit into newly created files. `Android/data` and
  `Android/obb` have protected DACLs retaining Windows user/SYSTEM access;
  each package receives a separate explicit grant on its own directories.
  Policy files receive their read ACE before atomic replacement so a running
  container can read the replacement inode. ACL errors abort publication and
  are reported, rather than treating a requested permission as applied.
  `MEDIA_RO` uses a coarse read ACL; media-type selection is enforced by the
  storage policy layer, not by Windows ACLs. This remains the optional M1
  experiment, not isolation for arbitrary native code. Existing open Windows
  handles retain the access the kernel already granted, protected/imported
  child ACLs can prevent inheritance, and Windows can skip propagation into
  children held without compatible sharing. The remaining M2 work still applies.
- **binderd** (sandbox on): clients open the pipe for read, `FILE_WRITE_DATA` and
  `FILE_WRITE_ATTRIBUTES` only (`kPipeClientAccess`), and the capability gets just
  that: never `FILE_CREATE_PIPE_INSTANCE`, so a container cannot serve an
  instance of the device's pipe. Pipe DACL `D:P(A;;GA;;;SY)(A;;GA;;;<user>)(A;;0x12018b;;;<gesherBinderClient>)S:(ML;;NW;;;LW)`.
  A restricted client's hello (in an AppContainer, or below Medium integrity,
  which the Low label also admits) is refused (`-EPERM`) when it claims a UID
  below 10000 or a pid other than its own (`GetNamedPipeClientProcessId`). The
  user's own Medium processes (the system server) are admitted as before.

## Milestones

| | Milestone | Status |
|---|---|---|
| M0 | ADR: threat model, trust split | done: ADR 0090 |
| M1 | Proof of concept: launch path, grants, binderd admission, a real launch; collect what is denied | done, results below |
| M2 | Runtime fixes for containers: descriptors, named objects, properties, record locks; caller UID from the container SID | next |
| M3 | Permission → capability mapping, devices through the broker, network switches, loopback exemption policy | |
| M4 | dex2oat, profman and `gesher-exec` in containers; LPAC where no device is needed | |
| M5 | On by default, per-app opt-out, docs/microg.md's warning updated | |

## M1 results (2026-11-23)

Windows Server 2025 (26100), local build of `main` at 0d25937 with this change
(dalvikvm, binderd, servicemanager, gesher-app.jar) over the framework, boot jars
and boot image of a CI layout from 2026-11-21 (the only patch change between them,
#157, does not touch this path). `lifecycle.apk`, headless.

**Works:**

- Profile creation, the grants above (no grant reported an error), and `CreateProcessW` with
  `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`: the app process ran as
  `S-1-15-2-…` at Low integrity (0x1000), its stdout and stderr in its log.
- In the container, ART started from the granted runtime files: boot class path,
  ICU, tzdata, `build.prop`, the package's logcat file.
- binderd (probe, CI track): a contained process with `gesherBinderClient` is
  admitted at its own pid with an app UID; its claims of UID 1000 or of another
  pid get `EPERM`; a container without the capability cannot open the pipe
  (`ERROR_ACCESS_DENIED`); the user's own file in `%TEMP%` is closed to it
  (`ERROR_ACCESS_DENIED`). A Low integrity process outside a container gets
  `EPERM` for UID 1000 as well, and the user's Medium process is still admitted
  with UID 1000.
- The same layout without the sandbox (`-NoSandbox`) runs `lifecycle.apk` to
  `onResume` and a clean exit.

**Blocks (M2), in the order the launch met them:**

1. **Descriptors.** gesher-base reserves every fd by opening `NUL`
   (`compat/posix/src/fd.cc`, `ReserveFd`). In an AppContainer `CreateFileW(L"NUL")`
   fails with `ERROR_ACCESS_DENIED` for every access mask and for `\\.\NUL`, so
   `eventfd`, `epoll` and sockets fail with `EACCES`, and the Looper aborts:
   `Could not make wake event fd: Permission denied`. Anonymous pipes,
   unnamed events and `Local\` objects of its own do work in the container, so
   the reservation needs a handle of that kind (or one the broker passes).
2. **System properties.** The area file is under `data/system/gesher-properties`
   and opened read/write by every process (`system_properties.cc`); the container
   has no access there, so the process falls back to its own `build.prop`
   values and cannot set the `ro.*` it seeds. Android's model fits: the area
   read-only to apps, writes through the system server.
3. **Shared named objects.** App processes share `Local\` objects with the
   system server and binderd by name: the descriptor broker's doorbell and
   stream positions (`binder_wire.h`), the properties mutex, the logging mutex,
   process mailboxes, revocable files, open file description pipes (`ofd.cc`),
   native fences (`native_fence.cc`), the audio shared state
   (`compat/media/audio/shared_state.cc`) and the trust store's lock
   (`compat/net/trust_store.cc`). In an AppContainer `Local\` is the
   container's own namespace, so these are silently separate objects. They need
   handles passed by the broker (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, or the
   rights channel) or objects created in the container's namespace by the broker.
4. **Record locks.** The registry is in `%ProgramData%\Gesher\RecordLocks-v1`
   with `Global\Gesher.RecordLocks.v1` (`record_lock_registry.cc`); the
   container can neither resolve `ProgramData` (`SHGetKnownFolderPath`:
   `E_ACCESSDENIED`) nor open the directory, and a `Global\` mutex it creates is
   not the one other processes use.
5. **Thread queries.** `sched_getscheduler` of the process's own threads fails
   (seen in the abort's thread dump only).

**Not reached yet** (beyond the Looper): binder transactions from an app
process, windows and Direct3D, the JIT and its profile, WebView2, clipboard,
IME and drag and drop under UIPI, Conscrypt and the user's root store
(`CurrentUser\Root`, content filters), loopback.

**Not a security boundary yet:**

- A contained process can still claim another application's UID (≥ 10000) to
  binderd; M2 takes the UID from the container SID.
- With the switch on, the pipe's Low label lets the user's own Low integrity
  processes outside any container use the user's full-access ACE: they cannot
  claim a system UID or another pid, but they could create an instance of the
  device's pipe or change its DACL (a Medium label stops both when the switch is
  off). The DACL cannot tell Low from Medium processes of one user, so M2 replaces
  the named pipe for contained processes with a connection the broker hands each
  one at launch (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), and keeps the pipe at
  Medium.

## Open questions

- WebView2 under an unpackaged AppContainer parent.
- Whether the JIT needs the `codeGeneration` capability outside the `*FromApp` APIs.
- Clipboard, TSF and drag and drop under UIPI (Low integrity).
- Reading `CurrentUser\Root` from a container, for content filters.
- The cost of propagating ACEs over large data directories.
