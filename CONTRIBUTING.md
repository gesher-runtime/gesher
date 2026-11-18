# Contributing to Gesher

Gesher runs Android applications on Windows by providing an Android
compatibility layer. Contributions must respect the project scope below.
Contributors and reviewers, including AI assistants, must read this document
before implementing or reviewing a change.

## Project scope: preserve application behavior

Gesher provides the runtime, Android APIs and surrounding system services that
an application expects, implemented on Windows. Windows adaptation is a core
purpose of the project: use Windows APIs, real hardware, desktop presentation
and system mechanisms where appropriate. Internal architecture and implementation
may differ substantially from AOSP; source-level similarity is not a goal.

Judge compatibility at the application-visible boundary. Preserve the operation
the application requested, the meaning and origin of its inputs and results,
and the relevant Android contracts. A Windows implementation of the requested
operation is welcome. Replacing that operation with a different one is not.
Lifecycle, permission checks, errors and capability reporting are part of this
contract. If a capability is unavailable, use the Android-defined unsupported or
unavailable behavior rather than inventing a successful result.

Do not add behavior that replaces, bypasses or changes an application's workflow
or requirements to make a particular operation succeed. This applies even when
the APK is unchanged and the replacement is implemented entirely inside Gesher.
A user opt-in or a successful demonstration in one application does not by itself
make such a change compatible with this policy.

Examples:

| Change | Policy |
| --- | --- |
| Connect Android's camera APIs to a Windows camera, preserving capture semantics and reporting actual capabilities. | Within scope. |
| Let the user select a real Windows camera and describe its physical mounting. | Within scope when the configuration truthfully describes the device; selection alone does not establish additional capabilities or change sensor geometry. |
| Implement an Android file-selection intent with a Windows file picker when the application requests file selection. | Within scope, preserving the intent's selection, permission and result contracts. |
| Replace a camera capture with a file picker or feed a saved image as live camera input so an application accepts it as a fresh capture, including for cheque deposits. | Outside scope. Preserve the requested capture workflow and use an actual camera backend. |
| Report successful autofocus, sensor readings or other capabilities that the backend does not implement, to satisfy an application's checks. | Outside scope. Implement the capability faithfully or use the Android-defined unavailable/unsupported behavior. |
| Add package-name exceptions that change application semantics so tests pass while ordinary applications receive different behavior. | Outside scope. Test fixtures must exercise the relevant production behavior. |

Desktop presentation and input may differ where Windows requires it; the
Android-visible contract and the application's choices must still be preserved.
An implementation difference, desktop-specific setting or use of a Windows API
is not by itself a scope violation. For example, selecting which real camera
performs a capture preserves the requested operation; selecting an existing
image and returning it as a fresh capture replaces that operation.
Mock devices and synthetic inputs may be used in explicit, isolated tests. They
must not silently become production defaults or be presented as real hardware
behavior. Synthetic inputs must not replace real inputs in ordinary application
use.

Discuss materially unclear application-visible contracts or non-runtime
project-scope questions in an issue or PR, and link the relevant discussion.
Ordinary Windows implementation choices do not require a separate scope
approval. Maintainer approval, user opt-in
and successful demonstrations do not waive the Android-contract requirements
above. Routine compatibility work needs no separate scope approval. A violation
of these requirements blocks a recommendation to merge until removed or
redesigned.

## Preparing a contribution

- Describe the Android API or system contract being implemented or repaired.
  Link the relevant Android documentation or AOSP source where applicable.
- Explain the behavior before and after the change, including what applications
  observe and any capability, permission or workflow changes.
- Keep changes focused enough to review. Identify any unresolved scope question.
- Run checks appropriate to the change. For behavior changes, cover the actual
  production path and relevant failure cases; distinguish host fixtures, mocks
  and physical-device or Windows/ART validation. State what was not verified.
- You are responsible for understanding and checking submitted code, including
  code generated with AI. Generated explanations and passing tests are not
  substitutes for reviewing its behavior.

Submit contributions under the applicable project license. Gesher uses
[Apache License 2.0](LICENSE); see also the
[licensing decision](docs/decisions/0001-license.md). Identify third-party code,
its source and license, retain required notices, and ensure you have the right
to contribute it, including any necessary employer permission. Follow the
licensing decision's restrictions on copying code from other projects.

## Required review: scope before correctness

Human reviewers and AI assistants used for review must:

1. Read this policy and inspect the actual diff at the current PR head. Do not
   rely solely on the PR description, checklist, previous review or test results.
2. Decide whether the implementation preserves the Android contract and the
   application's workflow. Look for substituted inputs, fabricated success or
   capabilities, package-specific workarounds and test/production differences.
   Evaluate what the application observes, not whether the internals duplicate
   AOSP or whether the implementation uses a Windows-specific mechanism.
3. Report a policy violation as a **merge blocker**, with the relevant code
   location, requested operation, actual behavior, applicable contract and
   observable consequence. A request to remove or redesign useful work must be
   grounded in such a mismatch or a concrete correctness, security, performance
   or resource problem. Preserve useful changes that have no substantiated
   defect; do not reject them merely for adapting Android to Windows.
   Do not recommend approval merely because the implementation works or CI is
   green. Ask for removal or redesign of the violating behavior; maintainer
   approval does not waive the Android-contract requirements.
4. If a scope decision depends on missing evidence, state what is missing and
   withhold an approval recommendation until it is resolved. Do not infer
   compatibility from silence, a checked box or the contributor's own assertion.
5. After scope review, check correctness, regressions, resource use and relevant
   tests. Scope compliance does not replace these checks.

The review must state the reviewed commit, its scope conclusion and supporting
evidence, any blockers or unresolved questions, and the validation performed
with its limits. Reassess affected findings after updates to the PR. Review
advice does not itself authorize posting to GitHub, applying labels or merging.
