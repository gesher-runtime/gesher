# Contributing to Gesher

Gesher runs Android applications on Windows by providing an Android
compatibility layer. Contributions must respect the project scope below.
Contributors and reviewers, including AI assistants, must read this document
before implementing or reviewing a change.

## Project scope: the same experience, implemented for Windows

Gesher runs Android applications on Windows so that the user gets the
experience each application was built to give. How Gesher gets there may differ
from Android as much as Windows requires: Windows APIs, the PC's real hardware,
desktop windows and presentation, and defaults suited to a desktop are the
expected way to implement Android here. Similarity to AOSP's source or internal
mechanisms is not a goal, and a different implementation is never a defect by
itself.

What must stay the same is what the user and the application actually do:

- **The operation itself.** When an application asks for an operation (take a
  photo, record audio, read the location, pick a file, sign in), the user
  performs that operation, through the PC's real equivalent. Do not replace it
  with a different operation because the real one is hard to provide. For
  example, when an application asks for a live camera capture, do not let the
  user choose an existing image from the PC instead, including for cheque
  deposits, unless the capability is missing on the PC and the alternative
  operation is enabled through an explicit, documented setting approved by the
  maintainer and listed in the examples below.
- **Honest results.** Do not report that something happened when it did not:
  a capture, a payment, an autofocus, a permission the user did not grant. When
  something is unavailable, use Android's own unavailable or unsupported
  behavior.
- **Honest capabilities.** Features, capabilities and hardware characteristics
  describe what the PC and Gesher really provide, apart from the decided
  adaptations listed below. Selecting a real camera or describing its mounting does not raise its
  hardware level or add capabilities.
- **The user's choices and protections.** Permissions, consent, security and the
  application lifecycle keep working as on Android.

Adapting Android to the desktop is in scope: presentation, input, orientation
and defaults may follow what a PC is. Such adaptations are documented, named as
Gesher's where an application can see them, and give way to real hardware when
it exists. A default may describe the PC's real state (it stands still, its
camera faces the user); it never substitutes content (an image, a location, a
reading that does not match the PC's real state). For capabilities that may be
missing on a PC, a maintainer-approved adaptation may substitute a similar
available capability (for example, using a regular PC camera in place of a
missing rear camera) even without a user setting. Providing an alternative
operation for a missing capability (such as letting the user choose an existing
image from the PC when an application asks for a live camera capture) is in
scope only with both maintainer approval and an explicit, documented setting.
Any such adaptation, default, or substitution setting that applications can see
is a maintainer decision and is listed in the examples below before it is merged.

Examples:

| Change | Policy |
| --- | --- |
| Connect Android's camera APIs to a Windows camera. | Within scope. |
| Let the user select which real Windows camera is used, and describe its physical mounting. | Within scope. |
| Report a PC camera not configured as rear as Android's front camera. | Within scope: a desktop default. A PC's camera faces the user; `GESHER_CAMERA_REAR_ID` marks a rear one. |
| Report a camera's orientation relative to the display rotation of the Activity using it, since Windows delivers frames upright for the monitor. | Within scope: a desktop adaptation. The frames and the capture are real. |
| Expose Gesher's stationary accelerometer (gravity only) on a PC without motion sensors. | Within scope: a desktop default. Real motion sensors replace it, it is documented and `GESHER_SENSOR_FALLBACK=0` turns it off. |
| Substitute a similar available PC capability for a missing one (such as exposing a regular PC camera as a rear camera) with maintainer approval. | Within scope once approved by the maintainer and listed here as a decided adaptation (no setting required). |
| Provide an alternative operation for a missing PC capability (such as choosing an existing image from the PC for a camera capture) via an explicit, documented setting with maintainer approval. | Within scope once approved by the maintainer, gated behind an explicit setting, and listed here as a decided adaptation. |
| Report the device's identity: the PC's real manufacturer and model (SMBIOS) as `Build.MANUFACTURER`/`Build.MODEL`, Gesher's own brand, product, device and fingerprint, and the Windows computer name as the device name. | Within scope: maintainer decision (2026-11-25). Gesher does not impersonate another device (it no longer reports a Pixel 8); firmware placeholders fall back to the baseboard's names, then to `Gesher`/`PC`, never another vendor's; a virtual machine reports its hypervisor's values; a device name the user set stays. See [docs/device-identity.md](docs/device-identity.md). |
| Implement an Android file-selection request with a Windows file picker. | Within scope: the user picks a file, as asked. |
| Answer a camera capture request with a file picker or a saved image presented as a fresh capture. | Outside scope: a different operation, unless enabled by a maintainer-approved, documented setting for a missing PC capability. |
| Report success for an operation that did not run (a capture, a payment, an autofocus). | Outside scope. |
| Add package-name exceptions so tests pass while ordinary applications behave differently. | Outside scope. Tests must exercise the production behavior. |

Mock devices and synthetic inputs belong in explicit, isolated tests. Ordinary
applications see synthetic values only through a decided desktop default listed
above, never through a test fixture.

When it is unclear whether a change keeps the requested operation or replaces
it, discuss it in an issue or PR and link the discussion. The maintainer decides
such questions; decisions that set a general rule are recorded in this document.
Routine compatibility work needs no separate scope approval. A change that
replaces the requested operation or reports a false result blocks merging until
it is removed or redesigned.

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
2. Decide whether the user and the application get the operation they asked
   for, with honest results. Look for a replaced operation, a false success,
   package-specific workarounds and test/production differences. Evaluate what
   the user and the application observe, not whether the internals duplicate
   AOSP or use a Windows-specific mechanism; desktop adaptations listed in the
   examples above are not violations.
3. Report a policy violation as a **merge blocker**, with the relevant code
   location, requested operation, actual behavior and observable consequence.
   A request to remove or redesign useful work must be grounded in such a
   mismatch or a concrete correctness, security, performance or resource
   problem. Preserve useful changes that have no substantiated defect; do not
   reject them merely for adapting Android to Windows. Do not recommend
   approval merely because the implementation works or CI is green.
4. If a scope decision depends on missing evidence, state what is missing and
   withhold an approval recommendation until it is resolved. Do not infer
   compatibility from silence, a checked box or the contributor's own assertion.
5. After scope review, check correctness, regressions, resource use and relevant
   tests. Scope compliance does not replace these checks.

The review must state the reviewed commit, its scope conclusion and supporting
evidence, any blockers or unresolved questions, and the validation performed
with its limits. Reassess affected findings after updates to the PR. Review
advice does not itself authorize posting to GitHub, applying labels or merging.
