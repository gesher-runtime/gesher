# Merge-queue source identity qualification

CI run 44163397530, Windows jobs 134404171819 and 134404171886, rejected
`tools/ci/source_identity.py` because the selected TCP receiver still pinned the
pre-queue blob `e03b6d4d926f954eecd524ee63ca182edfacdd17`.

Reviewed replacement: `8bbf1174020adab86b02403070f88c5aa42d9755`, introduced by
the native-queue migration in e80cd1854163bd18e4ea5392c25f423184048804. Its only
behavioral addition is a separate merge_group source mode. It binds the event
action, exact group head and ref, main target and valid base SHA. Existing PR
head/source-repository identity and raw two-parent/target-ancestry checks remain
unchanged. Push and workflow_dispatch behavior also remains unchanged.

Update both the receiver's trusted map and its independent metadata expectation
to this reviewed blob. The metadata test additionally rejects the pre-queue blob;
changed, missing, historical and additional helpers continue to fail. The other
six helper pins, controller budgets, artifact acceptance and retirement predicates
are unchanged.

The coherent TCP role checker also needs an explicit merge_group ref branch,
after source_identity validates that event. All four roles retain their exact
repository, workflow path/ref, source SHA, policy SHA, run/attempt, nonempty actor
identity and selected supplier-role checks. A queue event is not a general
exception allowing arbitrary branch refs.

Validation against production functions:

- 14 source identity tests pass, including real Git commit/ancestry fixtures.
- All four coherent TCP roles pass queue-source controls; 249 negative mutations
  across PR, push/dispatch and queue identity are rejected.
- Windows PowerShell metadata tests pass: exact helper pins, stale/changed/missing
  and extra helper rejection, four controller call budgets, 20 retirement
  mutations, 72 source/artifact identity negatives and unchanged original receipt
  acceptance predicates.

These are source/provenance and isolated metadata tests. They do not execute the
native TCP controller or establish a fresh Windows runtime/ART test result.
Full PR CI and native merge-group execution remain required. No Android-visible
runtime or application contract changes.
