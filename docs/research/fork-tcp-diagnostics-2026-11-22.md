# Selected TCP diagnostics in a genuine fork

Date: 2026-11-22. Base: `0da2b7f18fa8013932a6cb77dac32423df9ef8ab`.
Related: issue [#56](https://github.com/gesher-runtime/gesher/issues/56) and
the separate [DNS fork diagnostic change #281](https://github.com/gesher-runtime/gesher/pull/281).

## Observed blocker

Manual fork run
[45053128467, attempt 1](https://github.com/yom-chadash/gesher/actions/runs/45053128467)
uses head `66c8107ea8d4f7d4b22fedd2c04f6c12d7d97e32` in
`yom-chadash/gesher`, repository ID `1460486951`. Its
[compile-selected-minimum job 137302455487](https://github.com/yom-chadash/gesher/actions/runs/45053128467/job/137302455487)
fails at the branch assertion in `coherent_tcp_context.py`, before compiler
setup. The
[Linux reference job 137302455533](https://github.com/yom-chadash/gesher/actions/runs/45053128467/job/137302455533)
has the same failure. The actual compile log and run/job API metadata bind that
failure to the stated head and attempt. Existing canonical authority correctly
rejects this manual feature-branch fork context.

Changing that first assertion alone would leave independent canonical gates in
the minimum factory, supplier verifier, manifest, Windows receiver and supplier
correlation. This change instead adds a distinct diagnostic type throughout
that chain. It changes no Android runtime operation, application input,
capability or success result, and claims no new runtime speedup.

## Authentication and separate evidence

The new `fork_tcp_context.py` permits only `workflow_dispatch` on a branch in a
genuine noncanonical fork of `gesher-runtime/gesher` (ID `1426153186`). It verifies
both repository name and numeric ID against the event and repository API, and
requires the API source repository to match the canonical upstream. The run
API must match the actual run, attempt, head, repository/head repository,
workflow, event and branch.

The checkout, workflow SHA and source-identity job's authorized source must be
equal. The workflow ref and policy ref use the **real fork name**. The four
allowed jobs are compile, Linux reference, Windows and supplier correlation;
non-reference jobs must select the existing minimum recipe. A descriptor also
declares `scope: fork-diagnostic` and the boolean `canonical_authority: false`.
Canonical name/ID substitutions, other events and jobs, stale or foreign runs,
and malformed descriptors fail before supplier work. No environment identity is
rewritten. A command-line switch selects an entry point; it cannot supply its
authentication or change an existing receipt's type.

| Boundary | Existing canonical evidence | Fork diagnostic evidence |
| --- | --- | --- |
| Context | Existing coherent TCP context | `gesher-tcp-fork-context-v1` |
| Publisher baseline role | `publisher-reference-unmodified-v1` | `publisher-reference-fork-diagnostic-v1` |
| Minimum receipt role | `gesher-winpthreads-selected-minimum-v1` | `gesher-winpthreads-fork-diagnostic-v1` |
| Minimum output file | `gesher-winpthreads-authority.json` | `gesher-winpthreads-fork-diagnostic.json` |
| Supplier envelope role | `gesher-winpthreads-selected-minimum-v1` | `gesher-tcp-supplier-fork-diagnostic-v1` |
| TCP envelope schema | Existing envelope unchanged | `gesher-tcp-fork-diagnostic-v1` |
| Artifact prefix | `tcp-coherent-selected-minimum` | `tcp-fork-diagnostic-selected-minimum` |

Every diagnostic minimum, publisher, supplier and TCP envelope binds the same
authenticated descriptor. Diagnostic consumers reject canonical roles and
canonical consumers reject the diagnostic roles/identities. The original
recipe pin is retained verbatim, including its original recipe label; it
describes the twelve input/output sources and two patches and does not grant
the diagnostic issuer canonical authority.

## Build and runtime preservation

The original canonical `coherent_tcp_context.py`, minimum `context()`, canonical
minimum shell, immutable source/patch pins and canonical supplier CLI entry are
unchanged. Factual source, archive, SDK, ABI, header, alias, PE, compiler and
object checks are extracted into common functions behind the strict canonical
and diagnostic entry points. Canonical wrappers still issue their original
roles with the same fields and predicates.

`build-minimum-diagnostic.sh` has its own authenticated entry and typed receipt
calls. Its mechanical recipe from fresh compiler/source resolution through
configure, SDK compilation, make, install and final verification matches the
unchanged canonical shell byte-for-byte after replacing only the receipt CLI
path. It uses the same archive and upstream commit, same ordered patches, SDK601
and ABI480/all-field checks, fatal compiler warnings, source object proof,
exports/imports, public headers and aliases. It writes no canonical authority
file. Its bounded receipt log transport has a separate prefix that the original
canonical reconstruction function rejects.

The outer supplier compiler body is shared. It creates a separate fresh
`tcp-fork-diagnostic-supplier` workspace and records the actual diagnostic
factory command and log. The TCP manifest still verifies the actual host,
translator, both NDK guests, compiler commands, linker inputs and symbols,
import policy, guarded UDP references and complete tracked source inventory.
The original complete thirteen-member role map is shared as immutable data
between the producer and diagnostic artifact verifier.

The Windows receiver retains its exact pinned download/controller helpers,
source/run/attempt/artifact predicates and all actual execution gates. The
runtime tail is byte-for-byte unchanged: host and both ABI paths, guarded UDP
and fault/negative cases, supplier retirement, cancellation, process ownership,
census, finite waits and TCP assertions all still execute. Controller deadlines
are unchanged. The diagnostic switch authenticates the real fork before
download and chooses a distinct artifact name and strict receipt verifier.

The current full product build on feature branches selects the existing
publisher cancellation repair. Diagnostic correlation verifies that actual
host artifact and its current publisher recipe/context alongside the complete
typed TCP artifact. It rejects a selected full-host receipt and explicitly
reports that a selected full-host byte comparison is not applicable to a
publisher host. It does not expand selected full-host authority or invent
library equality. The canonical correlation implementation remains unchanged.

The existing artifact download and run identity helpers are not modified.
Workflow jobs retain their dependency graph, failure propagation, matrix and
timeouts. Compile and Linux reference receive the existing read-only token
capability for API authentication. The new negative/source tests run in the
normal artifact preflight and selected compile preflight. Noncanonical pushes,
PRs, merge queues and tags remain unsupported; they are not relabeled as manual
diagnostics.

## Local verification

| Check | Observed result |
| --- | --- |
| New diagnostic suite | 18 unittest groups pass |
| Canonical supplier behavior against immutable base | 564 leaf mutations agree |
| Existing coherent TCP context | 249 negative controls pass |
| Existing minimum supplier entries | 4 tests pass |
| Existing selected supplier metadata | 36 semantic mutations and authority-digest negative pass |
| Existing minimum SDK/ABI/layout/map metadata | 46 controls pass |
| Existing selected-source preservation | 20 negative controls pass |
| Existing correlation metadata | 12 mismatch controls and alias control pass |
| Existing workflow coverage | 11 tests pass |
| Existing artifact batch / source identity | 22 / 14 tests pass |
| PowerShell pinned helper checks | All 9 exact helper pins, 23 negatives and 4 controller-budget controls pass |
| PowerShell artifact and producer predicates | Original three predicates preserved; 72 artifact and 10 producer negatives pass |
| PowerShell retirement checks | 20 mutations pass |
| actionlint / Bash syntax / whitespace | Pass |

The new tests compare the actual extracted factual function bodies with the
immutable base and compare the canonical supplier validator on 564 mutations,
resealing authority hashes so that internal checks are exercised. They cover
both cross-type directions, wrong repository/source/event/run/attempt/job,
changes to every envelope level, compiler/SDK/ABI/patch/object evidence,
unexpected/missing/linked/modified members, role mismatches and publisher-host
correlation misuse. Their fixtures are explicitly synthetic metadata and
nonexecutable files. API/git reads and the preexisting host artifact
authentication are declared test boundaries; the new diagnostic authentication,
supplier checks, whole-member verification and correlation logic run unchanged.

PowerShell checks ran with the official portable PowerShell 7.4.22 Linux x64
release, archive SHA256
`9de443bef974f219c98a33b0f4096aace7f30b42188f37a1b14c5da7e82c8a6a`:
[official release](https://github.com/PowerShell/PowerShell/releases/tag/v7.4.22).
Workflow lint used official actionlint 1.7.12, archive SHA256
`8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8`.

## Limits and historical source gates

No local compiler execution is presented as an Actions-authorized supplier
build. No Windows native process, controller, Android guest or physical device
was executed by these metadata tests. The published-head fork run must establish
the actual cross-build, Linux references, typed artifact reception, correlation
and Windows runtime outcomes. Passing these local checks does not establish
that the complete fork CI suite passes.

An extra attempt to run the legacy `test_host_pthread_interface.py` CLI could
not load its hardcoded historical source-policy commit
`1bf8fad9ec3b423ddef8c97c1d72b25790974364` in this checkout. That CLI's historical
source proof also describes the old first-job publisher/selected transition,
not the present publisher-cancel workflow. It is not invoked by current
`ci.yml`; it is **not reported as passing**. Its gate was not changed or skipped.
Historical first-link/native-off/union source receipts and their immutable pins
are also preserved, not reused as evidence for this new source snapshot.

## Upstream release-context composition

Upstream commit `20252fde18db387b4d99d99f7050d6336f58bf6e` subsequently added
canonical `push` events for version tags to the canonical context. The
diagnostic source-preservation fixture now compares that context and its test
byte-for-byte with this exact upstream commit. Every other original baseline,
minimum factory/input/patch pin, factual comparison and canonical supplier
mutation comparison remains at `0da2b7f18fa8013932a6cb77dac32423df9ef8ab`.
The fork context is unchanged: all four roles reject both a push and a manual
dispatch of `refs/tags/v0.2.0`, with matching workflow refs, at the explicit
manual-event or branch gate. The 18 diagnostic groups and 564 canonical
supplier comparisons pass, as do upstream's four canonical roles and 273
negative controls. This is a test-only qualification of an upstream policy
extension, not a new authority or a generalized source-normalization rule.

Reproduce the active checks with:

```sh
python3 tools/net-tcp-priority/test_fork_diagnostic.py
python3 tools/toolchain/test_coherent_tcp_context.py
python3 tools/host-pthread-authority/test_minimum_supplier_entry.py
python3 tools/net-tcp-priority/test-selected-supplier.py
python3 tools/host-pthread-authority/minimum_receipt.py selftest
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/toolchain/test_coherent_supplier_correlation.py
python3 .github/scripts/test_automerge_workflows.py
python3 -m unittest discover -s tools/ci -p test_artifact_batch.py
python3 -m unittest discover -s tools/ci -p test_source_identity.py
pwsh -NoProfile -File tools/net-tcp-priority/test-helper-metadata.ps1
pwsh -NoProfile -File tools/net-tcp-priority/test-receipt-metadata.ps1
pwsh -NoProfile -File tools/net-tcp-priority/test-retirement-metadata.ps1
actionlint .github/workflows/ci.yml
bash -n tools/host-pthread-authority/build-minimum-diagnostic.sh
git diff --check
```
