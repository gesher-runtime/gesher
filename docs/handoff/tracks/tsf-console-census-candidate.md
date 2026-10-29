# TSF and Services controller: bounded console and census work

This is CI-only supervision of the fixed trusted TSF and Services probes on a
trusted runner. The inherited console is borrowed, not an owned or authenticated
console host. Console membership proves attachment, not authority or exclusive
access. Standard handles are explicitly whitelisted; output files allow sharing
during execution and the final exclusive read proves retirement of open writers,
not protection against a hostile same-user writer that opened and closed a file
earlier. This assumption does not apply to Binder or guest trust.

Run 40869317576/source 9c7de861 was red on both Windows controllers: after an
exited retained child passed its parent query, a second PID from the older job
list could not be opened (Win32 87). Unknown unretained identities must never be
authorized by recensus. Raw logs are in `C:/work/gesher-ci-review/tsf-408693-controller-{2022,2025}.log`.

Run 40869961275/source 57283386 passed both controlled and actual TSF jobs: the
retained-child exit and Toolhelp absence, denied parent query, wrong PID, unknown
child, orphan, retirement and output controls passed; both actual TSF probes had
zero failures. This is bounded parent-query proof, not combined full acceptance.

Run 40871088307/source cba69331 passed both TSF jobs with a one-process limit:
the same child ran in the unrestricted positive control; the restricted attempt
failed with Win32 1816, no child marker, total one, and clean root retirement.
The later Services run 40871529177 was red: detached PowerShell exited zero
without executing its script. Runs 408728 and 408763 distinguish attachment and
console creation; the runner has a console even though its HWND is zero. Run
40877220121 proved the inherited-console diagnostic script and both streams on
both Windows with total one, but actual Services remained red. These diagnostic
runs do not qualify the current candidate or a combined full.

The active-process limit applies to simultaneous job
members; the cumulative total-one check is independently required. Any vanished
unretained identity or unproved cleanup remains a failure, not a trusted host.

## Bounded component evidence

Exact source `c228a04efbe6d6ad51a7009c69f0067cfc3ccbf3` passed TSF component
40880552104 and Services component 40881149995 on Windows 2022 and 2025.
Both TSF controllers executed the live-retained exit/Toolhelp-absence control,
parent-query denied and wrong-PID controls, actual unexpected child and orphan,
invalid/missing/real changed-console membership controls, original lifecycle,
output cap, termination/query denial and handle retirement controls. Matched
unrestricted child executed; limited child was rejected with Win32 1816, no
marker and exactly one job process. Both actual TSF probes emitted 71 store and
three service PASS records with zero failures, total one and 7/7 handles closed.
Producer receipt: run 40880552104 attempt 1, job 123470923924, archive digest
`sha256:3bda14309ed34002ca18adec1aa99a2abf1aeaf8efb383b92e46a1d073f3f66b`.

Both Services runners confirmed all source pins and the same controller controls;
the noncooperative worker reached both barriers and timed out, then retired with
total one and 7/7 handles closed. Each compiled the helper in-process with pinned
Roslyn, ran real WinRT success/error/timeout/late bridge controls, and called the
actual Maps provider, which returned `InvalidCredentials` after a completed task
and disposed resources. Compiler, fixture and provider each had total one,
7/7 handles closed and no errors. The two Windows logs and four TSF controller/
runtime logs are in `C:/work/gesher-ci-review/` under `services-408811-*` and
`tsf-408805-*` respectively.

The exact component source remains unintegrated. A separately reviewed combined
guard/source-map update and fresh integration full remain required. The policy
and console observations are scoped to these fixed trusted CI probes; they do
not establish general process admission, hostile same-user output integrity, or
Binder/guest isolation. An unopenable prior PID still fails closed.
