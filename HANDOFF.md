# Gesher 0.1.0 — portable stop handoff (2026-10-31)

The user requested a near-term stop and transfer to another computer. All listed source work is checkpointed; **do not treat checkpoint refs as CI authority, a frozen review, or release approval**. No main merge, release tag, release, CI cancellation or new CI was performed for this handoff. Resume only after the user asks to continue. The latest stop request supersedes older instructions to keep working indefinitely.

This document and `evidence.zip` are backed up on the repository branch `wip/portable-handoff-2026-10-31`. The ZIP contains plain reports, scripts, metadata, and logs, not CI artifacts or native binaries. ZIP SHA256: `1fddb655d879033add23cce7a2c1225549ce653699652afa11560bcad98bc2af`; `EVIDENCE-MANIFEST.json` SHA256: `b39e9886f2828a0489cb44047209e9391552b7bdd38e2bafbbe1a27d36fc2480`. Each member's length/hash is in that manifest. Some historical reports mention old Windows absolute paths; resolve their basenames inside the ZIP's `evidence/` directory instead. The compact source-of-truth log is `evidence/recovery-2026-10-31-current.md`; later entries explicitly supersede stale earlier status. Read also `evidence/portable-fonts-2026-10-31.md`, `evidence/portable-pthread-2026-10-31.md`, and `evidence/track-rules.md`.

## Working model and scope

The primary agent is manager only. Delegate execution, Git, tests, and log work to at most three workers, with one sole owner for integration/main/PR/tag/release. Use independent worktrees and make a fresh assignment when a worker finishes. Reply to the owner in Hebrew; write code and project documentation in English. The later user preference is Terra with medium reasoning for routine work and high reasoning for complex work; it supersedes the historical Opus-only note.

All known roadmap, handoff, and ADR gaps remain in scope. Do not weaken or delete requirements. Main and release are preauthorized only after every applicable qualification is met; do not ask again for approval already given. The current stop request supersedes the older instruction to continue until completion. On resumption, use the latest handoff and actual evidence over older snapshots. The historical 1,157-entry inventory is not a count of missing defects.
## Restore source and evidence

```sh
git clone https://github.com/gesher-runtime/gesher.git gesher
cd gesher
git fetch origin 'refs/heads/checkpoint/*:refs/remotes/origin/checkpoint/*' 'refs/heads/wip/portable-handoff-2026-10-31:refs/remotes/origin/wip/portable-handoff-2026-10-31'
git worktree add ../gesher-portable origin/wip/portable-handoff-2026-10-31
git worktree add -b resume/coherent ../gesher-coherent e5b9baadd7ca4fe2b64c81f6d4a47ff6a6e5e8a9
git worktree add -b resume/fonts ../gesher-fonts a61d01adbc2de537c1b90b17944af3783cfbb3ae
git worktree add -b resume/helper ../gesher-helper b922ec7d9f36eb24cae7992a7a26e665161febcf
git worktree add -b resume/graphics ../gesher-graphics 49504aa2a4f7a0bdcae474321049bea4af17e09a
git worktree add -b resume/join-witness ../gesher-join-witness 5e35b0dd80f627bd446c4371b5d8d691645e97f7
git worktree add -b resume/join-object ../gesher-join-object 9e9fc4915dab742b3dbf7649432505d6f9c473bf
```

On Windows use paths appropriate to the new computer. The portable branch is an orphan evidence branch, so its worktree contains the handoff rather than source. Open its `HANDOFF.md`, verify ZIP SHA256, and extract `evidence.zip`. Explicitly verify the fetched refs below before work; don't assume an old local process, watcher, credentials, external authority, or branch state was restored.

| Preservation ref under `refs/heads/` | Exact commit | Meaning |
| --- | --- | --- |
| `checkpoint/portable-coherent-20261031` | `e5b9baadd7ca4fe2b64c81f6d4a47ff6a6e5e8a9` | Committed incomplete coherent full/TCP wiring; **not runnable or peer-approved**. |
| `checkpoint/portable-2026-10-31-fonts-resource` | `a61d01adbc2de537c1b90b17944af3783cfbb3ae` | Exact Fonts parser source; guard `a4a5b3d073d0dfe421b0242f2e42d3abf1f18c0b` is an ancestor. |
| `checkpoint/portable-2026-10-31-helper` | `b922ec7d9f36eb24cae7992a7a26e665161febcf` | Exact helper CRT collector; guard `b36152963430b517b345ca715e01fe1780105c55` is an ancestor. |
| `checkpoint/portable-2026-10-31-graphics` | `49504aa2a4f7a0bdcae474321049bea4af17e09a` | Incomplete Graphics bridge; caller has **zero guard pin**. Do not run. |
| `checkpoint/pthread-witness-runtime-20261031` | `5e35b0dd80f627bd446c4371b5d8d691645e97f7` | Unreviewed WIP, stale/zero guard, controller returns UNKNOWN. Do not run. |
| `checkpoint/pthread-nontail-object-20261031` | `9e9fc4915dab742b3dbf7649432505d6f9c473bf` | Non-tail collector source; guard `5a5c226e4c95c076c01028881fad01902f395655` is an ancestor. Independent peer and CI still pending. |

The coherent checkpoint contains the preserved integration `2e41f6d21b21639d586986b83e2a05df9bc7b688`, qualified docs union `ee52e7a120bc10ba1b69b5067f138f4ffaac66d3`, full source `1bf8fad9ec3b423ddef8c97c1d72b25790974364`, TCP `695229f0c31939ef4a95dc0cb4eef7ba5172786a`, and Native OFF `fcb41c8c3fb0796260fd672d65554d09282ba6d9` histories. The coherent prior reviewed-WIP source `0a0f63cf39f17315a63913395c72de13d0a688ae` and guard `90f01cad1f3e189d30beaf4ad96fa15ae75048eb` are ancestors. Its final checkpoint `e5b9...` adds incomplete selected TCP/full same-run work after that guard: caller bytes/pins are stale and the supplier correlation job is absent. Never dispatch it. Complete role wiring, validate exact same-attempt full-build vs TCP supplier receipts and binary hashes, rebuild/pin a fresh guard, run the original pure and negative controls, and obtain a different complete source/DAG/role peer before any CI. Preserve original full gates, historical publisher canonical detached +1 red, and selected/canonical role separation. Do not rerun an unchanged red simply for a new SHA. Review `evidence/coherent-selected-0a0f-independent-corrections-peer.md` and `evidence/coherent-selected-tcp-same-attempt-supplier-correlation-design.md`.

## Last observed runs and first resume actions

Last compact repository run-list snapshot: **2026-10-31 18:42:56 UTC**. All four named runs were terminal; all local observers were closed. Do not resume a watcher for a terminal run. No remote runs were cancelled.

- Native161 `41221763220` attempt 1, source `fcb41c8...`: five jobs succeeded and an independent saved-log audit verified both hosts' 161 rows/zero mismatches, 13 modes/zero failures, four stdio cycles, and same-attempt selected production-OFF provenance. See `evidence/runtime161-412217-independent-actual-audit.md`. Build raw API GET returned HTTP418/NetFree HTML; it is preserved as failed transport. One separate gh-rendered log fallback succeeded. Component scope only; no default/canonical/full/hardware/release acceptance.
- Join diagnostic `41224370978` attempt 1, source `1fe0068...`: both jobs succeeded. Actual object logs show diagnostic wait wrapper tail-jumps to `WaitForSingleObject`; no linked PE, live wait, cancellation or product claim. See `evidence/join-collector-412243-independent-actual-audit.md`.
- Fonts `41229488015` attempt 1, source `a61d...`: guard and producer succeeded, Windows 2025 succeeded and Windows 2022 failed **by terminal job metadata only**. No raw a61d job logs were fetched. On explicit resume, first inspect the saved handoff and assign one owner; verify exact run/attempt/source/job metadata and existing leases, then acquire each needed completed plain job log **once**, record bytes/hash and close receipts. Do not assume the failing cause or infer acceptance from the green job. Prior 2a3a root gate actually rejected named=1, ids=2, count=3; its failure is preserved.
- Helper `41230459346` attempt 1, source `b922...`: guard succeeded, producer failed and Windows was skipped **by terminal job metadata only**. Guard log was fetched once; producer raw was reserved but **not fetched**. On explicit resume, inspect `evidence/portable-fonts-2026-10-31.md` and existing lease records, assign one owner, and acquire the exact completed producer plain log once only if not already fetched; record byte/hash/closure. Do not infer collector output, CRT identity or Windows qualification. Prior eda8 actual trace had 11 archive readings/61 loaded; `ucrt` basename predicate alone failed while `libmsvcrt.a` appeared; names alone do not prove runtime CRT.

For any genuinely active run discovered on the new machine, first determine whether another owner/observer exists and record exact source/run/attempt. Only one observer per run: `gh run watch ID -R gesher-runtime/gesher --exit-status --interval 120`. The old machine's process IDs cannot be inherited. On HTTP403 back off about 10 minutes. Save plain logs with explicit once-only leases and hashes; do not download CI artifact archives locally. A failed transport is preserved, not relabeled as a job log; do not retry unchanged red without a concrete changed cause.

## Required boundaries

- GitHub writes, including pushes, variables and workflow dispatches, must use `gh`-authenticated transport and immediately beforehand `gh api user --jq .login` must equal `palmoni5`; otherwise do not write. Do not use the application GitHub connector to write for this user. Commits require `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; PRs require footer `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- No local C/C++/Java/C# compilation or native/JVM/C# fixture execution; only source/Git/lightweight scripts locally. No WSL, local CI artifact downloads, or access to `C:/work/gesher-backup-2026-10-23`. Native/managed fixtures compile and execute in CI only. Preserve both sides of integrations and use `--no-ff` merges.
- Historical automatic cyber-safety stop: the specific Web4 WIP `5facaf7025976b66be2874e68e3e8073a5ee20b7` continuation is preservation-only. Do not resume, rephrase, implement, review, run, or bypass it. Its recorded delta paths are absent from accepted `1bf8` and candidate ancestry by path-only check; this is not a blanket ban on unrelated canonical CI. See `evidence/new-manager-union-coordination.md` lines 281–297 and the retraction in the current recovery checkpoint. A separate automatic review rejection of an oversized Node save is scoped to that action; do not repeat or bypass it.
- Real hardware/interactive evidence, product gaps, full current-source proof, package/install/uninstall, both e2e, exact installer and released-byte provenance remain open. Full 410789 at `1bf8` is bounded historical coherent Fonts/Media/UDP evidence, not complete 0.1.0 release qualification. No main/PR/tag/release is ready.
