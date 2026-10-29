# Ordinary trusted Binder fixture bootstrap migration

Source checkpoint only. The c7bd80f9 full 40860584124 failed actual duplicate
resource regression on both Windows runners: direct binderd launch lacked its
private ABI3 startup. That is a real compatibility failure, not an allowed skip.

The first narrow repair retains the existing raw ioctl, libbinder/death and
13-mode duplicate ownership assertions. Their trusted host owners now create
the production private Channel and ABI3 broker context, transfer actual retained
process handles with creation times, and wait for publication ACK before resuming
the ordinary children. Only the two reserved system fixture roles are needed;
the owner retains the private endpoint and never gives it to ordinary children.
An actual wrong-creation-time publication precedes honest owner admission and
must be rejected. Broker readiness, exact client PID/open counts, terminal
statuses and owned-job cleanup remain gates in the duplicate controller.

These are trusted test harnesses outside untrusted AppContainers. Their
publication authority does not demonstrate isolation of arbitrary same-user
guest code. Global Binder trust remains open. The environment selector in the
native parent only selects its already admitted honest UID; it cannot authorize
a process, replace the retained handle or publish through public Binder.

The narrow workflow builds the real current-source binderd, servicemanager,
both native probes and actual duplicate test, with current-run producer receipts
and executable hashes. Both Windows runners must execute the original tests.
No local native/Java/C# compilation or fixture execution is permitted.

Still required: independent frozen source peer before push/CI, actual component
results, migration of raw launches in tracks 50-services and 56-net with proper
ongoing writer wiring, and an immutable approved whole-source policy update
before a new full. That full must rebuild matched production host/API3 DLL,
current Java jars/images/APKs and pass all existing ordinary services, net,
Web3, process completion, package and e2e gates. No acceptance is claimed here.

## Narrow component 40866670764

Exact source c2bd1687 built successfully and its current-run receipts passed on
both Windows runners. Both actual raw ioctl and libbinder probes reported all
passed. The duplicate controller reached a real admitted Binder child, whose
17 assertions and original broker roundtrip passed and whose terminal status
was zero; the controller then correctly failed its exact-line completion gate.
The binary capture adapter had omitted Popen(text=True)'s CRLF normalization,
leaving a carriage return before the anchored regex end. The follow-up restores
that text I/O contract without changing any assertion. This source requires a
new two-runner component; the original failed run is retained as red.

## Bounded accepted component 40867373809

Exact source `467c27de80c4fdcec1512b6dfa07cd5cc5945837`, attempt 1:
build 123425446593, Windows2022 123425782774 and Windows2025 123425782841
all succeeded. Independent plain-log audit retained the build and both runner
logs under `C:/work/gesher-ci-review/binder-harness-408673-*.log`.
Both runner receipts identify this exact source and `40867373809/1/build`;
the five executable hashes passed. The build log contains the actual current
native compiler/link commands with the reviewed bootstrap header compiled into
both probes; the DLL/production Java/installed APK graph is outside this job.

Both runners actually emitted `binder probe: all passed` and
`libbinder probe: all passed`, including original raw calls, nested calls,
object transfers and death tests. Libbinder logs late bad-fd messages during
its intentional broker teardown; this is not evidence that production bad-fd
failures are fixed. The duplicate controller retained all 13 original modes,
with 8,399 actual assertions and all 63 required semantic case records per
runner; zero failures. Real raw publisher observations were 2,445 (2022) and
4,471 (2025), with 1,000 duplicates and zero errors each. Capacity actually
occupied all 8,192 descriptor slots before the cleanup gates passed.

Actual final gates on both runners:

```
ACTUAL_DUPLICATE_BROKER_AUDIT ... normal-exits=13 opens=2 unexpected=0
ACTUAL_DUPLICATE_BROKER_CLEANUP ... exit=211 reaped=true intentional-owned-kill=True clients-qualified=True
ACTUAL_DUPLICATE_JOB_CLEANUP children=14 reaped=true
ACTUAL_DUPLICATE_ORACLE modes=13 failures=0 semantics=bounded-resource-controls
```

Windows2022 broker PID3352, admitted Binder client PIDs5588/7192;
Windows2025 broker PID3596, client PIDs7652/8556. Each client had a real
fresh process and normal exit, and trace opens matched exactly those two PIDs.
Wrong-birth publication refusal precedes owner admission in all three fixture
owners; each original operation still depends on actual production challenge
and leases, with no public registration or unauthenticated broker fallback.

This accepts only migration of the duplicate controller and two native probes.
The historical full 40860584124 remains red. No integration/main import, full
source-policy change, full dispatch or inventory closure follows from this
component. Remaining work includes tracks50/56 repeated-client writer wiring,
production provisioning/Web3/e2e failures, current matched host/DLL and Java
artifacts, and an independently approved immutable whole-source full policy.

### Successor design for tracks50/56 (not implemented)

Use a tests-only Java owner calling existing public Zygote.startDevice. Its
designated writer entrypoint runs the actual SystemServer (or original
BinderTest service) plus a trusted host command reader. Fresh clients are
created through existing nativeSpawn from that sole writer; retained native
ExitStatus/Kill observe their exact terminal status. A fixture-local command
directory must remain outside guest write grants. Do not share the private
byte channel across host and writer, add public registration, replace actual
SystemServer with a pretend service, reuse a JVM for the fresh-process gates,
or grant a snapshot/header the authority to publish. Tests dex and boot-class
path classes have distinct classloaders, so package-private direct calls to
Zygote cannot be assumed to work; a tests-only reflective bridge to existing
methods is one option requiring exact-source Java/runtime CI and source peer.
