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
