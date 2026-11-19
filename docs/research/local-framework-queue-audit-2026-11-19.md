# Local framework queue audit

On 2026-11-19, 51 recent/current CI runs and successful reference runs were
inspected through the GitHub Actions API. Sixteen had both framework producer
jobs completed successfully: three used `gesher-wsl-trial`, thirteen used
GitHub-hosted runners. This is an observational sample across different PRs,
source revisions and cache states, not a controlled hardware benchmark.

| Both framework jobs together | Local (3 runs) | Hosted (13 runs) |
| --- | --- | --- |
| Median active work | 30.2 min | 29.5 min |
| Median sum of work and job queue times, computed per run | 82.9 min | 44.9 min |
| Observed range of that sum | 71.4-106.7 min | 28.5-88.3 min |

Queue times are job `started_at - created_at`; active work is
`completed_at - started_at`. These measures do not include earlier root-CI
prerequisites and do not establish full CI elapsed-time changes. Finished local
framework jobs lagged their native `build` completion by 29-75 minutes in these
three runs; other independent jobs and downstream Windows tests also affect
the full critical path.

Local examples:

- [44310994957](https://github.com/gesher-runtime/gesher/actions/runs/44310994957): 30.2 min work, 76.5 min queue.
- [44309605764](https://github.com/gesher-runtime/gesher/actions/runs/44309605764): 30.4 min work, 52.5 min queue.
- [44302366944](https://github.com/gesher-runtime/gesher/actions/runs/44302366944): 30.2 min work, 41.2 min queue.

The earlier 12-second warm local host-tools Build step was real, but that
single-step improvement did not establish an overall benefit. Across PRs, the
single runner serialized source acquisition, host tools, framework compilation
and artifact uploads. At this snapshot eight more self-hosted framework jobs
were queued for 33-138 minutes. Additional older live runs were subsequently
found during the complete rollback scan.

The operator explicitly selected hosted rollback. Both `GESHER_SELF_HOSTED_PRS`
and `GESHER_SELF_HOSTED_TRIAL` were set false. Thirty current CI workflows with
still-active local framework jobs were cancelled and rerun in full, retaining
same-attempt source/artifact receipt checks. CI runs whose local work was already
complete were left alone. The build runner was confirmed online and idle after
migration. This does not disable existing approval or CI requirements.

The new dedicated merge-controller group is a separate proposal: short main-only
API operations should not share the build runner's queue or guest credentials.
