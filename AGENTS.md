# Instructions for AI contributors and reviewers

Read [CONTRIBUTING.md](CONTRIBUTING.md) before implementing or reviewing changes.
Its project-scope policy is a required review criterion, including when the user
asks for a general code review without repeating the policy.

## Implementation

- Gesher runs Android applications on Windows so that the user gets the
  experience each application was built to give. Windows APIs, real hardware,
  desktop presentation and different internal mechanisms are the expected way
  to get there; do not require AOSP's implementation or treat a
  Windows-specific choice as a defect by itself.
- Keep the operation the application asked for: the user performs it through
  the PC's real equivalent. Do not replace it with a different operation because
  the real one is hard (for example, choosing an existing image when the
  application asked for a live camera capture), and do not report success for
  something that did not happen.
- Report features, capabilities and hardware characteristics the PC really
  has. Desktop adaptations (presentation, input, orientation) are in scope when
  they are documented, named as Gesher's where applications can see them, and
  give way to real hardware. A default may describe the PC's real state, never
  substitute content, and a new default applications can see needs a
  maintainer decision recorded in CONTRIBUTING.md, which lists the decided
  ones (display-relative camera orientation, the front-facing PC camera, the
  stationary accelerometer).
- Preserve permissions, consent, security and lifecycle behavior. Discuss
  unclear scope questions in the issue or PR; the maintainer decides them.
  Continue ordinary compatibility work without additional scope approval.
- Test the relevant production behavior and describe validation limits. Keep
  synthetic fixtures isolated from ordinary application behavior.

## Review instructions

1. Read CONTRIBUTING.md and inspect the actual diff at the current head before
   deciding whether to recommend approval. Treat PR descriptions, checkboxes and
   contributor explanations as claims to verify against the implementation.
2. Review project scope before implementation correctness. Check whether the
   requested operation is replaced by a different one, or a result is reported
   that did not happen. Inspect affected call paths and tests
   where needed; green CI does not establish policy compliance.
3. Mark a policy violation as a **merge blocker**. Cite the code location,
   what the application requested, what the implementation does and the
   observable consequence. Base removal or redesign on that
   mismatch or a concrete correctness, security, performance or resource problem.
   Preserve useful Windows adaptations without a substantiated defect; internal
   differences from AOSP are not sufficient grounds for rejection.
   Recommend removal or redesign of the violating behavior,
   and do not recommend approval while the violation remains unresolved.
   Follow the maintainer's scope decisions recorded in CONTRIBUTING.md or in
   the linked discussion.
4. Verify contract evidence and any relevant scope discussion in the supplied or
   accessible sources. If evidence is unavailable, state what is missing and
   withhold an approval recommendation. Do not treat a contributor's assertion
   or a checked box as evidence.
5. Check correctness, resource use and relevant regressions after scope review.
   Distinguish real production execution from mocks and host-only tests. Reassess
   affected findings when the PR changes.

In the review, state the reviewed commit, the scope verdict (compliant,
blocked, or unresolved), the evidence for it, actionable
findings and validation limits. If blocked or unresolved, clearly state that
the PR is not ready for approval. A review request alone does not authorize
posting the review, modifying GitHub state or merging.
