# GitHub read transport follow-up — 2026-11-08

## Observed failure before runtime execution

The Binder SG jobs in CI run `42444051868`, testing PR source
`79567dadaacefd15983e9230bfd80290d6cd52f9` through merge checkout
`a8f2940af981046ffd8a76cd0dccab23397584b0`, stopped before invoking the Binder
runtime suite:

| Job | Supplier receipt step | Error |
| --- | --- | --- |
| [Windows 2025, 128621104716](https://github.com/gesher-runtime/gesher/actions/runs/42444051868/job/128621104716) | `boot-image` | GitHub HTTP 502 at 03:57:09 UTC |
| [Windows 2022, 128621104756](https://github.com/gesher-runtime/gesher/actions/runs/42444051868/job/128621104756) | `binder2-sg-modes` | GitHub HTTP 504 at 03:57:00 UTC |

Both failed in `artifact_receipt.py` while obtaining the exact run-attempt jobs
inventory. They had already verified and downloaded the same `windows-x64`,
`framework`, `arm64`, and `binder2-sg-host` artifacts. The Windows 2025 job also
successfully verified `binder2-sg-modes` artifact `13602718334` two seconds before
the other job's failed inventory read. Neither job produced
`binder-full-output.txt`; neither failure is a Binder runtime result or an
archive-integrity rejection.

The receipt implementation and Binder verification/runner scripts were unchanged
between the initial audit head `3843241` and `79567dad`. Separate CI log review
also identified the precise Go HTTP/2 error
`stream error: stream ID 1; CANCEL; received from peer` in the ordinary web
supplier verification path. That observed form is the only non-HTTP-status
transport retry added here.

## Bounded transport correction

`tools/ci/github_read.py` provides `get_json(endpoint)` and a CLI:

```sh
python tools/ci/github_read.py 'repos/OWNER/REPO/actions/runs/RUN/attempts/ATTEMPT/jobs?per_page=100'
```

The helper issues an explicit GET to the exact supplied endpoint. Its argument
vector ends option parsing before that endpoint, so it cannot introduce a new
method, host, body or pagination option. It makes at most four attempts, with
1-, 2- and 4-second delays, only for GitHub's reported HTTP 502/503/504 or the
observed HTTP/2 stream CANCEL form. A reported permanent HTTP status takes
precedence over incidental transport-looking text. Other statuses, generic
resets/timeouts, command-launch failures and invalid JSON fail immediately.
Each subprocess has a 60-second ceiling; reaching that ceiling is not retried.

The CLI writes one complete JSON value to stdout only after a successful command
and parse. Diagnostics go to stderr. Operational or parse failure returns exit
1; argument-parser misuse returns exit 2. Failure exhausts the bounded attempts
and stays a failure; no partial stdout response is accepted.

`artifact_receipt.py` uses this helper for its existing run, jobs and artifact
metadata reads. Its repository/run/attempt paths and every identity, producer,
conclusion, digest and extraction predicate remain unchanged. Archive download
and verification are outside this helper and receive no new retries.

`tools/web3/verify_full_suppliers.ps1` uses the same CLI for its three existing
run, jobs and artifact inventory GETs. It checks the Python exit status before
`ConvertFrom-Json`, retaining the existing acceptance predicates and post-parse
checks. The ordinary Windows web regression jobs already depend on Python.

## Executed evidence and limits

```sh
python3 -m unittest discover -s tools/ci -p test_github_read.py -v
python3 -m py_compile tools/ci/github_read.py tools/ci/artifact_receipt.py tools/ci/test_github_read.py
git diff --check
```

**Result: 15 focused Python tests passed.** They execute the production helper
against controlled subprocess outcomes, with sleeps observed instead of waited.
They prove exact GET identity across retries, recovery for each allowed failure,
success on the fourth attempt, bounded exhaustion, permanent-status precedence,
rejection of unobserved transport errors, rejection of nonzero exits with
valid-looking stdout, parse-failure behavior and the CLI stdout/stderr contract.

The integration cases execute the actual `artifact_receipt.py` with controlled
GitHub responses. They verify unchanged repository/run/attempt paths and exact
verified-download arguments after transport recovery. Nine semantic mutation
cases still reject wrong source, wrong attempt, failed or ambiguous producers,
invalid/expired artifact metadata and creation outside the producer interval,
without retrying the semantic failure or downloading an artifact.

The PowerShell consumer change was mechanically checked against its previous
version: reverting just the three transport replacements reproduces the original
non-transport code. PowerShell execution and a successful rerun of the affected
GitHub jobs remain CI evidence to collect; the local Python tests do not claim
either result.
