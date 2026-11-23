# Bounded recovery for the observed unframed GitHub EOF

Three actual CI consumers downloaded a verified artifact and then failed a
required producer-jobs read with the sole diagnostic `unexpected end of JSON
input`, a `gh` exit status of 1, and no recorded HTTP framing. The reader treated
this as an unclassified failure, so the existing complete-inventory fallback
was never reached. This repair recognizes only that exact failed-read shape.
It discards the subprocess output and uses the existing finite read policy;
every producer, source, archive and post-download check still executes.

The original failures do **not** establish why the remote read ended. In
particular, they do not establish quota exhaustion for that read, and they did
not archive the failed subprocess stdout. No live GitHub recovery, Windows or
native runtime result is claimed by the host qualification below.
The new path is demonstrated for the **unframed** form of the diagnostic. Since
stdout was not retained, the logs alone cannot establish that every historical
request met this guard or would recover on a later request.

## Actual failure evidence

| PR and job | Tested source / checkout | Boundary reached |
| --- | --- | --- |
| [#309 SG, job 138010494677](https://github.com/gesher-runtime/gesher/actions/runs/45249096332/job/138010494677), run attempt 3 | `35083c4736b4753079e79ac7fa002625abf07950` / `58082d0502332a63bd09baa1ae1d77f17c76606d` | Current-attempt jobs snapshot after the authentication archive was verified. |
| [#330 TSF, job 138033554385](https://github.com/gesher-runtime/gesher/actions/runs/45271588999/job/138033554385), attempt 1 | `6ed97f62c136268d93153c7d6dc9c3875306052c` / `c60df45e35b5e98a1c4ffefbaefb835e1a65f77a` | Current-attempt jobs snapshot after the authentication archive was verified. |
| [#429 native blocking, job 138043743970](https://github.com/gesher-runtime/gesher/actions/runs/45273973618/job/138043743970), attempt 1 | `036a7fb6a1e44019f18295cb4bd849eae170e1b0` / `2c3b462d2ea3a07ca35941b7dddb4eefa1ce46b0` | Original-attempt snapshot after the initial batch archive was staged, before the authentication archive. |

All three traces reach the actual `github_jobs._single_page` call with
`retry_transient=False` and stop with `GitHub GET failed after 1 attempt(s)
(gh exit 1)`. Their complete logs and exact failed-checkout reader, inventory
and producer-context sources are retained in the evidence archive. The reader's
decision functions match this repair's baseline as syntax trees after removing
docstrings; inventory and producer-context files match byte for byte. Earlier
failed readers had the older quota-wait constant; this repair preserves the
current main constant and does not attribute those failures to that difference.

The two TCP authority failures on #429 are separate: its PR targets a stacked
branch while the existing coherent producer requires canonical `main` as the
PR base. This repair does not change that authority predicate or the author's
branch. It does not claim to make the entire PR green.

## Exact behavior and unchanged authority

The baseline is public main
`5882d0a44765be56562ba25443b8572b6466712a`. The local source checkpoint is
`3d1599f130906aa3778699209690db7c8a3d5854`; the companion JSON and archive bind
every source blob independently of local commit availability. The reader's
new Git blob is `54ec7099da2accee2daa783ff381e971369d3196`.

Only all three of these conditions admit the new recovery path:

1. The subprocess exits with status **1**.
2. No HTTP response has been parsed from its output.
3. Its complete stderr is exactly `unexpected end of JSON input`, with either
   no terminal newline, one LF, or one CRLF.

The failed stdout is never parsed or merged into a later result. Tests supply
empty bytes, a truncated body, and even valid but untrusted JSON; only the later
complete HTTP-framed successful response can be returned. A successful framed
response containing malformed JSON remains immediately fatal. A framed denial,
another exit code, or a mixed/different diagnostic cannot use the new EOF
classification. Existing, separately qualified HTTP and transport policies
remain unchanged.

Each ordinary GET keeps its exact endpoint and argv, at most four subprocess
requests, the existing 1/2/4-second backoffs, and the 247-second transport
envelope. Existing authenticated quota handling retains its separate
61-minute cumulative wait ceiling. With `retry_transient=False`, the reader
raises the existing transient exception immediately, without sleeping or
retrying that 100-row request. `github_jobs` then uses its existing bounded
50-row complete-inventory fallback. Its page counts, inventory consistency
restarts and per-page read budgets do not change.

`github_jobs.py`, `producer_context.py`, `artifact_receipt.py`,
`component_producer.py`, source identity, artifact selection, archive verification,
all workflows and the OFD source policy are unchanged. Exact reversal of the
small reader addition reconstructs the complete baseline file. Every prior
component-test byte also remains after removing only the new test block and
import. The two existing TCP helper maps pin the reviewed new reader blob and
explicitly reject its predecessor; no caller can choose a different helper or
refresh authority at runtime.

This is within the current repository scope: the same real CI operation and
security predicates execute, and no app behavior, capability, desktop default,
worker or native implementation changes.

## Qualification and its execution boundary

The actual Python reader, complete-inventory code, artifact selector, ZIP
SHA-256 verification/extraction, original/current producer authentication and
final component publication execute in the maintained fixtures. The boundary
supplies subprocess HTTP results, storage responses, checkout identity and
clocks. The payload is explicitly synthetic text; no native binary is compiled
or run by these cases.

| Maintained entry | Passing tests |
| --- | ---: |
| Existing GitHub reader/inventory/receipt contracts | 51 |
| New exact EOF contracts | 12 |
| Existing quota contracts | 18 |
| Artifact batch / supplier bundle | 34 / 16 |
| Producer context / Binder modes / native2 context | 26 / 15 / 6 |
| Component publication, including new post-download cases | 19 |
| Source identity | 14 |
| **Total unittest methods** | **211** |

The real component cases finish both verified ZIP downloads before injecting
the EOF into the later original- or current-attempt snapshot. Recovery preserves
the original manifest bytes and caller attempt. Later mutations of source,
attempt, successful execution, run identity, digest, size, expiry and archived
role bytes still fail before final publication. Missing IDs/fields, duplicate
jobs, malformed framed JSON and explicit denial remain failures. Exhaustion
uses one failed 100-row request followed by the existing four attempts for the
50-row page, without another fallback loop.

The unchanged reader from public main fails the new recovery cases. Seven
isolated, syntactically valid source mutations also fail their designated real
assertions: broad exit-code matching, ignored HTTP framing, substring diagnostic
matching, an enlarged transport deadline, disabled caller no-retry behavior,
acceptance of malformed framed JSON, and removal of the post-download snapshot.
These mutations exist only in temporary test copies.

Actual selected-source, selected-supplier and vector preflights pass. The
existing PowerShell helper gate verifies all nine real source pins, 25
changed/missing/stale/additional/prequeue negatives, and four unchanged caller
budget negatives. The original 71-file OFD source protection and 23 shared-stdio
controls remain intact. No source or acceptance predicate was skipped.

Initial test-development failures are retained separately: an omitted field
raises the existing `KeyError`, one inventory error message was initially
misnamed, and a failed subtest initially left its own temporary destination for
the next case. The final fixture recognizes those actual failure boundaries
and clears only its owned temporary setup between cases. These earlier logs
are not counted as passing qualification, and no production behavior was changed
to satisfy them.

## Reproduction and limits

The existing CI already discovers the new `test_github_eof.py` through
`test_github*.py` and runs the component entry. From the repair checkout:

```sh
python3 -m unittest discover -s tools/ci -p 'test_github*.py'
python3 -m unittest discover -s tools/ci -p test_component_producer.py
pwsh -NoProfile -File tools/net-tcp-priority/test-helper-metadata.ps1
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-selected-supplier.py
python3 tools/net-tcp-priority/test-vector-policy.py
```

The JSON records all 14 full qualification commands and log digests. The
104-member archive contains all three original failure logs, failed and current
sources, source-reversal proof, final and initial controls, independent replay
receipts, and a portable `native_github_eof_controls.py` reproducer. Its README
explains how to run the old-reader negative and seven source mutations against
the actual checkout. The original failing subprocess stdout and remote cause
remain unavailable. Actual authenticated GitHub recovery and the full submitted
workflow must be reported separately from these host results.
