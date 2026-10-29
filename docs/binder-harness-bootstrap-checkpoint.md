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
