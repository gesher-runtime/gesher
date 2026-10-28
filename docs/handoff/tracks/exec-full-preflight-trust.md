# Exec full preflight trust boundary

The original Exec source is a1e06af6 (two test files only), based on qualified
integration f1550540186b4ae3f17a5e501a886ec1c4d5d613. Full40784262363
failed before runtime because historical current_payload.py correctly rejected
the two new paths. Its historical bytes and all producer/consumer role,
source/run/attempt/compiler, archive membership/digest, package and e2e checks
remain unchanged. No production change or caller policy qualification is claimed.

The new verifier is the reusable workflow exec-full-preflight.yml at immutable
ca955debcca84d466a621a5dbcc0f66f9ad1fdb3. ci.yml calls this SHA, with no inputs.
It executes only its own embedded Python, before every producer, and compares the
entire non-doc source tree to exact f155 plus the two individually pinned test
blobs and the externally reviewed caller/verifier workflow bytes. It also checks
the tracked checkout bytes and rejects dirty source. It does not execute or
expand a candidate-supplied pin list. The uncommitted preliminary exec_payload.py
clone has been removed; it would have allowed self replacement and arbitrary CI.

The trusted job resolves repository variable EXEC_FULL_CALLER_COMMIT directly
from vars context. No workflow_call input, candidate environment, event payload
or source file supplies this authority. A maintainer must set the variable to
the exact externally reviewed caller commit after source peer review, before
dispatch. The verifier loads ci.yml and the reusable file from that immutable
commit and requires their exact blobs. An unset/malformed identity or differing
caller fails closed. Updating the variable is a separate GitHub write requiring
the immediate palmoni5 account check and reviewed source. Do not set it to a
moving branch or automatically bless a newly pushed candidate.

The trusted job independently fetches its actual Actions run metadata and checks
repository, source, run ID, attempt, workflow path and dispatch event against its
GitHub runtime context. This initial verifier qualifies exact workflow_dispatch
fulls only. It intentionally fails closed for push/PR events; general main/PR
policy requires separate review before importing this automation into main.

Trust still requires external review that the exact ci.yml calls this pinned
verifier and all producers depend on it. A workflow that removes the gate cannot
be forced to execute that gate by code inside it. This limitation is explicit;
the repository variable does not turn arbitrary candidate workflows into trusted
ones. Root/integration alone owns integration, main, PR, tag and release.

Same-run controls reject missing/malformed/wrong external authority, actual
no-op guard and caller edits with exact restoration, combined guard/caller
replacement, a changed test blob, and a candidate-supplied guard clone. Read raw
EXEC_TRUST_NEGATIVE and EXEC_TRUSTED_SOURCE logs before accepting the gate.
The original e2e condition retains package failure diagnostics but now explicitly
requires trusted verifier success, so custom condition evaluation cannot bypass
that prerequisite. All original package/e2e bodies are unchanged.

Local checks are limited to Python syntax, exact Git source inventory, policy
negative controls and workflow dependency inspection. No Java/native fixture or
build ran locally. A fresh independent automation source peer is required before
any GitHub write, variable update or CI dispatch. Full Windows/runtime/package/
e2e evidence remains pending; no acceptance or completion is claimed.
