# Terminal CI results at PR391 head 740a484a

The main CI run is complete with conclusion **failure**. The separate ARM64 Windows workflow passed its selected-target
build and architecture checks. The ordinary WebView build passed, while its
separate runtime probe was skipped. Normal windowing is dependency-blocked by
the failed inherited compile preflight; it is not a passing or pending runtime
result.

The [paired JSON receipt](webview-ci-results-740a484a.json) contains the complete
job inventory, source identities, exact log anchors and evidence hashes. The
documentation commit carrying this receipt is not the tested source. This
report does not change PR391's head, any gate or the draft/no-merge status.

## Exact source and workflow identity

| Item | Identity |
| --- | --- |
| Tested PR head | `740a484a0586715bdeceb2393f6ac3be97edb5e6` |
| Tested base | `a62bd9a0a9bd5bcd5f428929dc52842fb938ac5a` |
| Actual test-merge checkout | `e02db283ad145e42532cfcea79c99ba71f464230` |
| Head and test-merge tree | `8cd93619615d15713d18d64f088fb3ba09f3a8ac` |
| Main CI | [45623774731](https://github.com/gesher-runtime/gesher/actions/runs/45623774731), attempt 1, `completed`, `failure` |
| Main job inventory | 67 jobs: 52 success, 6 failure, 9 skipped |
| ARM64 Windows | [45623774483](https://github.com/gesher-runtime/gesher/actions/runs/45623774483), attempt 1, success; all 3 jobs successful |

The actual checkout is established by completed build logs, including the main
build lines 127/130, ANGLE lines 116/119, ARM64 lines 123/126 and WebView lines
129/132. The test-merge
object has parents `a62bd9a0a9bd5bcd5f428929dc52842fb938ac5a` and `740a484a0586715bdeceb2393f6ac3be97edb5e6`; its tree equals the tested PR
head's tree. Source identity is not inferred merely from a workflow title or
the receipt's later commit. Pagination used `per_page=100`; all 1
page(s), 67 reported jobs and 67 unique returned job IDs agree.

## Ordinary build and runtime evidence

The integrated product build [139240649083](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139240649083) passed. Its `Build` step ran
from `2026-11-25T13:38:55Z` to
`2026-11-25T14:17:10Z`. The retained log reaches the final
`[1095/1095]` link of `dalvikvm.exe`, then completes the selected supplemental
targets. This is successful compilation and linking of the tested integration;
it does not establish execution of the blocked runtime shards.

The successful WebView build [139254235083](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139254235083) uses the actual test-merge
checkout. Its retained log records 71 storage-key selector checks, 51 current
quota-gate checks with expected negative controls, 14 discovery-barrier checks
and 36 darkening checks. These are build/isolated contract observations. The
`webview / probe` job is skipped, so it provides no live runtime result here.

The selected `compile` job is displayed as `compile-selected-minimum`. Its
failure blocks `supplier-correlation`, whose default success condition also
blocks `test-shards` and the ordinary `windows` component. Thus all fourteen
normal Windows matrix shards, including windowing, cannot run in this unchanged
attempt despite the main build's success. Always-run `test` summaries
only aggregate dependency results; they do not execute those runtime tests.
Both completed summaries record `SHARD_RESULT: skipped` and exit 1.
The exact dependency locations and observed job states are retained in JSON.

ARM64 build [139240630889](https://github.com/gesher-runtime/gesher/actions/runs/45623774483/job/139240630889) verifies nine selected PE outputs as aarch64:
platform, socket, mman, madvise, epoll and POSIX probes, timerfd and boottime
tests, and the ARM64 ELF test. Its log ends that verification with
`All binaries verified as aarch64!`. This is cross-build/PE evidence, not
Windows-on-ARM execution or full WebView runtime acceptance.

The eligible independent downstream jobs have the following observed outcomes.
These rows report job metadata; they do not imply that the blocked Windows
shards ran or add payload-level assertions from a job's title.

| Job | Evidence | Outcome |
| --- | --- | --- |
| binder2-sg-regression (windows-2022) | [139270818267](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139270818267) | success |
| web3-query-regression (windows-2022) | [139270818282](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139270818282) | success |
| web3-query-regression (windows-2025) | [139270818332](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139270818332) | success |
| core-suppliers | [139270818363](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139270818363) | success |
| binder2-sg-regression (windows-2025) | [139270818376](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139270818376) | success |
| package | [139270818479](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139270818479) | success |
| e2e (windows-2022) | [139274263976](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139274263976) | success |
| e2e (windows-2025) | [139274264038](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139274264038) | success |

## Failed gates and source-scope limits

| Job | Name | Failed step |
| --- | --- | --- |
| [139240648374](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139240648374) | artifact-preflight | Host event loop, audio state and surface geometry contracts |
| [139240648632](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139240648632) | windows-host-contracts (windows-2022) | Bound raw DNS owners with real Windows workers and loopback UDP/TCP |
| [139240649494](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139240649494) | compile-selected-minimum | Verify exact source and two-supplier preservation before native compilation |
| [139271458927](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139271458927) | test (windows-2025) | Require every Windows test shard to pass |
| [139271459009](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139271459009) | test (windows-2022) | Require every Windows test shard to pass |
| [139282547905](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139282547905) | ci-required | Require every production CI job |

The final `ci-required` job [139282547905](https://github.com/gesher-runtime/gesher/actions/runs/45623774731/job/139282547905) rejects the existing failure/skipped dependency set. Its log line 206 names `windows-host-contracts`, `artifact-preflight`, `test-shards`, `test`, `compile`, `supplier-correlation` and `windows`; line 207 records exit 1. This is an aggregate outcome, not an additional executed runtime fault.


`compile-selected-minimum` fails at the pinned reviewed-manifest SHA-256
assertion in `test-selected-policy.py`: `check():597` calls `reviewed():549`.
The named file is `reviewed-adr0088-memfd-seals.json`; native compilation has not
started in that job. `artifact-preflight` reaches the same assertion through
`test-vector-policy.py`. The retained policy and manifest blobs match base
`a62bd9a0`; this establishes unchanged source scope, not that every run of that
base necessarily fails. The retained error does not include the expected or
actual content digest, so this receipt does not report those values.

The Windows2022 host-contract failure is the existing log record
`FAIL DNS admission: exclusive peer UDP bind (native socket error 10013)`.
The affected workflow, production source and fixture/script blobs also match
the tested base. This remains existing-log and source-scope review only, without
a root-cause or environment attribution. No network/security experiment, fix,
bypass, gate change or rerun was performed for either finding. Exact source
blob comparisons and limits are retained in JSON.

The [separate actual storage receipt](webview-storage-results-18c1f813.md)
continues to record successful data/lifecycle sequences followed by the failed
final sentinel gate on both tested runtimes. This CI receipt does
not replace or convert that result into full storage acceptance. Neither
all-green CI nor completion of all compatibility work is claimed.

## Retained log identities

Every complete log below was fetched once and checked byte-for-byte against its
saved connector content. The JSON also hashes the run, pagination and
test-merge metadata. A job skipped or blocked by a dependency is not counted as
executed merely because a related build succeeded.

| Log | SHA-256 |
| --- | --- |
| `build` (139240630889) | `72690036a8af83c2fcf9d093e910495cadef56b184969f91ecbd42e4793a269c` |
| `angle / build` (139240654087) | `588f1ef90d765f6a30f2245a4c0cd47268418cfe8a830d6e5e6453ce6131f1cb` |
| `artifact-preflight` (139240648374) | `c119ca486ee7909d5d135de30b326e137e261d2c16aba7af182ff3797b285345` |
| `build` (139240649083) | `6d4632d5ae14d6090cbb48f22fdbaf0cacc4e37c67ee4ccd7b2a06827f0d13f5` |
| `compile-selected-minimum` (139240649494) | `20c89d8b3b5f54f70fc6b379e73dd7865ff30775ea3f2351cfb3ed55b23c87e3` |
| `ci-required` (139282547905) | `f9ef49430c3bc2cce7e865f19217d631d7437ba31b49377c0ab16eb54f7f5f8b` |
| `source-identity` (139238489105) | `2bba6dc236ab79f589944c7fbf6c7e66853ac2a9222764dc0538b1e572c79824` |
| `test (windows-2022)` (139271459009) | `72c930fa7db32d59fb6542acf725ad737b1adf6740748b2a66265e7569bbaca5` |
| `test (windows-2025)` (139271458927) | `bf2002d354a7b31337bdcce2725183742b3f70f4576beba297cb215e42cfc564` |
| `webview / build` (139254235083) | `24b0fb90c56c3cc6d389e8641c374bfd2127d57abb9dca37c3e437d5802aaeb8` |
| `windows-host-contracts (windows-2022)` (139240648632) | `c71b406bfa85fd594b448dc3ddd50bd062dab46b63655277e4713dc5bab2fb55` |
