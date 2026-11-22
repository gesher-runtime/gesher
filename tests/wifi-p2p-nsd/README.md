# Wi-Fi Direct NSD value contracts

These fixtures cover the complete seven-file Android16-r4 service-discovery
value family, including the necessary USD Config and Builder. They exercise
value construction, validation, query data, aliases/equality and genuine
native Parcelable readers. They do not operate a radio, send packets, create
a manager/backend, or claim working service discovery.

The exact SDK denominator, pinned source/profile, build adaptations, helper
provider, and measured limits are documented in
[the contract report](../../docs/research/wifi-p2p-nsd-values-contracts-2026-11-18.md).

## Host contracts and exact absence control

Supply the genuine JDK21, official SDK36r02 jar, retained genuine
framework/core classes, current app classes, and a directory containing all
seven pinned Wi-Fi sources plus the complete pinned `DnsSdTxtRecord.java`.
The source audit checks their exact SHA-256 hashes before compiling.

```sh
python3 tools/test_wifi_p2p_nsd_values.py \
  --java-home "$JDK21" --sdk-jar "$SDK36_JAR" \
  --framework-classes "$FRAMEWORK_CLASSES" --core-classes "$CORE_CLASSES" \
  --app-classes "$APP_CLASSES" --upstream-dir "$NSD_UPSTREAM" \
  --output-dir "$NSD_HOST_OUTPUT"
```

The public fixture compiles against the SDK jar alone and checks all eight
selected class definitions and 40 public/protected declarations. Hidden
CREATORs, constructors, query strings and transaction/session state use a
separate source-level caller. The Linux host loads freshly compiled value
classes and the real existing framework TXT helper. Its small JNI setup
supports host class initialization; it does not implement Parcel natives.
Successful host results therefore report `parcel=false`.

Pass `--source-ref` with the exact pre-change source commit to run the absence
control. It first proves that all seven production source paths are absent,
then prohibits fallback to the cached compile-only family. It expects the
missing value class, not a successful stub response. Use a separate output
directory for this control.

For source preservation alone:

```sh
python3 tools/audit_wifi_p2p_nsd_sources.py \
  --upstream-dir "$NSD_UPSTREAM" --output "$NSD_TOKEN_AUDIT"
```

## Isolated ordinary native comparison

The following command only prepares local artifacts. It requires the complete
pinned Wi-Fi `Environment.java` and flag declaration inputs under the original
source-relative paths in `WIFI_SUPPORT`, plus the genuine pinned release tree.

```sh
python3 tools/prepare_wifi_p2p_nsd_oracle.py \
  --java-home "$JDK21" --sdk-jar "$SDK36_JAR" \
  --framework-classes "$FRAMEWORK_CLASSES" --core-classes "$CORE_CLASSES" \
  --app-classes "$APP_CLASSES" --d8-jar "$D8_JAR" \
  --upstream-dir "$NSD_UPSTREAM" --wifi-support-dir "$WIFI_SUPPORT" \
  --release-dir "$RELEASE_SOURCE" --output-dir "$NSD_NATIVE_OUTPUT"
```

The output includes a source/artifact manifest, the three payloads, one
launcher, and explicit setup/run/cleanup request files for separate device
review and execution. The candidate and pinned reference own their entire NSD
value graph and complete genuine TXT helper. No owned name falls back to boot
or an SDK stub. `Parcel` and `Parcelable` always come from the genuine runtime.
The stock payload defines fixtures only and uses stock boot values.

The launcher checks provider identity before the corpus. It distinguishes
stock API36.0 legacy values from the pinned-r4/bp4a profile, where genuine flags
are freshly generated and USD is enabled for the selected value layout.
The matching candidate/reference profiles require 20 complete original-reader
records; the stock legacy profile has 13. Every record includes both the first
write and reader rewrite hashes. Only USD Info preserves the source's
null-to-empty query-list rewrite. Config identity equality and inherited
subclass-to-base reader results are asserted explicitly.

## Full build and production Windows gate

The ordinary `app/build.sh` and `tests/dex-services/build.sh` compile the same
production files and callers against genuine framework/core inputs. No
compile-only Android definitions are packaged in the test DEX. Verify all
eight SDK types and all 40 declarations in the app DEX, the existing TXT helper
in the framework provider, and no duplicate helper or manager in the app.

Windows Track96 runs `gesher.services.WifiP2pNsdValuesContractTest` on the normal
production boot graph. `AssertWifiP2pNsdValuesParcel.ps1` requires the coherent
provider marker, exact r4/bp4a profile, both passing caller suites, and the
ordered 20-case initial/read/rewrite evidence against the independent Android
reference. The test process exit code is checked as well.

The checker can be verified without claiming native execution:

```powershell
pwsh -NoProfile -File tests/windows/WifiP2pNsdValuesParcelChecks.ps1
```

Its deliberately incomplete, changed and out-of-order log examples validate
gate logic only. A green checker result does not qualify Windows Parcel or the
production classloader. Actual Windows runtime execution remains pending.
