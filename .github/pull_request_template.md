## Change and purpose

Describe the problem and the behavior before and after this change.

## Android contract and project scope

Read [CONTRIBUTING.md](https://github.com/gesher-runtime/gesher/blob/main/CONTRIBUTING.md),
especially "Project scope: the same experience, implemented for Windows".

- Which Android API or system contract does this implement or repair? Link the
  relevant Android documentation or AOSP source. For changes with no runtime
  behavior impact, explain why this is not applicable.
- Does it change application-visible inputs, workflows, permissions, capability
  reporting or success results? Explain how the user still performs the
  requested operation and gets honest results.
- Which Windows mechanisms or real devices implement the requested operation?
  Different internal mechanisms and desktop presentation are expected; explain
  any meaningful application-visible difference, not source-level AOSP parity.
- Does it introduce synthetic inputs, device simulation, a new desktop default,
  a substitution for a missing PC capability, a setting for an alternative
  operation, or package-specific behavior? Explain the production/test boundary
  and link the maintainer decision for a default, substitution, or
  alternative-operation setting and add it to CONTRIBUTING.md's examples, or
  state that it does not.
- If project scope is unclear, describe the question and link any relevant
  discussion; the maintainer decides. Otherwise, state "Within project scope".

## Validation

List the checks performed, their results, and what remains unverified. Identify
which checks exercise production code and which use mocks or host-only fixtures.

## Contributor checklist

- [ ] I have read the contribution policy and checked this change against it.
- [ ] I have explained application-visible behavior and identified any unresolved scope question.
- [ ] I have reviewed the submitted code, including any AI-generated changes.
- [ ] I have identified third-party code and licenses, if any, and have the right to submit this contribution.

## Reviewer instructions

Read CONTRIBUTING.md and inspect the actual current-head diff. Check project
scope before correctness. A checked box or green CI is not proof of compliance.
Report a replaced operation, a false success or capabilities the PC does not
have (outside the decided adaptations listed in CONTRIBUTING.md) as merge
blockers, identifying the requested operation, actual behavior and observable
consequence. Preserve useful Windows adaptations; a
different internal implementation is not itself a defect. Requests for removal
or redesign need a concrete contract, correctness, security, performance or
resource finding. Withhold an approval recommendation when scope evidence is
missing. State the reviewed commit, scope
conclusion, supporting evidence, blockers and validation limits in your review.
