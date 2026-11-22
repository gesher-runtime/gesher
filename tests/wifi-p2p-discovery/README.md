# Wi-Fi Direct discovery value checks

These fixtures cover five complete pinned Android value sources, their three
builders, ordinary validation/aliasing and their original Parcel readers. They
do not start discovery, advertise a service, resolve a peer or use a radio.

The public fixture compiles against the official SDK36r02 jar alone. Its
disassembly has 36 direct references to the 41 selected SDK declarations; the
five `writeToParcel` methods are reached by ordinary `Parcelable` interface
dispatch. The separate extended fixture calls the hidden vendor-list methods
and response constructor. Hidden members do not increase the SDK denominator.

## Genuine source inputs

Use the five raw value files from Wi-Fi commit
`e3f09203409da7a91d2c5cf9f0109da6d5243319`, preserving the `nsd/` subdirectory
under `UPSTREAM_DIR`. `SUPPORT_DIR` contains the complete pinned manager,
OuiKeyedData, PersistableBundleUtils, ScanResult and WifiScanner files under
`framework/java/`, and complete modules-utils SdkLevel under
`java/com/android/modules/utils/build/`. The latter is pinned to
`e945bbce0ccb83e1111b6031a3bba89e6b5cac6f`. Exact hashes, public source links,
constant bindings and complete token comparisons are in the
[provenance receipt](../../docs/research/wifi-p2p-discovery-values-provenance-2026-11-18.json).

The reference retains the manager's original qualified constant expressions.
It compiles those constants using only the original SDK manager class in a
separate compile-only directory. That class is never put in a runtime payload.
Neither graph contains a manager implementation or replacement Parcel.

## Host execution and exact absence control

The following variables identify already verified local inputs. `APP_CLASSES`
is the complete classes jar from the normal app build. Use a fresh output
directory for each command; the tools reject an existing output directory.

```sh
python3 tools/test_wifi_p2p_discovery_values.py \
  --java-home "$JAVA_HOME" --sdk-jar "$SDK36_JAR" \
  --framework-classes "$FRAMEWORK_CLASSES" --core-classes "$CORE_CLASSES" \
  --app-classes "$APP_CLASSES" --upstream-dir "$UPSTREAM_DIR" \
  --support-dir "$SUPPORT_DIR" --profile candidate \
  --output-dir "$ARTIFACT_DIR/host-candidate"
```

Run the same command with `--profile pinned-r4` and a different output
directory for the fresh reference graph. Both graphs compile complete value
sources and genuine complete helpers. The host uses the existing genuine
framework initialization boundary and provides no Parcel implementation.
The shared check counter is printed once after both suites finish.

For the old-source control, use `--source-ref "$ABSENCE_REF"` instead of the
profile argument. The selected baseline must actually lack all five value
source paths. The closed loader forbids fallback to cached framework or SDK
definitions. The public fixture resolves and its baseline body fails at the
genuinely absent `WifiP2pDiscoveryConfig.Builder`. This expected failure
is retained as negative evidence, not counted as a passing behavior test.

## Native capsule preparation

```sh
python3 tools/prepare_wifi_p2p_discovery_oracle.py \
  --java-home "$JAVA_HOME" --sdk-jar "$SDK36_JAR" \
  --framework-classes "$FRAMEWORK_CLASSES" --core-classes "$CORE_CLASSES" \
  --app-classes "$APP_CLASSES" --d8-jar "$D8_JAR" \
  --upstream-dir "$UPSTREAM_DIR" --support-dir "$SUPPORT_DIR" \
  --output-dir "$ARTIFACT_DIR/android-capsule"
```

Preparation compiles and hashes stock, pinned, candidate and launcher artifacts.
It writes explicit setup, profile-run and cleanup request JSON files; it does
not execute or queue them. Independent review must verify the frozen source,
toolchain, DEX definitions and classloader graph before the authorized oracle
owner executes the requests.

The source-owned graphs include the five values, builders, original CREATORs,
complete OuiKeyedData/PersistableBundleUtils and complete relocated SdkLevel.
D8's four empty synthetic constructor access markers stay in that same graph.
Parcel, Parcelable, PersistableBundle, Build and MacAddress are explicit real
Android boot leaves. Constructor/getter/CREATOR identities are checked before
the first record. The DirInfo comparison therefore qualifies its use of boot
MacAddress and does not qualify the candidate app MacAddress on Android.

The stock loader contains only callers. It records every selected type's
presence. A stock image lacking modern types attempts the six common
DiscoveryConfig cases; successful execution must be observed before those
cases can be qualified. Presence does not prove agreement with the newer pinned
source profile; any observed behavior difference remains a separate result.
The public response observer accepts Parcelable and performs its modern cast
inside the modern-only path, avoiding an eager public-signature dependency.
Private lambda descriptors still reference modern builders, so host reflection
evidence does not guarantee ART verification behavior.

The complete source profile requires 17 ordinary records, all with the full
original reader, exact record consumption, fresh returned objects and identical
rewritten bytes. There are no writer-only cases. Invalid setter arguments are
ordinary API rejection checks. Host-only constructor observations with null or
uninterpreted values are never serialized into the native corpus.

## Production Windows gate

Track96 runs `gesher.services.WifiP2pDiscoveryValuesContractTest` with the
production boot classpath. The fixture checks coherent boot providers before
printing its profile marker. The gate requires exactly 17 ordered original
readers, both hashes for every record, the pinned reference profile and the
single after-all-suites counter. Partial logs, host-only output, duplicate cases,
wrong readers and incorrect initial or rewritten hashes fail the gate.

`tests/windows/WifiP2pDiscoveryValuesParcelChecks.ps1` verifies that gate's
acceptance/rejection logic using constructed log lines. Passing those checks on
Linux PowerShell is not actual Windows ART/Parcel execution. The original
Track96 content remains byte-for-byte intact around the added block.
