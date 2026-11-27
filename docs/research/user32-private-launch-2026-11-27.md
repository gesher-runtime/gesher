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
