# 0080. Native3 advisory POSIX and open-file-description record locks

Status: reserved implementation/design work, not verified. Dedicated branch
`wip/native3-locks`, worktree `C:\work\gesher-native3-locks`, starts at
integrationc455f432. Reserved Windows test87. Every compilation and runtime
fixture is CI-only. Root coordinates integration and all release operations.

## Problem and existing behavior

The bionic Fcntl wrapper in posix_bionic.cc reports every requested lock as
granted and every queried range unlocked. The common POSIX fcntl supports
integer commands but does not implement record locks; its argument storage
is host32-bit long and cannot carry a Linux64-bit flock pointer. flock's
separate open-description lock remains implemented and must not change.

## Proposed implementation boundary

Implement both POSIX process-owned and Linux OFD-owned record locks for
regular files. Normalize SEEK_SET/CUR/END and signed start/length with
checked64-bit arithmetic; zero length extends through future EOF, negative
length covers bytes preceding the start. Validate type, access rights,
read/write ownership, pointer/layout and required OFD l_pid zero. Queries
return real conflicting ranges/types/PIDs (OFD owner PID is-1), leaving a
nonconflicting query's other fields intact. Own locks can split/coalesce or
convert; POSIX and OFD locks still conflict with each other, including within
one process. Record locks remain independent of flock.

Use an advisory coordination registry keyed by stable file identity, rather
than Windows mandatory locks over the data. Each update/query is serialized
across processes, and blocking waits release coordination before sleeping.
Registry capacity failures must fail explicitly rather than drop existing
locks. Dead process/PID-reuse cleanup must compare process creation identity,
and abandoned coordination ownership must preserve or recover a consistent
published snapshot. Tests must exercise genuine independent processes.

POSIX locks belong to a process and any close of that file drops its locks.
OFD locks belong to one open description, share through dup/dup2/F_DUPFD and
are dropped only on its final close. Replacing a dup2 target first closes its
old description. Descriptor numbers and native handles may be reused; neither
alone is a durable owner identity. Audit common close/dup paths and stdio
fclose/freopen, retain integer fcntl behavior, and dispatch pointer-sized lock
arguments before reading integer varargs. ARM flock ABI must be converted
through its existing guest wrapper without truncating offsets or pointers.

Fork is not implemented by Gesher and this change does not claim otherwise.
Document the unsupported fork boundary explicitly; prove relevant ownership
and inheritance with supported independently started/duplicated processes.
Signal interruption/deadlock detection and noncooperating Windows programs
must be evaluated against the actual promised POSIX/Linux scope rather than
silently excluded. No API/gap is marked closed by this design.

## Verification required before acceptance

An actual Linux CI reference must run the same operation contract against
native fcntl; a handwritten list of expected successes is insufficient.
Both Windows runners must execute real common-layer and bionic ELF clients
for both ABIs, plus JNI/single/device contexts as appropriate. Require actual
conflicts, blocking/unblocking, read/read sharing, advisory data I/O, query
normalization/PIDs, range splitting/conversion, invalid arguments/errors,
OFD/process distinction, any-close/final-close/dup2/stdio lifetime, descriptor
reuse, independent files and flock, process crash cleanup and contention.
No unchanged red retry, weakened assertion or symbol-only acceptance.

Bounded independent peer review is required for the coordination design and
final pointer/layout/lifetime source before qualifying CI. Full coherent
Windows suites/package/install/run/uninstall/both complete e2e remain the
integration acceptance gate. Native API inventory proof/history is append-only.
