# Private runtime USER32 initializer diagnostics

The private launch gate still requires the original application witness to run.
These controls cannot turn a failed witness into success or change production
permissions. CI runs 45944645450 and 45946701754 observed USER32 static startup
failure `0xc0000142` and late-load error 1114. A medium-integrity private token
failed with both the exact SYSTEM-only process/thread object policy and the
guest default object ACL. The later hardware trace observed successful CSR
connection, client call, and ALPC exchange. These results do not justify an ACL
expansion.

The next diagnostic observes actual USER32 DllMain basic blocks and call
returns. Capstone 5.0.7 is an isolated test tool, never a runtime dependency or
packaged application file. Its official PyPI Windows x64 wheel is pinned by
SHA-256 `4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1`;
installation uses `--require-hashes --no-deps --only-binary=:all:`. The decoder
checks PE architecture, exact unwind function bounds, complete instruction
coverage and direct branch destinations. Only entry and its initializer tail
functions enter the bounded map. The map is not uploaded as a Windows binary.

The controller copies the map into its protected fixture directory using the
existing SYSTEM/administrator file descriptor. Application and private-token
grants on the directory permit read/execute only, and the map gets no guest
write grant. The SYSTEM observer verifies the actual System32 loaded-file
identity, SHA-256, PE entry/image bounds, map SHA-256 and every mapped opcode before arming.
It reads actual EFLAGS and raw EAX; it changes neither RIP nor application
arguments/results. Calls execute normally and are observed at their return.
Nested DLLs return through the same loader address, so completion additionally
requires the original stack frame. At most 128 owned hardware events and the
existing five-second observation deadline apply. Twelve recent records fit
inside the existing bounded diagnostic tail. Owned debug registers are cleared
on exit or observation failure. The same event deadline is checked during
metadata validation, DLL hashing and opcode verification. Synchronous Windows
filesystem calls themselves cannot be forcibly cancelled by this observer.

Local Windows x64 validation used the production late-loader probe under the
ordinary desktop identity, with a temporary readiness event only in the local
smoke controller. The pinned isolated tool generated 542 instructions from the
installed USER32 DLL, SHA-256
`d2bcc3d2fc93d4c237eade68ea6a6599ed3db8afef1e8b9bf0dd4874e1ed1dff`.
The real initializer completed with raw return 1, setup/error/cleanup 0 and
87 hardware events; the real child exited 0. Seven preparation integrity
mutations were rejected. Five live controls changed the DLL digest, an opcode (with refreshed map
digest), the map digest, the entry boundary and a different valid entry: all failed before any hardware event while the child's
actual USER32 operation still succeeded. This validates diagnostic mechanics;
it does not validate the separate private service identity or fix its failure.
The next elevated Windows CI run must supply that evidence.

## Actual CI initializer failure and separate heap control

Run `45960493274`, PR head `6c64c518d1625e0f95a0002129d04b1586e82a39`,
produced healthy bounded CFG traces on Windows 2022 and 2025. Both observers
reported setup/error/cleanup zero and a completed false USER32 initializer
return (84 and 83 events respectively). The original protected launch gate
still failed with `0xc0000142`; the observer-free step was skipped after that
failure and has not been claimed as passed.

On Windows 2025, USER32 SHA256
`d2bcc3d2fc93d4c237eade68ea6a6599ed3db8afef1e8b9bf0dd4874e1ed1dff`
returned `0xc0000017` from callsite RVA `0x6e1c6`. Its following negative-status
branch returned false. The exact image's import table maps the called IAT slot
`0xab078` to `GDI32.dll!GdiDllInitialize`. Windows 2022 recorded the same raw
status at its corresponding indirect initializer callsite `0x13316`.
The System32 image hashes, PDB identities and actual indirect-import names are
now printed locally by the post-failure explainer; DLLs are never uploaded.

Local static analysis of GDI32FULL SHA256
`db859a44f7e96788a910832817ec565201fbc3587809d7c99e458643598a0d70`
shows a `0xc0000017` path when its heap helper returns false after
`HeapCreate(0, 0, 0)` returns null. This is a target for further investigation;
the original CI log did not bind its GDI32FULL image or trace that heap call.
A standalone heap control cannot prove which internal GDI operation failed.

The explicit `--heap-control=` mode reuses the kernel32-only late-loader image
in three additional disposable processes: the two existing private-token object
policies and the actual lowbox. It calls the documented heap operation, records
the actual success flag and immediate `GetLastError`, and, if a heap exists,
records `HeapDestroy` and its immediate error separately. Errors after a
successful API call are raw observations, not defined failure results. The
control then exits after delivering its report; zero process exit means report
delivery, not a successful heap operation or successful USER32 initialization.
All three original loader rows run unchanged before these additional controls.

Exactly three new report files in the protected fixture directory are
SYSTEM-owned, with only the tested private/package SIDs receiving leaf write
and synchronize access. Only those report leaves receive a low mandatory
label. The parent directory, executable, metadata, process and thread policies
are unchanged. The child writes a fixed 32-byte report; the service fills the reserved
bounded process-observation fields. The controller reads the fixed-size reports after
the owned worker finishes. These files follow the existing exact owned-fixture
cleanup or explicit protected retention. The original service/controller IPC
payload bound, overall worker deadline, original gate exit and observer-free
condition remain unchanged.

Local MSVC `/W4 /WX` builds pass for the complete fixture and all three original
`/NODEFAULTLIB` probes. The ordinary desktop heap control created and destroyed
a real heap and delivered its report. A missing report file returned the real
`ERROR_FILE_NOT_FOUND` and was not created. All three original local loader
controls still exited zero. A quoted executable pathname containing the heap
mode substring also retains the ordinary loader path; the parser only reads
arguments after the image name. The decoder retains its seven original metadata
integrity controls and adds four malformed PE diagnostic-input controls. Actual
private-token/lowbox heap results and CI GDI image identities remain pending.

Run `45966509354` supplied those separate controls on both Windows versions.
All three actual subjects on each version created and destroyed their heap:
six create-success and six destroy-success observations, with report, process
exit and cleanup statuses zero. The Windows 2025 GDI32FULL hash matched the
local image above; Windows 2022 used a different recorded image. USER32 still
returned false through the `GdiDllInitialize` failure. These heap results do
not establish the internal failing call. Static decoding also identifies an
alternate initializer body, so the heap-failure branch is only a possible
path. The next observation must identify the actual GDI initializer path;
there is no evidence here supporting an ACL or memory-limit expansion.

## Separate GDI32FULL initializer observation

The preparation step keeps the original USER32 map and derives a second map
from the runner's own System32 GDI32FULL image. The GDI target is the
`GdiDllInitialize` export when it starts a verified unwind function, or the
exact unwind entry reached by its single direct leaf thunk. The DLL's PE entry
is checked independently. An unrecognized export shape produces an explicit
unavailable diagnostic and the original USER32 witness can still execute.

After the original loader rows, USER32 trace and heap controls, a separate
disposable late loader runs under the same existing fixture policy. The
observer verifies the loaded image, export and unwind bounds, map hash and
opcodes before arming. Calls inside that initializer execute normally and
only their returns are observed, including full RAX for GDI. The trace still
stops at the original stack frame, 128 hardware events or the existing
five-second deadline. Its last eight records fit beside the existing USER32
summary without adding IPC fields or receiving any new guest write grant.

With the matching local ordinary-desktop GDI32FULL image, the observer armed
and completed in eight events with status zero and the loader exited zero.
The actual root used the alternate helper at RVA `0x8a0bc`; the observer did
not enter its body. Three live map mutations (different valid instruction root,
different PE entry and opcode mismatch with refreshed table hash) were
rejected before hardware events; each loader still exited zero. These runs
validate the observer and leave the failing private branch unresolved until
the new control runs on the elevated Windows 2022 and 2025 fixtures.

Run `45988836731`, head `c75196f35855a4e007439a459712ba70f4efe2b7`,
provided that observation. Windows 2022 returned zero from the actual
`win32u!NtGdiInit` import at RVA `0x1436b`, then selected the explicit
`STATUS_NO_MEMORY` branch. Windows 2025 entered the alternate direct helper at
`0x8a0bc`, which returned `STATUS_NO_MEMORY`. Its body was not observed in that
run, so the Windows 2022 result does not establish its internal failing call.

The follow-up map preserves the original exported initializer root and may
include one immediate direct child containing the actual `NtGdiInit2` import.
The decoder rejects ambiguous children. A version-2 marker identifies that
one call; the native observer additionally checks its original root function,
direct-call opcode and target, exact child unwind entry and unique marker.
The observer enters only that child, observes its own calls at their real
returns and resumes observation at the original caller. It retains the
original root stack, 128-event limit, five-second deadline and IPC payload.
No Windows code, result, permission or production launch policy is changed.

On the matching ordinary-desktop Windows 2025 image, the extended observation
completed in 23 events, returned zero and left the real loader exit zero.
Four live mutations (removed marker, downgraded map, extra marker and retargeted
marker), each with a recomputed map digest, were rejected before any hardware
event while the loader still exited zero. The original version-1 root map
continues to complete in eight events. Preparation retains the original seven
USER32 and four PE controls and passes nine GDI controls. The full fixture
compiles with MSVC `/W4 /WX`. Private-token execution of this follow-up remains
pending; neither the ordinary run nor the new map is a USER32 repair.

## Kernel initialization result and an owned desktop control

The `2a0f23f208b1528ebed3a2c83d8d15bda0ee7e2e` hosted run completed
the extended observer on Windows 2025 in 17 events with setup/error/cleanup
zero. The actual `win32u!NtGdiInit2` import at `0x8a10c` returned raw RAX zero;
the branch at `0x8a128` selected `0x8a170`, which returned
`STATUS_NO_MEMORY`. Windows 2022 again returned zero from its actual
`NtGdiInit` call at `0x1436b`. These observations identify the failing kernel
initialization result; they do not identify its underlying cause. The original
protected-launch witness still failed.

An additional test-only control compares the unchanged Default desktop with
a newly owned noninteractive window station and desktop. The Session0 service
cannot create these objects in the interactive session: the documented
[window-station API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createwindowstationw)
uses the calling process's session. A separate SYSTEM helper therefore runs
in the already verified target session, under the existing fixture job and
deadline. Windows forbids cross-session handle inheritance, so the helper starts
suspended with inheritance disabled. Exact token, report and job handles are
duplicated directly into the retained helper process. A fresh first-instance,
SYSTEM-only named pipe delivers their values in a fixed bounded request. The
service checks the live retained client PID and session; the helper checks the
server PID, birth time and real SYSTEM identity. The request and both sides
share the original absolute deadline. No process memory is patched and no
production desktop-selection option is added.

The helper creates a GUID station with `CWF_CREATE_ONLY` and initially permits
only SYSTEM and administrators. It creates its own desktop there, then grants
the actual private user and package full rights and a low mandatory label on
those two new objects only. It reads back both descriptors. Matched additional
private and lowbox probes use the same existing tokens, token default ACL,
SYSTEM-only process/thread policy, environment and mitigation on Default and
the owned desktop. A nested kill-on-close job bounds these children. The helper
restores its station/thread desktop and closes its owned objects only after
confirmed child termination. An unconfirmed termination is reported as a
cleanup failure, with remaining owned handles retained until helper exit.
Existing station and desktop permissions are unchanged.

The controller reads the separate fixed report from the protected fixture
directory. A summary is appended to the existing diagnostic tail only if it
fits; it never evicts an original USER32 or GDI observation. The original IPC
payload size, original three loader rows, gate result and observer-free
condition are unchanged. A zero helper exit means report delivery, not successful USER32
initialization. A positive owned-desktop result would localize the desktop or
station context; it would not prove an ACL-mask cause, because heap and other
desktop state differ too.

Local MSVC `/W4 /WX` compilation passes. The ordinary identity selftest covers
only a new desktop under its unchanged current station: both the original
Default and owned-desktop late loaders exited zero, descriptor verification
and cleanup succeeded. Removing only the private test copy of the executable
produced the real missing-file error for both probes, a nonzero selftest exit
and successful object cleanup. The real SYSTEM helper rejects this ordinary
identity with access denied before accepting any handle. Named station
creation is unavailable under this non-admin local token; an unnamed
`CREATE_ONLY` attempt also refused an existing object without changing it.
Actual SYSTEM-helper, private-token and lowbox rows remain pending hosted CI.
