# Bounded local ephemeral VM evaluation

Date: 2026-11-18. Source: `effbbf5e52d1a5596e2bb6855ce066abfaf5cb26`.

The local workstation can create clean Hyper-V Linux environments for jobs,
but the tested conservative configuration did not improve execution time.
Keep automatic routing on GitHub for now. This is a feasibility experiment,
not a production fork-runner deployment or an isolation security audit.

## Observed execution

The exact 16-command `JNI and mixer allocation bounds and surface image-cache
lifetime` step from `artifact-preflight` ran successfully on both platforms.
The same tracked source commit was used; no tests or acceptance predicates
were relaxed. The local invocation used an unprivileged guest account with
an empty environment and no GitHub token or shared host folders.

| Measurement | Local test VM | GitHub Ubuntu 24.04 |
| --- | --- | --- |
| Production test group | 218.50 seconds | 56 seconds |
| Result | Exit 0, all fixture checks passed | Success |
| Queue delay | Not applicable to direct invocation | 50 seconds |
| Complete hosted job including setup | Not measured locally | 66 seconds |

Hosted evidence: [run 44302202538](https://github.com/gesher-runtime/gesher/actions/runs/44302202538),
job `134794512179`. The hosted workflow was a separate manual evaluation branch,
with read-only contents permission and checkout pinned to the measured source.
It did not change main, PR checks, approval rules or production routing.

The local VM had two virtual CPUs, Hyper-V maximum CPU 50% and low weight,
dynamic RAM 768 MiB minimum / 1 GiB startup / 3 GiB maximum, and a 20 GiB
dynamic disk. This deliberate CPU restriction is part of the result; it is
not an equal-resource hardware comparison. Other CPU allocations could change
the outcome and were not benchmarked. `/usr/bin/time -v` reported maximum
child-process RSS 427,608 KiB and no swapping. Guest idle memory was observed
around 322-359 MiB, with about 895 MiB visible RAM at those samples.

## Fresh-environment checks

Compilers were installed once into a clean Ubuntu 24.04 template. The template
was cleanly shut down before copying it as the parent of disposable differencing
disks. A private NAT/switch did not replace existing host networking.

The first job wrote a sentinel and extracted the source under its runner home.
After shutdown, that entire VM and its child-disk directory were removed. A
new generation-2 VM with a new child disk was then created from the untouched
clean parent. Neither the sentinel nor the previous checkout existed in the
second VM. The second VM passed its first observed SSH/isolation check within
24.75 seconds of its recorded start; this is an upper bound on readiness,
not a precisely measured minimum boot time.

The guest runner account could not use sudo or read the separate management
account's SSH directory. The active guest nftables rules rejected new IPv4
private/link-local and IPv6 private/link-local connections, while allowing
established management replies. These checks do not prove resistance to guest
kernel or hypervisor escapes. No external contributor code was executed and
no GitHub self-hosted runner was registered in this experiment.

## Remaining requirements for external PRs

An actual deployment would still need a trusted host lifecycle controller,
job-scoped read-only credentials, ephemeral runner registration, reliable cleanup
after crashes/cancellation/timeouts, concurrency control, log retention, safe
cache separation and a full preflight validation. A constant VM or just the
runner's `--ephemeral` option does not establish fresh guest state per job.

The experiment's VMs, child disks, prepared test parent and dedicated NAT/switch
were removed after the checks. Existing host networking was retained. The
previously downloaded original image outside the experiment remains available;
no experiment VM consumes idle CPU/RAM.
