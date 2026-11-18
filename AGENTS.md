# Instructions for AI contributors and reviewers

Read [CONTRIBUTING.md](CONTRIBUTING.md) before implementing or reviewing changes.
Its project-scope policy is a required review criterion, including when the user
asks for a general code review without repeating the policy.

## Implementation

- Gesher provides Android compatibility on Windows. Preserve Android-visible
  contracts and application workflows when implementing desktop integration.
- Windows APIs, real hardware, desktop presentation and different internal
  mechanisms are expected implementation choices. Match the meaning of the
  requested operation and its observable results; do not require AOSP's internal
  implementation or treat a Windows-specific choice as a defect by itself.
- Do not substitute inputs, fabricate capabilities or success, or bypass an
  application's requirements to make a specific application operation succeed.
  Changing only Gesher rather than the APK does not exempt a change.
- A Windows camera backend and an Android-requested file picker are legitimate
  integrations when they preserve their respective contracts. Replacing live
  camera capture with a saved image to satisfy an application's capture flow
  is outside scope.
- Selecting a real camera or explicitly configuring its physical mounting is
  legitimate when it describes reality. It does not by itself establish a
  higher hardware level, additional capabilities or different sensor geometry.
- Preserve Android lifecycle, permission, error and capability contracts. Discuss
  materially unclear application-visible contracts or non-runtime scope questions,
  but do not treat
  maintainer approval as permission to waive those contracts. Continue ordinary
  compatibility work without additional scope approval.
- Test the relevant production behavior and describe validation limits. Keep
  synthetic fixtures isolated from ordinary application behavior.

## Review instructions

1. Read CONTRIBUTING.md and inspect the actual diff at the current head before
   deciding whether to recommend approval. Treat PR descriptions, checkboxes and
   contributor explanations as claims to verify against the implementation.
2. Review project scope before implementation correctness. Check whether inputs,
   workflows, capabilities, permissions or success results seen by an application
   are changed to bypass its expectations. Inspect affected call paths and tests
   where needed; green CI does not establish policy compliance.
3. Mark a policy violation as a **merge blocker**. Cite the code location,
   what the application requested, what the implementation does, the applicable
   contract and the observable consequence. Base removal or redesign on that
   mismatch or a concrete correctness, security, performance or resource problem.
   Preserve useful Windows adaptations without a substantiated defect; internal
   differences from AOSP are not sufficient grounds for rejection.
   Recommend removal or redesign of the violating behavior,
   and do not recommend approval while the violation remains unresolved.
   Maintainer approval does not waive the Android-contract requirements.
4. Verify contract evidence and any relevant scope discussion in the supplied or
   accessible sources. If evidence is unavailable, state what is missing and
   withhold an approval recommendation. Do not treat a contributor's assertion
   or a checked box as evidence of Android compatibility.
5. Check correctness, resource use and relevant regressions after scope review.
   Distinguish real production execution from mocks and host-only tests. Reassess
   affected findings when the PR changes.

In the review, state the reviewed commit, the scope verdict (compliant,
blocked, or unresolved), the evidence for it, actionable
findings and validation limits. If blocked or unresolved, clearly state that
the PR is not ready for approval. A review request alone does not authorize
posting the review, modifying GitHub state or merging.
