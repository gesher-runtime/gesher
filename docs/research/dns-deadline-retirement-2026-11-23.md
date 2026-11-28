# DNS deadline fixture: observe actual owner retirement

Date: 23 November 2026. Base: `3b2eaca95b5ddd9cee3c3db4e430460e512eb355`.

This common fixture repair preserves the complete production `DnsResolver`, its
30-second timeout, the fixture's absolute 40-second acceptance budget, and the
runner's 75-second per-process bound. It fixes an observation made before the
inline callback/Executor owners are required to have retired. The raw CI failure,
ordinary passing baseline, complete candidate run, compiled negative controls,
independent replay, source identities and main composition are retained in
[the adjacent JSON record](dns-deadline-retirement-2026-11-23.json).

## Actual failure and the limit of the evidence

[Fork run 45179176769, Windows job 137727834614](https://github.com/yom-chadash/gesher/actions/runs/45179176769/job/137727834614)
passed 3,973 checks in the general DNS case. Its next fresh JVM waited for the
production deadlines and failed `queued timeout unlinks; timed-out uncancelable
lookups keep their slots` at the immediate snapshot in `deadlines()`.

The log identifies the complete production resolver as
`7e1f52cde992719220a3576c05f8289569339976215d904c4eda74dfca5daba2` and the pinned
Android `CancellationSignal` as
`a671b35dfef0e319cc2855551d696aca845bc6ce25c59bb18867de5386048adc`.
Those files, the original runner and all original DNS fixture sources are
identical between the failed PR head `ec2ed793952d` and the current main base.
The failure therefore belongs to a shared fixture/source boundary; this change
does not modify the already merged #298 branch.

An ordinary unmodified Linux replay passed 88 deadline checks in 30,020 ms.
The historical log contains no ownership snapshot or thread dump at the failing
instruction. The controlled case below proves a real invalid observation in the
fixture, but it does not establish the precise interleaving of that Windows run.

## Why the immediate snapshot can be premature

The actual callback counts down its latch in its own `finally` block. Once the
last count reaches zero, the waiting test can resume before the callback returns
to `Request.deliver`. The production owner then still has to clear
`callbackOwned`. For an inline Executor, the dispatch path also has to return
from `Executor.execute` and clear `dispatchOwned`.

Production `retireLocked` correctly retains admission while any of `preparing`,
`lookupOwned`, `dispatchOwned` or `callbackOwned` remains. Consequently, a valid
snapshot immediately after the latch can show 33 admitted requests: the 32
uncancelable native lookups and the queued request whose callback or Executor
has not yet retired. Its lookup queue entry can already be removed.

This ordering follows the
[JDK 21 CountDownLatch contract](https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/concurrent/CountDownLatch.html):
the latch publishes preceding actions, not cleanup performed after the signal.
The [Executor contract](https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/concurrent/Executor.html)
also permits inline execution. There is no production requirement that callback
notification wait for all of these separate owners to retire.

## Repair and unchanged acceptance conditions

After all 33 timeout callbacks, the fixture now waits for one synchronized
snapshot containing exactly 32 requests and an empty lookup queue. The wait uses
`started + 40 seconds`, the original deadline measured from submission. The
existing elapsed-time assertion, `29,000 <= elapsed < 40,000 ms`, is checked
after this retirement observation. The total accepted interval is not extended.

The fixture still requires exactly 33 errors and zero answers, exactly 32 actual
native calls still active, no replacement native work, recovery after their
real return, no duplicated late callback, empty internal queues, retired timers
and all byte credits returned. No production source, lookup/callback ownership
predicate, worker budget, timeout, Windows identity check or provider behavior
is changed.

## Controlled real-request proof and compiled negatives

The new `deadline-retirement` case creates 32 real resolver requests whose
existing explicit native adapter is held at its return boundary. A 33rd actual
request is queued. Its Executor runs the callback and then remains held before
returning. The case invokes the actual private `Request.timeout()` transition
under a controlled schedule. **This controlled transition is separate from the
unchanged case that actually waits for the production 30-second timer.**

Before releasing the Executor, the case checks all 33 timeout callbacks, empty
lookup/completion queues, 33 still-admitted requests and 32 still-active native
calls. An observer running the repaired predicate must reach its bounded wait
and cannot finish while the Executor remains held. After the actual Executor
return it must observe exactly 32 requests. Finally both gates are released,
the observer must terminate, and the existing drain verifies actual ownership,
queue, timer and byte-credit retirement.

`tools/test_dns_deadline_retirement_controls.py` compiles the complete production
resolver and the same explicit platform adapters for each isolated variant:

| Variant | Required result |
| --- | --- |
| Current source | Exit 0, 54 retirement checks, no stderr |
| Restore the original immediate two-line observation | Exit 1 at `callback notification is not Executor retirement`, with the original retirement assertion as its cause |
| Disable the actual queued lookup removal in `timeout()` | Exit 1 at the check requiring lookup and completion queues to be empty |
| Remove the actual `lookupOwned` retirement guard | Exit 1 at the check requiring all 32 native owners plus the held Executor request to remain charged |

Every negative must compile successfully and fail at its exact expected
assertion. A compilation error, process timeout or unrelated failure does not
count as evidence. Production mutations exist only in those isolated control
directories. The checked-in production resolver is unchanged.

## Validation and reproducibility

The original runner now executes `all`, `deadline-retirement`, `deadline` and
`delayed-deadline` in separate JVMs. The recorded complete candidate invocation
passes with the following actual output:

| Case | Recorded result |
| --- | --- |
| General production contracts | 4,026 checks |
| Controlled retirement | 54 checks |
| Real production deadline | 88 checks, 30,084 ms, 32 native owners retained |
| Delayed deadline | 44 checks |

The general case has concurrent/path-dependent check counts. Its archived 3,973
and current 4,026 observations are both retained; no fixed count is fabricated.
The initial controlled prototype with 53 checks is also retained separately.
The final source adds the explicit empty-queue assertion and reports 54.

Root and Media independently ran all four separately compiled control variants.
The raw record preserves their full commands, logs, return codes, assertions
and SHA-256 identities for every Java input. All seven current runner, fixture,
production and workflow inputs are identical before and after advancing from
main `bcfb5044029d` to `3b2eaca95b5d`.

To reproduce with JDK 21:

```sh
python tools/test_dns_resolver_admission.py --java-home "$JAVA_HOME"
python tools/test_dns_deadline_retirement_controls.py --java-home "$JAVA_HOME"
```

The second command also accepts `--build-output` pointing to an empty directory
to retain compiled sources, compilation/run logs and receipts. Both the Linux
DNS step and the Windows host-contract step retain their original full runner
and additionally run the compiled controls.

Local evidence is Linux host execution using the real Java resolver and an
explicit native test adapter. It is not actual Windows DNS-provider, ART,
application or complete CI acceptance. A genuine manual full fork `ci.yml` run
must validate the published head on the configured Windows hosts; its result
will be linked in the PR rather than inferred from these controls.
