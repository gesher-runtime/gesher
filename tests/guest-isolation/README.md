# Bounded AppContainer source experiment

This disposable fixture investigates a prerequisite for a future guest isolation
design. Production guest processes still run under the unrestricted same-user
host boundary. This probe does not modify that launch path, qualify Binder
authentication, qualify secondary AOT, or close the production security gap.
An unrestricted process controlled by the same Windows user remains trusted and
can change the fixture, its files, launch policy and host process.

The parent creates a unique profile and unique owned directory. Only that profile
receives read/execute access; owner rights preserve host management. It starts a
child suspended using SECURITY_CAPABILITIES with zero capabilities and an exact
two-handle inheritance allowlist. Before resume, it verifies the actual process
token's AppContainer flag and SID, and assigns its child to a kill-on-close job
with an active-process limit of one. The child independently checks its token,
then requires ERROR_ACCESS_DENIED for process injection/duplication rights,
thread context/suspension rights, independent writes to readable existing trusted
code and config markers, and opening a real host-only named pipe. The parent
first opens that pipe successfully and leaves it listening without pending I/O.
The allowed inherited input pipe supplies a fixed challenge, and the allowed
output pipe returns exact observations. No handles to host process/thread,
trusted files, host pipe, job, or other parent's pipe ends appear in the list.

The child has a 15-second deadline, with owned termination and a five-second
termination wait on failure. Parent response reads first peek available bytes;
there are no indefinite reads on retained writers. All handles are scoped; parent
checks both file contents remain unchanged, and deletes only its three files,
owned directory and newly created profile. Cleanup errors fail the fixture.
The CI invocation uses the existing qualified RunTimed supervisor with its
independent process, reader and cleanup deadlines.

CI cross-compiles the actual source using pinned llvm-mingw and -Werror, then
uses current-run/attempt/producer artifact archive receipts on both Windows
versions. A strict member inventory plus source, executable and actual compiler
invocation/hash/version receipts precede execution. No CI artifacts are fetched
to the developer machine. Source peer review precedes the first changed-source
CI push. Runtime qualification remains pending until both real runner logs are
audited; success would qualify only this isolated experiment.
