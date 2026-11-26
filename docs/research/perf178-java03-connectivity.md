# JAVA-03: connectivity observation follows current demand

This is the connectivity ownership leaf of [issue 178](https://github.com/gesher-runtime/gesher/issues/178).
It does not complete JAVA-03, its physical-device acceptance, or the epic's
whole-process idle/CPU/RAM targets. The implementation starts from
`751d82310f7c4c470c098ccebfd5d2d04699a0cd`, verified through both Git fetch and
GitHub's `main` commit metadata on 22 November 2026.
The source and tests were frozen for independent review at
`6abea83774fe2e9b718e0d0ee1a22df06754be54` (tree
`4848458d4c89fd75f6e4a00e128091bbd6a58478`). This report is a separate
documentation-only addition to that source candidate.

## Contract and ownership

[Android's network callbacks](https://developer.android.com/reference/android/net/ConnectivityManager#registerNetworkCallback(android.net.NetworkRequest,android.net.ConnectivityManager.NetworkCallback))
remain registered until removed or their process disappears. The client already
releases its shared service subscription when its final callback/PendingIntent
request ends. The service now also releases that subscription's death recipient
and observation demand; a failed death-link cannot acquire either resource.

[Context.registerReceiver](https://developer.android.com/reference/android/content/Context#registerReceiver(android.content.BroadcastReceiver,android.content.IntentFilter))
distinguishes a registered receiver from a null-receiver sticky query. Both must
return the current connectivity sticky state, but only the former requires
subsequent broadcasts. This change preserves that distinction through the actual
BroadcastController receiver publication/removal lock. ReceiverList generations
own tokens, so duplicate filters do not multiply demand and process death removes
all of that generation's tokens. Filters which cannot match the data-less
connectivity action do not subscribe. Existing receiver identity, permission,
export, matching and delivery checks remain in their original paths.

[Network-constrained jobs](https://developer.android.com/reference/android/app/job/JobInfo.Builder#setRequiredNetwork(android.net.NetworkRequest))
must still receive a satisfying network and react to constraint loss. A
[persisted job](https://developer.android.com/reference/android/app/job/JobInfo.Builder#setPersisted(boolean))
must be recovered at boot with its original constraint. JobScheduler acquires one
system listener while any scheduled job needs a network, including pending,
periodic, retrying and active jobs. Every job-list mutation updates this ownership
under the scheduler monitor, including persisted loading, replacement, completion,
cancellation and package removal. Jobs that need only latency, charging, idle or
storage do not acquire network observation. Persistence format, admission,
permissions, retry policy, deadlines, network matching and stop reasons are not
changed.

| Owner or operation | Network observation | Release or refresh |
| --- | --- | --- |
| App's shared IConnectivity listener | One record per Binder endpoint | Explicit removal, death, or failed delivery; unlink the exact record |
| Matching registered CONNECTIVITY_ACTION receiver | Token for its ReceiverList generation | Unregister or owning process death |
| Null receiver / getSnapshot query | No observation lease | Read current state when unobserved |
| Any job with a required network | One JobScheduler system listener | Final applicable job completes, is replaced/canceled, or its package is removed |
| Persisted network job | Reacquired during start/loadPersisted | Same lifetime as an ordinary scheduled job |
| Resume while observed | Refresh current native state | Detach the resume listener when observation ends |

The runtime's existing implicit connectivity broadcast goes to registered
receivers. BroadcastController's existing manifest dispatch rules are unchanged;
this PR does not claim to implement additional legacy manifest-receiver support.
The audit found no other callers of ConnectivityService.addSystemListener.

## Lifecycle and bounded work

ConnectivityService's demand flag is distinct from its Thread field. With no
owners, it clears the cache and published-snapshot state and removes its
WindowsHost subscription. A draining thread therefore cannot make a query serve
stale state. A first owner resynchronizes the cache, reusing that draining thread
when present; only the departing thread can hand its slot to a new one. The host
fixture observed at most one native waiter, including churn and concurrent
registration/removal.

Native interface/address/route events still drive active observation. The
15-second fallback remains necessary for Network List Manager properties such as
validation and cost, for which this native implementation does not subscribe to
a comprehensive notification source. Resume also requests a refresh directly,
without relying on an IP notification. JobScheduler invalidates the cached query
state before handling its own resume event, because its WindowsHost callback can
run before ConnectivityService's callback. The last published snapshot remains
separate, so that fresh query cannot consume an application notification.

Refresh requests have one dispatcher and one pending bit. Overlapping signals
are coalesced before the next observation; there is no snapshot queue, extra
executor, or unbounded task accumulation. The service serializes snapshot
publication but invokes system listeners, Binder callbacks and broadcasts outside
its monitor. Receiver demand callbacks only update ownership; they never deliver
application callbacks inline under BroadcastController/StickyBroadcasts locks.
An already admitted/in-flight callback is not retracted by unregister.

### Native boundary that remains

`WindowsNetworks.nativeWaitForChange` has no cancellation API. Removing the last
owner does **not** interrupt that native call. Its final wait can drain for up to
15 seconds, plus the existing 300 ms debounce when an event arrives. The Java
loop checks demand before reading again, then exits; it starts no further
periodic wait or snapshot. The host trace explicitly drains the already admitted
wait before its stabilized interval.

The native event and the three IP Helper notification registrations are still
process-owned (`std::call_once` in `compat/net/windows_networks.cc`). This PR
does not free those native registrations, measure their handle cost, or claim
immediate complete native teardown. It does retire the Java polling/snapshot
loop and its Java thread. Native network files are unchanged; open #268 owns
adjacent NETLINK_ROUTE/network work. WindowsHost's shared dispatcher lifetime and
timeout remain separate work in #251. Bluetooth, sensor sharing, other service
housekeeping and the eight-hour soak remain outside this leaf.

## Validation and measurements

All local results use the actual changed Java source, not an extracted observer
model. The native boundary supplies controlled snapshots and a blocking host
wait; Binder endpoint/death operations and package/process metadata are isolated
fixtures. The actual ConnectivityManager, ConnectivityService, StickyBroadcasts,
AMS/BroadcastController and JobScheduler transition code executes.

| Local check | Observed result | Boundary |
| --- | --- | --- |
| Existing connectivity callback registry | 80 assertions passed | Controlled Handler scheduling; no real Binder transport |
| Existing query freshness | 9 assertions passed | Actual query/cache path; controlled native snapshots |
| Demand lifecycle and integration | 74 assertions passed | Actual observers/receiver delivery/scheduler worker; 10,000 sequential plus 2,000 concurrent attach/remove cycles |
| Existing scheduler constraints | 397 assertions passed | Production service logic and native host boundaries |
| Existing scheduler/alarms/broadcast work | 155 assertions passed | Same existing host fixture; no weakened assertions |
| Full app Java/DEX build | Passed | Cached Android framework/core classes and genuine source compatibility metadata |
| Full services Java/DEX build | Passed | Includes the real-Parcel persisted-job target fixture |
| Windows x64 fixture cross-link | Passed with LLVM-mingw | PE build only; not Windows execution |

The demand fixture covers native-event handover to JobService.onNetworkChanged,
then a resume with no interface event stopping the running job with
STOP_REASON_CONSTRAINT_CONNECTIVITY. It uses the real scheduler thread and no
network-constraint override for that case. It also checks first/last receiver
ownership through actual register/unregister/processDied entry points, current
sticky-only queries, callback failure, stale death delivery, observation after
idle changes, fallback cost changes, and reentrant callbacks outside owner locks.

The new real-Parcel persisted job case runs from the existing Windows/ART
JobSchedulerConstraintContractTest entry point in track 94. It writes the job
through persist(), restores those bytes, starts a fresh scheduler, checks the
loaded required network and observation, and checks that cancellation removes
disk state and observation. **Its target execution is pending CI at this local
checkpoint.** Host fixtures deliberately do not emulate Parcel deserialization.

### Ten-minute no-owner control

The control compiles baseline
`751d82310f7c4c470c098ccebfd5d2d04699a0cd` against the same fixture. Each run
registers a listener, lets the native wait begin, removes the listener, drains
that final wait and observes 600,000 real milliseconds. Native timeouts remain
15,000 ms. No app, system or receiver owner remains during the interval.

| Metric during stabilized interval | Baseline | Candidate |
| --- | ---: | ---: |
| Native snapshot calls | 40 | 0 |
| Further native-wait calls | 40 | 0 |
| Retained Java connectivity watcher | 1 | 0 |

The baseline intentionally fails the no-owner assertion. Its separate demand
negative control also fails because explicit removal retains a death recipient.
The candidate assertions pass. The final paired run captured hashes and commands
before launch and checked that every source/fixture hash remained unchanged after
both runs. Raw source hashes, exact commands and interval results are recorded in
[`perf178-java03-connectivity-results.json`](perf178-java03-connectivity-results.json).

These are call-count and lifetime observations with a controlled host backend.
Windows network enumeration cost, OS wakeups/context switches, total CPU,
private commit/RSS, handles, foreground latency and multi-app results were not
collected; they are unavailable, not zero. The results establish removal of this
periodic Java work, not a whole-product CPU or memory ratio. Physical Windows
network/resume traces, real Binder process death, Wi-Fi/VPN/cost transitions and
the broader JAVA-03 acceptance remain open.

## Reproduction and build-input provenance

```sh
python3 tools/test_connectivity_callback_contracts.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" \
  --apksig-jar "$APKSIG_JAR" --case all

python3 tools/test_connectivity_callback_contracts.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" \
  --case idle-trace --trace-ms 600000

# Expected negative result on the original owner behavior:
python3 tools/test_connectivity_callback_contracts.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" \
  --case idle-trace --trace-ms 600000 \
  --source-ref 751d82310f7c4c470c098ccebfd5d2d04699a0cd

python3 tools/test_scheduler_work_contracts.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" \
  --apksig-jar "$APKSIG_JAR" --case all
```

The first local broad scheduler run failed the existing abandoned-job
compatibility assertion because the cached framework classes omit SOURCE-retained
ChangeId annotations and the initial source input contained only three unrelated
changes. The correction was to use the genuine pinned
`android-16.0.0_r4/apex/jobscheduler/framework/java/android/app/job` source directory
as build input. All 20 file hashes were rechecked against an independently
downloaded/extracted provenance receipt. The upstream archive is
<https://android.googlesource.com/platform/frameworks/base/+archive/android-16.0.0_r4/apex/jobscheduler/framework/java/android/app/job.tar.gz>,
SHA-256 `c3c5328cf7a91103cccd739386ab3772e8f1bf83ca975805c841f41a12162a38`.
JobParameters.java is
`644b94f9f10b94b3d55ff46b591a20375e86b63cc02426849f93c76a70b625f9`.
The unchanged CompatChangesGen produced ten relevant/in-repository entries,
including the original disabled change, with zero unresolved values. No table
value, permission, test assertion, workflow or source pin was overridden. The
ordinary CI framework build has its own complete AOSP source checkout.

## Composition

The complete open-PR filename inventory at 14:04 UTC contained no overlap in the
four production files. A later focused check found #245's test-only sensor setup
and cleanup in JobSchedulerConstraintContractTest (head
`6eda068919d90aab690b862ffa870c3348cb064a`): its four initialization lines and one
cleanup line apply cleanly to this candidate with `git apply --check`. That
checks textual composition only; integrating the sensor changes still requires
their full relevant tests. Their implementation is not imported here. No native
network, dispatcher, Account, Trust, UIA or window-session work is duplicated.
