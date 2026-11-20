# CI artifact API quota — 2026-11-19

## Evidence and scope

Baseline main: `f4d6e6c1095ca0d7407f046964b56ce00b54b21a`.

Multiple consumers failed before product execution on 2026-11-19 at
14:10–14:14 UTC. Jobs `135617779113` and `135620659853` failed on the
first current-run JSON read with GitHub's explicit installation quota message.
The previous JSON helper did not request response headers, so these logs do not
identify their exact bucket, remaining count or reset time. Two other consumers
reported `api-http-403` from the archive redirect; their old sanitized diagnostics
do not establish whether those responses were quota errors or authorization errors.

Separately, controller job `135613634148` recorded a 403 response at 14:13:50 UTC
with `x-ratelimit-limit=5000`, `used=5000`, `remaining=0`, `resource=core`, and
`reset=1795099648` (14:47:28 UTC). These are observations for that controller
request, not proof that every CI token shared its bucket. GitHub's documented
GITHUB_TOKEN default and an observed effective limit must not be conflated.

The main CI definition contains 163 receipt invocations in a fully reached
workflow: 114 in eight test shards, including two service-only capture receipts.
Each ordinary receipt independently reads the run, one or two pages of jobs,
a name-filtered artifact inventory, and an archive redirect. Without retries,
that is 652–815 API requests from these receipt invocations alone. Other jobs,
artifact services and simultaneous workflows are additional consumers.

## Consumer batch

The committed `tools/ci/test_shard_artifacts.json` lists the same 14 core
name/producer/destination mappings previously passed as separate CLI arguments.
All eight test shards use it. The service-only capture artifact remains a
separate receipt with its existing role verifier. Product test commands, matrix,
needs, permissions, runner selection and timeouts are unchanged.

One batch performs these operations:

1. Parse the strict manifest, reject duplicate/unknown fields and Windows path
   aliases, and bind its actual parsed bytes to the exact checkout's Git blob.
   Only Git's CRLF text conversion is permitted.
2. Read the live run and a complete exact-attempt job inventory. Read the full
   paginated artifact inventory, using the existing bounded count/unique-ID checks.
3. Apply the original producer predicates and the existing #60 artifact selector
   separately to every name. Failed producers are never permitted in a batch.
4. Download each selected archive through the existing digest and safe ZIP
   extraction path into a private staging tree. No assembly path is published.
5. Re-read jobs and artifacts. Reapply every predicate and require each selected
   producer and artifact identity to match its first proof. Unrelated new jobs or
   artifacts do not invalidate a batch. The final API read revalidates the live
   run, source and attempt; the checkout is checked again immediately afterward.
6. Exclusively create the destination container and rename the entire verified
   artifact tree into it. Existing destinations are not overwritten. Failures
   remove staging and do not print successful receipt markers.

There is no metadata cache across processes, jobs, runs or attempts. The original
single-artifact CLI and its narrowly scoped failed-installer exception remain
available. No new failed-producer exception is accepted by batch mode.

The integration fixture executes the production selectors and extractor, adapting
only GitHub/remote-storage transport. For 73 jobs and 14 artifacts it counts
8 JSON reads plus 14 archive redirects: **22 API requests**, versus 70 separate
receipt requests with two job pages. With 103 artifacts initially and an unrelated
104th artifact on revalidation, it counts 10 JSON reads plus 14 redirects: **24**.
Thus the eight core batches save 368–384 API requests for these demonstrated
inventory sizes. Counts exclude retries and other CI API users; this does not
promise unlimited parallel workflow capacity.

## Quota recovery and diagnosis

JSON reads use `gh api --include` so an error's status and headers are available.
A 403 or 429 is quota-retryable only with GitHub's explicit quota message and
sufficient valid timing evidence. Generic forbidden requests, malformed framing,
conflicting statuses and authentication failures remain terminal.

Primary exhaustion requires remaining=0, a valid reset epoch and the response's
server Date. Retry-After, when supplied, must also be honored. A documented
secondary quota response without Retry-After waits at least 60 seconds. Numeric
limit/used/remaining/reset and a bounded resource name are the only extra headers
reported; archive diagnostics do not expose the response body, credentials or
signed storage URLs.

JSON reads keep at most four requests and the original maximum read envelope of
247 seconds. Rate-limit waits have a cumulative 120-second ceiling within that
envelope. Archive downloads keep their original three-attempt bound and add a
120-second cumulative quota-wait ceiling. A distant reset fails with diagnostics;
there is no blind one-second retry of an unidentified API 429. Existing archive
5xx/transport recovery, isolated storage credentials and all integrity checks remain.
No workflow timeout, permission or branch protection is enlarged.

The small quota-classification policy is duplicated in the standalone archive
helper deliberately: historical trusted-source consumers acquire no new unchecked
import dependency. Tests exercise both policies against the same valid and
adversarial responses. Versioned copies of other artifact helpers remain pinned
to their historical reviewed source and are not silently replaced.

## Validation and remaining proof

- 43 reader/receipt tests, including all 31 prior receipt tests.
- 18 batch integration tests: actual ZIP extraction, paginated/growing inventory,
  exact-attempt and source rejection, changed producer/digest, late same-name
  artifact, partial failures, committed-manifest binding, Windows aliases/CRLF,
  existing/racing destinations, symlinks and rename failure.
- 13 existing downloader tests and 15 archive/assembly negative controls.
- Actual local gh 2.46 executed a loopback-only 403-to-200 sequence with the new
  reader. It observed real status/header framing and preserved the exact JSON.
  This test used no GitHub request or real credential.

These are host and controlled transport results. Fresh Windows CI is still
required for production artifact downloads and Windows directory publication.
A real live quota recovery has not been claimed. CI starts should be paced so
parallel workflows do not immediately consume the shared server-side allowance.

## Primary references

- [GitHub REST rate limits and response headers](https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api).
- [GitHub API best practices and quota backoff](https://docs.github.com/en/rest/using-the-rest-api/best-practices-for-using-the-rest-api).
- [GitHub CLI api and --include](https://cli.github.com/manual/gh_api).
