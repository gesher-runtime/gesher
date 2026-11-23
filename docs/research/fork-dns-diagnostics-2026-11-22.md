# DNS evidence for manual runs in a genuine fork

Date: 2026-11-22. Base: `b915b0c46599e360bf345e85a033c6fe87d8558b`.
Related work: issue [#56](https://github.com/gesher-runtime/gesher/issues/56),
independent fork execution of the compatibility and efficiency changes.

## Observed failure and bounded change

The manually dispatched fork run
[45049466398, attempt 1](https://github.com/yom-chadash/gesher/actions/runs/45049466398),
head `1591d41dabc6436a8137f28e5cb3d9a0b5464e7e`, failed in
[native2 / native2, job 137291209995](https://github.com/yom-chadash/gesher/actions/runs/45049466398/job/137291209995).
The actual job log reaches `python3 tools/net-dns/payload.py build dist`, then
raises `RuntimeError: Wrong repository` from the canonical DNS context. The
failure occurs before that step builds or executes the DNS references. The run
was subsequently cancelled at the user's request after its upstream PR merged;
this change does not rerun it or report it as passing.

The repository API identifies `yom-chadash/gesher`, ID `1460486951`, as a fork
whose source is `gesher-runtime/gesher`, ID `1426153186`. That observed fork is a
positive test fixture, not an owner allowlist. The implementation authenticates
the actual fork dynamically and permits only manual branch dispatches.

This change adds a separate DNS diagnostic evidence type. It leaves the
canonical producer/consumer contract and selected-toolchain authority gates in
place. It changes no Android runtime behavior, substitutes no application input,
and claims no newly executed production result from synthetic test metadata.

## Two evidence types

| Property | Existing canonical path | Explicit fork diagnostic path |
| --- | --- | --- |
| Manifest schema | Integer `1` | `gesher-dns-fork-diagnostic-v1` |
| Producer label | `native2 / native2` | `native2 / native2 (fork diagnostic)` |
| Repository | Existing canonical context | Actual noncanonical fork name and ID |
| Additional scope | Existing context unchanged | `evidence_scope: fork-diagnostic`, `canonical_authority: false`, pinned upstream identity |
| Accepted events | Existing canonical policy unchanged | Only `workflow_dispatch` on a branch |
| Success label | `DNS_FULL_VERIFIED` | `DNS_FORK_DIAGNOSTIC_VERIFIED` |
| Cross-consumption | Rejects diagnostic schema/context | Rejects canonical schema/context |

The diagnostic context requires an Actions invocation, an exact 40-character
checkout/workflow/head SHA, positive repository/run/attempt IDs, the actual
workflow path/ref and a permitted producer or consumer job. Both the event
payload and the GitHub repository API response must identify the same genuine
fork. The API's source repository must match the canonical name **and** ID. The
run API response must match the same repository/head repository, run, attempt,
source, workflow, branch and manual event. Canonical names or IDs are rejected
by the diagnostic entry point, even when only one of the two is substituted.

`fork_diagnostic_context.py` uses the existing restricted GitHub GET helper and
exact run predicate. Its live entry point performs two read-only API requests,
then carries the authenticated context through one build/verify invocation.
The current environment and actual checkout are rechecked before use. It does
not write environment identities or grant a selected-toolchain role.

The default `payload.py` CLI still calls the original canonical `context()`.
Only `--fork-diagnostic` enters the new path. The native2 workflow selects this
flag using its actual repository name; the Windows DNS track makes the same
selection for verification. Noncanonical **push, pull request, pull request
target, merge queue and tag events remain intentionally unsupported**. A fork
push/PR will fail the manual-event gate rather than silently emit diagnostic or
canonical evidence.

## Actual build and execution checks remain mandatory

Both modes use the same build and verifier implementation: pinned NDK archive
identity and fresh extraction, actual compiler commands, x86-64 and AArch64 ELF
roles, private ABI/member digests, complete tracked source inventory, the Linux
reference and both signed/unsigned-char models of the pinned AOSP reference.
The existing Windows runtime loop still executes both guest ABIs through the
actual production host and matching DLL, including main, extended, safety and
negative modes and exact comparison with the independent reference output.
Its error propagation, predicates and deadlines are unchanged.

The enclosing native2 archive continues through the existing
`tools/ci/artifact_receipt.py` selection, binding the real repository,
producer, source, run and attempt before artifact download. A diagnostic DNS
manifest is not a way to satisfy the canonical schema, a selected supplier
receipt, or a different run's artifact requirements.

The reusable native2 workflow now requests `actions: read` for the two identity
queries, alongside its existing `contents: read`. The main CI caller already
grants that read permission. The Windows test-shards already have `GH_TOKEN`
and include `tools/ci` in their sparse checkout. Native2 now includes that
directory and runs both new test modules before building.

GitHub documents that a reusable workflow's `github` context is associated
with its caller, and that a local reusable workflow uses the caller's commit:
[reusable workflow reference](https://docs.github.com/en/actions/reference/workflows-and-actions/reusing-workflow-configurations),
[calling a reusable workflow](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows).
The accepted contexts therefore cover a `ci` caller and a direct `native2`
manual dispatch, with their actual corresponding workflow paths. Direct
native2 dispatch produces diagnostics but does not itself run the Windows
consumer suite.

## Validation

The following checks pass locally:

| Check | Result |
| --- | --- |
| Fork context tests | 10 unittest groups |
| DNS manifest/verifier tests | 7 unittest groups |
| Existing workflow coverage/failure propagation tests | 11 tests |
| Existing source identity tests | 14 tests |
| Existing artifact batch tests | 22 tests |
| Existing selected-source preservation selftest | 20 negative controls |
| actionlint 1.7.12 | Changed `native2.yml` and caller `ci.yml` pass |
| Git whitespace check | Pass |

The new tests exercise positive dynamic fork identities and reject wrong or
missing repository names/IDs, canonical identity substitutions, upstream source,
event, branch, checkout/head, workflow, job, run and attempt. They check exact
API endpoints and verify that the environment remains unchanged. Manifest
tests reject both cross-scope directions, wrong schemas/roles/digests, missing
or extra members and linked assets. Resealing a mutant manifest does not bypass
the existing compiler, ABI, archive, reference or ELF machine predicates.

The manifest fixtures are explicitly synthetic metadata, including nonexecutable
ELF header samples. API responses and source inventory are isolated test
boundaries. These fixtures exercise the actual verifier but do not build a
compiler, emulate Android, run Windows or supply executable artifacts.

Source preservation was checked against the exact base commit. The original
`context`, JSON reader, validator, mutation controls, digest, compiler record
and `build_guests` functions remain byte-for-byte identical. The original
canonical `context()` source has SHA256
`7a15170c498d5eb252362faed65877cac27598a54301983451467f120f6733b5`.
Removing only the added context parameter/routing and diagnostic success label
reproduces the entire original `build` and `verify` function sources. The
Windows runtime loop is byte-for-byte identical after normalizing its scope
label. Archive/source inventory helpers, AOSP reference code, artifact/run
identity helpers, selected TCP policy and minimum supplier pins remain
unchanged. No historical source receipt is rewritten or weakened.

Actionlint was obtained from its
[official 1.7.12 release](https://github.com/rhysd/actionlint/releases/tag/v1.7.12);
the Linux amd64 archive SHA256 is
`8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8`.
The local lint covers workflow syntax, expressions and call structure; external
ShellCheck and PowerShell execution were unavailable in this Linux executor.

## Remaining live evidence and separate blockers

A new Actions run at the published head must execute the real native2 build,
independent references and diagnostic verifier. The full Windows DNS receiver
additionally requires its normal host and artifact dependencies. Those live
results are not established by this local change or its metadata tests.

The selected TCP jobs still reject noncanonical identities and unsupported
manual feature-branch authority. In particular, `coherent_tcp_context.py`,
`prepare-supplier.py`, the minimum supplier factory/receipt, selected supplier
validator and host supplier consumers remain strict and unchanged. Full fork
CI is therefore **not claimed to pass**. Extending those separate roles needs
its own fully typed diagnostic producer/consumer design and review. Historical
native union source receipts also retain their original pins and are not
repurposed as diagnostic evidence.

Reproduce the scoped local checks with:

```sh
python3 -m unittest discover -s tools/ci -p test_fork_diagnostic_context.py
python3 -m unittest discover -s tools/net-dns -p test_payload.py
python3 .github/scripts/test_automerge_workflows.py
python3 -m unittest discover -s tools/ci -p test_source_identity.py
python3 -m unittest discover -s tools/ci -p test_artifact_batch.py
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
actionlint .github/workflows/native2.yml .github/workflows/ci.yml
git diff --check
```
