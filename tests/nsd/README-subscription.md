# Continuing service-info callback fixtures

These fixtures cover the six selected SDK36 subscription declarations and their
Java/service/native ownership. The [research note](../../docs/research/nsd-windows-subscription-2026-11-18.md)
records the source profile, measurements and remaining Windows qualification.
Each fixture uses owned ordinary records. It does not enumerate unrelated local
services.

## Genuine application artifacts first

Build the complete application and services tests with their ordinary scripts:

```sh
bash app/build.sh /path/to/fresh/app \
  --framework-classes /path/to/genuine/framework-classes \
  --core-classes /path/to/genuine/coremod
bash tests/dex-services/build.sh /path/to/fresh/services \
  --framework-classes /path/to/genuine/framework-classes \
  --core-classes /path/to/genuine/coremod \
  --app-classes /path/to/fresh/app/gesher-app-classes.jar
```

The build scripts' normal pinned JDK, D8, AOSP, annotation and APK-signature
dependencies are required. Extract the resulting `gesher-app-classes.jar` into a
fresh directory; do not substitute SDK stubs or a small hand-compiled replacement
for the runtime providers.

```sh
python3 tools/test_nsd_subscription_contracts.py \
  --java-home /path/to/jdk \
  --app-classes /path/to/extracted/fresh-app-classes \
  --framework-classes /path/to/genuine/framework-classes \
  --core-classes /path/to/genuine/coremod \
  --apksig-jar /path/to/apksigner.jar \
  --output /path/to/new/subscription-host --loopback-peers
```

The runner records commands, source/class hashes, elapsed times and owned-peer
receipts. It compiles the complete maintained fixtures afresh. `--suite state`,
`manager`, `service` or `worker` selects a particular investigation; multiple
`--suite` arguments may be used. The manager/client fixture includes definite
pre-admission permission denial: the original synchronous exception must leave
no callback, retained operation, death recipient or retry work. Ambiguous accepted
transactions retain their exact cleanup owner separately.

The host native initializer and Android Cleaner module bridge only support the
explicit JDK fixture boundary. They are never packaged in the application.
The manager/client and service fixtures deliberately supply authority, executor,
driver and Binder-lifetime observations. They do not execute remote Binder or
Windows networking. The worker's two loopback modes use actual UDP/TCP and a
separate standard-library Python producer, while their driver explicitly supplies
loopback packet attribution. Both facts are recorded in the result markers.

## Source and provider audit

```sh
python3 tools/check_nsd_subscription_provenance.py \
  --java-home /path/to/jdk --android-jar /path/to/android-36/android.jar \
  --framework-dex /path/to/actual/framework.jar \
  --application-dex /path/to/fresh/app/gesher-app.jar \
  --application-classes /path/to/fresh/app/gesher-app-classes.jar \
  --core-classes /path/to/genuine/coremod \
  --upstream-sources /path/to/pinned/raw/upstream \
  --output /path/to/new/subscription-providers.json
```

The seven raw upstream inputs must match the checked-in provenance manifest.
The audit reproduces imports, verifies all six selected SDK declarations in real
classfiles and DEX, checks the separate eight-entry subscriber JNI table, and
resolves selected Android/Gesher member references. It explicitly distinguishes
actual framework/application DEX from inherited core compile declarations.

## Ordinary Android reference

```sh
python3 tools/build_nsd_subscription_reference.py \
  --java-home /path/to/jdk --android-jar /path/to/android-36/android.jar \
  --build-tools /path/to/build-tools --output /path/to/new/subscription-reference
```

The builder copies the exact source/manifest into an immutable capsule, compiles
against official SDK36, signs a debug fixture APK, verifies its signature and
permissions, and emits explicit setup/execution/cleanup JSON arrays. It does not
run ADB. A separately reviewed device run owns installation, launch, result
collection and cleanup.

The ordinary Activity asserts actual application/Binder identity and Network,
public callback execution on its executor, selector validation, identity reuse,
binary/null/empty TXT, update/loss/reappearance, stop and late-packet suppression.
Its peer sends only its unique owned service's valid records and serves a real
TCP greeting. The pinned base-PTR goodbye assertion is deliberate: a differing
installed Android module must be reported as a profile difference or investigated,
not silently reclassified as a pass. Preserve every executed capsule and result.

## Native helper and actual Windows gates

The [native subscription instructions](../net/README-nsd-subscription.md) describe
the 97 finite continuity/cancellation checks and genuine x64/ARM64 compile/link
runner. Cross-builds do not execute Windows.

The services track calls `tests/windows/run-nsd-subscription-contract.ps1`. It
requires a real IPv4/IPv6 adapter and starts only its owned independent multicast
peer. `NsdSubscriptionWindowsTest` invokes the production subscriber backend and
its native driver, verifies an actual TCP endpoint, observes real native resource
progress and waits for retirement. An absent selected Network must send nothing.
Missing IPv6 is an unqualified gate, not a skip reported as success.

This native transport gate explicitly reports that it is neither an ordinary
installed application nor a remote Binder test. Separate installed-app and
service-process runs must still qualify permission checks, death/uninstall,
service replacement and cancellation ownership across actual process boundaries.
No helper starts, restarts or changes CI.
