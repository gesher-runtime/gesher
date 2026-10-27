# 0076: Apply spawn file actions inside the child launcher

Status: proposed

## Context

Pinned android-16.0.0_r4 bionic libc/bionic/spawn.cpp applies attributes and
ordered file actions after fork/vfork. The parent returns0 and a real PID;
failed open/dup2/chdir/fchdir or exec ends that child with127. Failed close
is deliberately ignored. dup2(fd,fd) explicitly clears FD_CLOEXEC.

Gesher's old implementation opened files in the parent and built a handle
map. It swallowed open/dup2 failures, treated chdir as a final string and
ignored fchdir. Parent-side materialization cannot implement relative opens
or failure short circuit without changing shared parent state.

## Decision

Start the actual gesher-exec child with a versioned private argument
protocol containing its program/argv, the caller's PATH, descriptor flags
and ordered actions. Reuse the existing Windows argument quoting, explicit
handle inheritance, environment block and real child tracking/waiting.
After decoding, use the actual bionic open/close/dup2/chdir/fchdir/fcntl APIs
inside that new process. Restore inherited close-on-exec flags before the
actions; close remaining flagged descriptors after them. Preserve ignored
close errors and stop on the first other action failure.

Resolve the program after directory actions. posix_spawnp uses the caller's
PATH independently of replacement envp. Enter ELF/sh/tool programs in this
same child, so the returned PID is the program's PID. Failed setup/lookup
returns127 from that real child. Existing PE/batch execution retains its
wait-and-propagate convention and an additional process.

Use the existing arm64 open-flag conversion for addopen too. Route bionic
chdir through gesher-base's UTF-8 directory API, including fchdir's resolved
path. No signal/scheduler/ID attribute completion is inferred.

## Verification and limits

Source only; compilation, actual NDK children for both ABIs, parent isolation
and failure short-circuit tests are pending CI (Windows test77).
Test74's separate registration gates remain unchanged.

The existing inherited-fd snapshot scans below2048 and Windows transport
supports fewer than7000 descriptors. Complete descriptor limits, socket/
device-state inheritance, spawn attributes, true fork/image replacement
and console delivery remain explicit Exec4 work. Windows command-line
capacity can reject oversized encoded requests with E2BIG before a child
is created. A child setup error is never synthesized as a fake wait status.

UCRT's unbound standard stream sentinel is-2, distinct from invalid fd-1;
neither is a file handle. Snapshot/child sweep code excludes those exact
values before Win32 handle APIs. The transport's temporary NUL standard
handles are closed before actions if the parent slot was absent. The CI
probe requires an actual no-console raw-2 parent and real NDK child closed0,
rather than manufacturing a wait status.
See [Microsoft's _get_osfhandle contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-osfhandle?view=msvc-170).
These additional runtime controls remain pending full40502225202.
