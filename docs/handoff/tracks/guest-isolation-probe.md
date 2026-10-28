# Guest isolation disposable experiment

Worktree: `C:/work/gesher-guest-isolation-probe`, branch
`wip/guest-isolation-probe`, integration base `f1550540`.

The initial `065f66b0` checkpoint lacked host IPC, independent trusted code/config
write denials, an explicit allowed resource challenge, strict receipts and final
cleanup verification. Implementation `15797ccc` was source-reviewed and corrected
in `9948b68cd1b6b1121576b7720ad9a45baba0e421`. The sole integrator source peer
accepted that corrected source before its first push. Lightweight Python/YAML
and embedded PowerShell parsing and `git diff --check` passed locally. No local
native/Java/C# compilation or fixture was performed.

Run `40783239100`, attempt 1, at `9948b68c` completed red on both Windows:
the actual suspended AppContainer SID and owned job were proved, but child exit
17 means strict host IPC denial was not established. Build, pinned upstream
archive, actual llvm-mingw -Werror compilation, producer archives, compiler
receipts and 16 receipt mutation controls passed. The saved plain logs are under
`C:/work/gesher-ci-review/guest-40783239100-*`. Missing raw open handle/error
prevented distinguishing successful access from a different failure.

Diagnostic-only changed source `c2d667eaa3dc6d53eda9ffeadcf0d189d358a955`
adds actual pipe owner/group/DACL, suspended child token SID/user/groups,
restricted SIDs/capabilities/integrity and host-positive open evidence. Child
raw handle, immediate Win32 error and requested access travel over the existing
explicit pipe and are printed before the parent rejects a failed child exit.
The exact ERROR_ACCESS_DENIED requirement, ACLs, success rows, ownership and
cleanup controls remain strict. No root cause or correction is claimed.
The sole integrator source peer accepted this diagnostic diff before its push.
`git diff --check` passed; no local compilation or fixture was performed.

Diagnostic run `40783823671`, attempt 1, exact `c2d667ea`, completed red on
both Windows. Actual raw child handle was INVALID_HANDLE_VALUE, error 2
(FILE_NOT_FOUND), requested access `0xc0000000`; the host positive opened its
LOCAL endpoint. Actual token had the expected AppContainer SID, low integrity
4096, zero capabilities and no restricted SIDs. The protected pipe DACL was
OW-only; owner was BA. This proves the attempted LOCAL endpoint was unreachable
from the child, not that its ACL denied access. Saved plain logs are
`guest-40783823671-job-123152001909.log` and `...1957.log` under the review folder.

Changed-source scoped run: `40784976349`, attempt 1, exact
`4458ce295721d56e0bbd8de8afbfaeba9d841fa1`. Its source peer accepted the
diagnostic design before push. The endpoint now uses the unpackaged host path.
Before requiring denial, the same server handle/name temporarily grants only
the exact AppContainer SID read/write access at low integrity. An actual fresh
isolated connection must report the host server PID and send the exact inherited
resource challenge through that endpoint. After the child closes, the parent
restores the exact protected OW-only DACL, checks actual ACEs/SIDs/masks and the
low mandatory label, proves fresh host access, and releases the child to attempt
fresh read/write access. Only INVALID_HANDLE_VALUE plus exact error 5 passes.
Both initial and tightened descriptors are read back and asserted. No namespace
or ACL compatibility, strict denial or isolation is claimed before actual CI.
The original LOCAL failure remains recorded; error 2 never qualifies denial.

Run `40784976349` completed red on both Windows at stage 62, after a valid
isolated read/write positive connection with error 0, actual host server PID and
exact challenge. The initial descriptor had the expected exact AppContainer ACE
and low mandatory label; assertions passed. The combined disconnect/protection
transition failed before its second descriptor readback. Exact denial was never
reached. The owned server handle requested duplex data rights but omitted
WRITE_DAC, which [CreateNamedPipe requires explicitly](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createnamedpipea)
for DACL modification. This source oversight was identified after push and
retained with the actual red run. Saved plain logs are the three
`guest-40784976349-job-*.log` files under the review folder.

Source candidate `1e90373f2f7f3e785454d823bc053ade83163ccf` adds WRITE_DAC
only on the owned host handle and captures separate raw disconnect result,
SetSecurityInfo returned Win32 status, and descriptor assertion phase/result.
Guest access, ACLs, mandatory label and exact error-5 gate are unchanged. Source
peer passed before push. Run `40785726716`, attempt 1, exact
`d549283d32aec04b0fed0ec56bfebc966f1e2120` completed green: all three jobs
succeeded. This worker independently read the saved terminal metadata, actual
build log and both Windows runtime logs. No local compilation or fixture was
performed.

Both actual runners proved an isolated positive read/write connection to the
same host server PID and exact challenge, successful disconnect, SetSecurityInfo
status 0, protected OW-only DACL with low mandatory label and successful actual
descriptor assertion. After fresh host access and relistening, the child raw
handle was INVALID_HANDLE_VALUE with exact error 5. All ten child success rows,
unchanged trusted files, child exit 0, owned child/files/profile cleanup and
RunTimed reader retirement were present. Actual key rows include:

```
PASS parent same-endpoint-isolated-positive-challenge fresh-host-positive protected-dacl
DIAG child host-ipc-open handle=FFFFFFFFFFFFFFFF invalid=1 error=5 requested=0xc0000000 expected-error=5
PASS child host-ipc-denied error=5
PASS parent trusted-files-unchanged host-ipc-positive=1 child-exit=0
PASS parent owned-child-handles-files-profile-cleanup
```

Pinned archive verification and actual clang 23.1.3 llvm-mingw -Werror compilation
passed. Producer build job `123158301517`, archive `12859516194`, digest
`sha256:dc84857eb8fef9c1b71c172769ab8e658e8bd2ad1ba5ab2f70f411090d7d2e38`
were identical in both exact-source/run/attempt archive receipts; both also
passed all 16 strict compiler receipt mutation controls. Runtime jobs are
`123158393883` (2025) and `123158393928` (2022). Plain logs and terminal JSON
are saved as `guest-40785726716-*` under `C:/work/gesher-ci-review`.

This closes only the disposable fixture's strict denial diagnostic. No production
launch migration, authenticated Binder admission, secondary AOT, full-suite,
package, integration, main or release qualification follows from this component.
Previous same-ref runs were terminal before each account-gated gh-credential
HTTPS push. No duplicate dispatch was used.
Watch uses `gh run watch --exit-status --interval 120`. No unchanged red retry
is authorized; retain actual failures and make a concrete correction first.
The sole integrator owns the single watcher and terminal runtime/receipt audit;
this worker has no observer. Read the new diagnostics before proposing any
correction. Neither the old red run nor a diagnostic-only run qualifies isolation.

Addressing references: Microsoft's [IPC documentation](https://learn.microsoft.com/en-us/windows/apps/develop/communication/interprocess-communication)
describes AppContainer pipe namespace constraints; its
[CreateFile documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea)
distinguishes missing pipes from busy instances. The changed address remains
an empirically gated hypothesis: only the actual same-endpoint isolated
positive followed by strict ACL denial can qualify this disposable fixture.

The fixture starts its child suspended with SECURITY_CAPABILITIES, zero
capabilities, an explicit two-handle allowlist and atomic JOB_LIST membership in
a kill-on-close job with one active process. Both parent and child check the
actual AppContainer SID. Each process/thread right is tested independently with
host-positive controls. Separate readable existing code/config markers must deny
writes and remain unchanged. A live listening host-only pipe must deny opening
after the host successfully opened it. Explicit resource input/output pipes
carry a fixed challenge and exact observations. Owned child cleanup requires a
signaled process within the deadline; unique files/directory/profile cleanup
errors fail. No pending overlapped I/O is left on the host listener.

The workflow uses the qualified existing RunTimed supervisor, current-run and
attempt producer archive receipts, exact member inventory, source/executable
hashes and compiler invocation/binary/version provenance. The upstream toolchain
archive SHA256 is pinned from release asset `618062377`; no unverified toolchain
cache is consumed. Receipt mutation controls run before either Windows launch.

This is a disposable Win32 capability experiment only. Production guest launch
still uses an unrestricted same-user host boundary. An unrestricted same-user
process remains trusted. No production guest isolation, Binder authentication,
secondary AOT, integration, full-suite, package, main or release qualification is
established by this component. The production security requirement remains open.
