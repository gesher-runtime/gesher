# Native internal kernel-wait witness (unverified source)

Base: qualified Native source1037c8f103a341b5b733a78c064efd1db2b54e72,
component40782744623. Separate tree/branch: native-kernel-witness.
This extends internal finite waits only. Public7/38, signals, EINTR/SA_RESTART,
OFD blocking and the remaining Native inventory stay open.

The dedicated fixture compiles the real registry and record_lock_wait.cc with
GESHER_RECORD_WAIT_WITNESS_TEST. An isolated callback records QPC immediately
before/after the actual WaitForMultipleObjects, after registry unlock. It
does not signal, wait, acquire a mutex, or modify the wait result. Production
gesher_posix and the previously accepted fixture lack the callback definition;
actual archive/object symbol checks enforce this in CI.

An external controller holds the actual file lock in a distinct live process.
The fixture runs the internal wait against that conflict, then cancels and
joins it. A scoped system ETW session records actual fresh Thread Start and
CSwitch events. TDH decodes named properties and exact scalar sizes; unavailable
metadata, missing privilege, trace errors, event/buffer loss, missing birth or
missing transition fail closed. RAW_TIMESTAMP preserves the QPC clock.

Acceptance requires one actual Start for this PID/TID before the phase, a
retained thread handle whose FILETIME identity matches, CSwitch old TID with
state5 Waiting, mode1 UserMode, reason6/13 UserRequest during the actual phase,
controller cancellation after that event and before phase return, and a
subsequent schedule-in in the same phase after cancellation. Collection duration
and registered requests are never proof. This is a transition witness for that
phase; ETW does not identify the event object and this is not a simultaneous
snapshot of current state. No WCT event-wait claim is made.

Controls require another real event-waiting thread with its own birth/wake,
a genuinely kernel-observed runnable thread rejected as waiting, a different
retained thread rejected, a stale creation identity rejected, a phase after
thread completion rejected, unsupported observability rejected, and an actual
pre-signaled cancellation returning ECANCELED without entering any wait phase.
All threads have finite waits and external controller25s execution bounds.
Owned trace storage is64MB maximum; normal cleanup stops the session, joins
workers, closes retained handles, deletes ETL and proves the named session absent.
Failure cleanup independently retires the native process, invokes a bounded
native cleanup helper for the exact native process-owned logger, proves
ControlTrace query returns ERROR_WMI_INSTANCE_NOT_FOUND, and always retires
the holder. Errors are aggregated while preserving the original failure.

The new component retains inherited strict archive helper controls, producer
run/attempt/source/compiler receipts, actual161 comparator and13 duplicate
controls, cleanup controls and both Windows runners. Its explicit artifact
inventory adds only the dedicated executable/imports and its symbol listing.
It does not change accepted supplier/full workflows or Native coverage counts.
Independent source review is required before push/CI. No local native build or
execution has occurred; no CI result or closure is claimed.

Primary Microsoft references (reviewed2026-10-29):
- https://learn.microsoft.com/en-us/windows/win32/etw/cswitch
  documents Waiting5, UserMode1, and UserRequest6/13.
- https://learn.microsoft.com/en-us/windows/win32/etw/thread-v2-typegroup1
  distinguishes actual Start from DCStart rundown and defines PID/TID.
- https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracea
  documents named system loggers and scope/cleanup restrictions.
- https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_properties
  documents QPC clock, THREAD/CSWITCH flags and loss counters.
- https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_logfilew
  documents raw timestamps and consumer loss reporting.
