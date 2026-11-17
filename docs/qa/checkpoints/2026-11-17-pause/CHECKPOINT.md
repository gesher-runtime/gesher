# Paused Gesher QA checkpoint — 2026-11-17

The user requested a near-term stopping point and preservation of existing work in GitHub. This is an evidence-only checkpoint, NOT a completed QA round, a code approval, or authorization to merge. No new broad review or implementation work is started by this checkpoint.

## Scope of this stopping pass

The GitHub connection was verified as **palmoni5 (216508796)**. The hourly ChatGPT task **מעקב מיזוגי Gesher** was disabled successfully at 2026-11-17T17:38:34Z. The repository's existing CI runs and automerge configuration/labels were not canceled or changed. Pausing the ChatGPT task does not pause repository automation.

This branch preserves the seven original files from the local QA handoff archive, byte-for-byte, as well as this newer checkpoint. The historical `archive/README.md` and `archive/qa-manifest.json` describe the previous QA pass; their then-current state, tool availability, and absence of GitHub writes must NOT be interpreted as describing this later archival pass. Test logs are historical evidence, not newly rerun tests. No subagents were launched in this stopping pass.

Original archive: `gesher-yom-chadash-qa-20261117.zip`, 14,481 bytes, SHA-256 `d32440b7f9ca2d2311dbab9ff8700fb9064e49debaf7a66a694e5aa085a88978`. The expanded files are stored under `archive/`; the ZIP container itself is not part of this commit. File digests are in `ARCHIVE_SHA256SUMS`.

The checkpoint is based on main `0af3fd8333e43cdeb7a3b4bcd0403758e6d556c2` (tree `5c3b1b372ae1858d9c81f7a934e9d590fd1f5711`). No product source, workflow, existing PR branch, review, label, or main ref is modified by this archival commit. No existing branch is deleted.

## Verified outcome of the interrupted user script

All five PRs below were re-read as OPEN, non-draft and unmerged during this stopping pass.

| PR | Live head read | Application state |
| --- | --- | --- |
| [87](https://github.com/gesher-runtime/gesher/pull/87) | `503eaf20885481c4f860d9bde611c75a82823448` | Repair applied; its upstream temporary branch is absent, consistent with the user's successful cleanup. |
| [92](https://github.com/gesher-runtime/gesher/pull/92) | `09627a9a1964ceb863a8eb46d33b458b11db6a00` | Repair applied. The user's push succeeded; the script stopped at immediate PR-head confirmation. The temporary branch still preserves this commit. The cause of the transient confirmation mismatch was not established. |
| [93](https://github.com/gesher-runtime/gesher/pull/93) | `1df606ebdc780bc376156e63c2e3a96fef350138` | Fixture repair `4e53dd7a46191d75df1bd1914389f599fe4b1828` remains on the upstream temporary branch and is not applied. The previously documented major lock-accounting/lifecycle request is NOT resolved by this fixture repair. |
| [94](https://github.com/gesher-runtime/gesher/pull/94) | `cc7656eaeb0b32b1b81d6bd264b524de1ebec00f` | Explicit-timestamp repair `78730c164966b0a288e9b65ae0b7e9ca5612a335` remains on the upstream temporary branch and is not applied. |
| [97](https://github.com/gesher-runtime/gesher/pull/97) | `6d43bf853b3e5467947dcee31d6f8ee947ff4f6b` | O_PATH repair is applied. A later CI-policy repair is staged separately, not applied. |

No user command is necessary to preserve this stopping point. Do not force-push or delete the pending repair branches. Before resuming, re-read state, draft flag, current head, contributor history and repair ancestry. The archived script has an immediate-confirmation limitation and does not include all newer staged repairs; it is preserved as historical material, not blindly recommended for a later run.

## Current-head CI snapshot, not old-head success

| PR | Live head | ARM64 result | Main CI result |
| --- | --- | --- | --- |
| [70](https://github.com/gesher-runtime/gesher/pull/70) | `7976f868aa846bf69638336da72f3000584d16c9` | [44128137249](https://github.com/gesher-runtime/gesher/actions/runs/44128137249) success | [44128138038](https://github.com/gesher-runtime/gesher/actions/runs/44128138038) queued |
| [76](https://github.com/gesher-runtime/gesher/pull/76) | `39be47da012f8fc7433fd29be2bfb90efdc15a7e` | [44128157096](https://github.com/gesher-runtime/gesher/actions/runs/44128157096) success | [44128157082](https://github.com/gesher-runtime/gesher/actions/runs/44128157082) queued |
| [97](https://github.com/gesher-runtime/gesher/pull/97) | `6d43bf853b3e5467947dcee31d6f8ee947ff4f6b` | [44115457610](https://github.com/gesher-runtime/gesher/actions/runs/44115457610) success | [44115457934](https://github.com/gesher-runtime/gesher/actions/runs/44115457934) failure |

The #70 and #76 bodies document newly published bounded CI repairs. This stopping pass verified their new heads and associated run states; it did not rerun their tests or independently renew their QA approvals. The #97 staged commit `abccd4ba7f693c646364bc4d97ae426b4ceef08f` changes `tools/net-tcp-priority/test-selected-policy.py` to pin the reviewed fadvise changes. Its diff was read, but it was not applied or retested in this stopping pass. Earlier red results must not be confused with later uncompleted runs, and ARM64 success alone is not overall CI success.

## Preserved upstream QA branches

The following refs and exact commits were read from the repository's `git/matching-refs/heads/qa/` endpoint during this stopping pass. Presence is proof of preservation, not a fresh QA approval or proof that a repair has been applied.

| Branch in gesher-runtime/gesher | SHA |
| --- | --- |
| `qa/pr-85-scanf-abi-20261117` | `4539a6f84acc3e8fd4c2bcf831b97888bc5163fe` |
| `qa/pr-92-external-url-results-20261117` | `09627a9a1964ceb863a8eb46d33b458b11db6a00` |
| `qa/pr-93-working-set-fixture-20261117` | `4e53dd7a46191d75df1bd1914389f599fe4b1828` |
| `qa/pr-94-explicit-timestamps-20261117` | `78730c164966b0a288e9b65ae0b7e9ca5612a335` |
| `qa/pr-95-metadata-contract-20261117` | `277071c8237a24273095ae846b11e3aec8d8bb88` |
| `qa/pr-96-searchable-visibility-20261117` | `f3cd35b538f87ef73246c9a9d618ce0ff774ce2c` |
| `qa/pr-97-fadvise-ci-20261117` | `abccd4ba7f693c646364bc4d97ae426b4ceef08f` |
| `qa/pr-97-fadvise-opath-20261117` | `6d43bf853b3e5467947dcee31d6f8ee947ff4f6b` |
| `qa/tcp-native-thread-observation-20261117` | `a8f3521bc9b7c1871d0767e37cacdbf8e8cbd985` |

## Resume gates

Revalidate each live head and skip drafts. Inspect the full current diff and any newer contributor changes before applying prepared repairs. Obtain QA for the resulting exact head and diagnose its failing CI rather than bypassing assertions or treating a previous SHA's success as current. Preserve Android/AOSP semantics, lifecycle, permissions, real errors and honest capabilities. The earlier reviews of #60, #93 and #96, and the unfinished reviews of the other PRs, remain follow-up items; this checkpoint does not declare those resolved. Existing repository automation may continue independently during the user's pause.
