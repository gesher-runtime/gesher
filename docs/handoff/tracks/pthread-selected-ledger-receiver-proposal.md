# Selected first-caller ledger wiring: source-only proposal

Base helper: `4216a2999f0755faa42993fe4ea38fe66efc4957`, independently API-peered.
The separate helper and factory worktrees stay frozen. This branch changes
only five existing tooling/action/first-job paths and adds this handoff and
one source/metadata test. No supplier implementation, public header, runtime,
source-authority guard, default supplier or Web4 implementation changes.
There is no CI/publication/full-adoption approval.

## Actual source wiring

The existing setup action adds the explicit selected CMake project include
only in its selected configure branch. Its publisher command and cache steps
remain exact. The first `ci/build` selector persists the action's selected
receipt, whole hash and existing `ci/build` caller; missing outputs fail. The
default publisher root and other workflow jobs are unchanged.

The standalone teardown-observer compiler command uses an empty launcher and
empty diagnostics array for publisher mode, preserving actual argv. Selected
mode invokes the same frozen original-argv launcher with explicit verbose and
map flags. Observer implementation and flagged Web4 code are untouched.

`host_roles.compiler()` adds only the stable optional `selected_toolchain`
field. The original metadata/driver/launcher/compile-command checks remain.
Publisher return JSON is byte-equal to the original function with the same
inputs. Production OFF/hook ON keep their original compiler equality check.

Production capture seals every actual copied executable before the hook
relink, including the separately built observer. Hook capture seals a separate
record for its renamed executable; production verification excludes that
separate hook filename from the production target set. Ordinary Binder actor
capture seals its moved output before computing the existing whole-file map.

Native capture seals the three existing outputs before computing its inventory:
exact original six EXE/import files plus exactly 13 selected evidence members
(three times record/trace/map/import plus one exact toolchain receipt). Its
three-target set and all source pins/constants remain unchanged. Publisher
keeps exactly six files and gains no selected fields. Selected evidence is
verified on manifest consumption and retained through exact assembly.

`verify_selected()` derives the mandatory five-field expected context from
the independently validated outer manifest, checks compiler identity against
it and passes that context to every portable `verify_sealed()` call. Missing
or extra target records fail. Publisher records reject injected evidence.

Whole sealed target records are emitted under distinct indexed
`SELECTED_LEDGER_*` markers, using the already qualified byte codec with a
distinct record label. Production/hook emit the exact archived role bytes;
Native emits its compiler/output summary. Sender and receiver target records
can be reconstructed and compared without downloading native artifacts.
Every ASCII line is at most 4096 characters, with whole/part hashes and strict
indices. Original supplier receipts retain their original namespace/identity;
this is reuse of a byte transport, not supplier relabeling. Publisher log
bytes and original flush behavior stay exact.

## Validation performed locally (metadata/source only)

`python tools/toolchain/test_selected_link_wiring.py` passes:

- Seven actual workflow root/context/default/other-job negative projections;
  exact publisher configure and observer argv; unchanged Native pins/constants.
- Byte equality of publisher compiler JSON against the original compiler
  function, using textual synthetic CMake metadata and a `.txt` driver marker.
  No executable/PE is generated or run.
- Exact Native six/default and six-plus-13 selected inventory; mandatory outer
  context forwarded to the three receiver calls; five context and two target
  set negatives; publisher evidence injection rejected.
- Exact publisher log output/flush and selected whole-byte transport roundtrip;
  nine missing/order/namespace/label/hash/long-line controls rejected.

The unchanged helper's 40 proof / two mode / six argv / eight response / 23
sealed negatives and full actual historical C++ replay remain passing:
142 definitions/seven members/all 18 old bindings equal/seven actual ablations.
That replay is only old diagnostic archive `5c53...`, not fresh selected
`b415...` or a new execution. Python AST, YAML and Bash `-n` checks pass.

## Deliberately open adoption gates

Actual selected CMake launcher/response/map/sidecar execution is unqualified.
Existing complete-source/trusted-caller guards still reject this new source;
none is relaxed. Global and independent roots, full runtime/tests, later
Binder consumer use of the portable member verifier, packaging consumers and
all stronger clock/provider/lifecycle/bootstrap/cancellation/TSD/async promises
remain separate gates. Existing canonical publisher failures remain hard.

## Cheap first execution: proposal only, no workflow/job implementation

Do not dispatch the original full `ci` workflow for a format test. Its existing
push trigger is main-only and it has independent producer jobs; reusing only
the display name `ci` would not establish the authorized caller or exclude
those producers. The future focused lane must have a separately reviewed,
immutable authority tuple: repository ID `1426153186`, exact dedicated
workflow **path**, ref, workflow name, job, caller key and frozen source/policy
commit. Extending any current single-caller factory guard to that one tuple
requires its own explicit source/policy peer. No wildcard/fallback tuple or
existing full-DAG guard relaxation is proposed.

One candidate focused path is
`.github/workflows/pthread-first-link-component.yml` on a dedicated focused
branch, with one explicitly named build job and no reusable full/independent
producer calls. This is a proposed name, not existing authority or approval.
Before publishing, evaluate every actual existing workflow's branch-and-path
trigger against the exact frozen change set; do not open a PR or dispatch full
CI. Bind source policy and produced artifacts to the exact focused path/SHA/
run/attempt/job. Report only focused-link qualification, never full success.

After the exact same selected two-patch factory/profile/receipt and ordinary
root configure, build only existing small CMake outputs:
`binderd`, `native3_lock_command_worker`, `native3_lock_duplicate_test` and
optionally `platform_probe`. Do not request `all`, dalvikvm or ART objects.
Root CMake globally links `gesher_posix`, so even platform_probe inherits
pthread: source language alone does not justify NOT-LINKED. Bound all actual
origins from actual logs/maps. A separately labeled standalone diagnostic of
the existing raw-Win32 `probes/platform/platform_probe.c`, with explicit
reviewed flags and no global POSIX target dependency, can test the actual
NOT-LINKED branch; its result must be measured, not guessed or called a
production CMake output.

Seal the original CMake outputs first. To exercise real flat GNU response
files cheaply, create a separately labeled diagnostic response relink from a
captured command: retain exact driver, source objects and all non-output
flags, change only explicitly enumerated output/map paths into a NEW owned
diagnostic path, encode the exact argv there and let the same launcher execute
`driver @response`. Check its expanded argv against that explicit diagnostic
command. Never overwrite or relabel the original final output; preserve both
receipts. This does not rely on an undocumented force-response CMake option.

Run the portable metadata verifier and missing/old-context/evidence negatives
in that focused CI lane; typed artifacts and plain bounded logs must retain
all inputs for independent audit. Windows runtime/Native behavior is not
qualified merely by cross-linking or metadata verification. Only after these
actual format results and independent peer may a separate full-adoption
decision update trusted source and route remaining product roots.
