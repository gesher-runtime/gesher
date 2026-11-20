# Wi-Fi Direct value qualification

This increment provides seven AOSP value source files, eight public SDK36 types
and 172 public/protected declarations. It does not register a Wi-Fi Direct
service, advertise radio support, discover peers, connect, or create a group.
The sources use the pinned Android16-r4/bp4a value profile.

## Host and source checks

The public caller compiles against the official SDK36 JAR only. A separately
identified source-level fixture sets normal fields that Android's framework
would usually populate. It checks copy depth, retained aliases, live collection
views and update behavior. Neither fixture supplies an Android implementation.
The host initialization boundary is the existing Wi-Fi fixture's native shim;
it does not implement Parcel or a Wi-Fi service.

```sh
python3 tools/test_wifi_p2p_values.py \
  --java-home "$JAVA_HOME" --sdk-jar work/sdk-36/android.jar \
  --framework-classes out/framework/classes --core-classes out/coremod \
  --app-classes out/app/gesher-app-classes.jar
```

The host runner freshly compiles all 24 required value source families, verifies
their providers before executing the corpus, and checks every selected SDK36
member's descriptor, access and flags. The 172 declarations are a presence
census, not a behavioral coverage percentage. The runtime corpus also checks
49 constants against the SDK-inlined values.

`--source-ref <verified-pre-increment-commit>` runs the controlled absence check.
The recorded run uses local integration `f0dce23e5418add5af38dd7762a061593b3751f1`;
the report maps its two inputs to their exact public commits and trees. The
runner verifies all seven sources are absent at the selected commit and prevents
cached P2P compilation stubs from filling the gap. The caller must fail
specifically at the missing Config.Builder.
`--output-dir` retains commands, provider hashes and results in a new directory.

`tools/audit_wifi_p2p_sources.py --upstream-dir <pinned-p2p-sources> --output <json>`
compares complete source token streams after the documented adaptations. It
checks the raw source hashes, original license headers, exact release predicate,
READ_ONLY profile constants, and the two hidden DIR sites. It found and helped
remove an orphaned annotation during implementation; the frozen v1 capsule was
not executed. The final source retains the adjacent pairing-method update.

## Genuine Android native Parcel reference

`tools/prepare_wifi_p2p_oracle.py` accepts the same five build inputs as the host
runner plus `--d8-jar`, `--p2p-source-dir`, `--wifi-support-dir`, `--release-dir`
and a new `--output-dir`. The source paths contain the exact official inputs
and hashes listed by the tool. It emits immutable payloads, source/provider
manifests and explicit setup/execution request arrays for the device owner.
It never invokes ADB.

| Profile | Value providers | Native Parcel | Corpus |
| --- | --- | --- | --- |
| Stock API36 | Actual installed Wi-Fi boot module | Actual boot provider | 15 ordinary public cases, including DeviceList readers |
| Pinned r4 | Independently compiled official P2P sources, full Environment, generated bp4a flags; unchanged approved Wi-Fi dependencies | Actual boot provider | 23 ordinary cases; three DeviceList cases write only |
| Candidate | Fresh production P2P sources and the same approved dependency graph | Actual boot provider | The same 23 cases; the same three DeviceList cases write only |

Every profile verifies loader and dependency type identities before the corpus.
The source-owned profiles never replace Parcel, Parcelable, libcore or Dalvik
classes. They retain the original `DeviceList.readParcelable(null)` call.
On Android that call resolves Device through Parcel's boot loader, which would
be a different class from the deliberately source-owned Device. Therefore the
candidate and pinned DeviceList readers are not executed. Their writer bytes
remain useful, explicitly limited evidence. No mixed-loader diagnostic is run.

The Windows package places both framework and gesher-app on its boot class path.
The required Windows fixture first asserts that Parcel and the production value
types share that provider, including the null-loader Device identity. It then
requires the full DeviceList readers as well as all other normal readers.

The native corpus uses ordinary valid values: defaults, null optional fields,
Unicode and embedded NUL strings, IPv4/IPv6 addresses, WFD fields, populated
device/client collections, pairing values, information-element bytes and
vendor PersistableBundles. Malformed/truncated Parcel records and radio/service
operations are outside this corpus. Java argument-validation cases run on the
host. Stock module/profile differences are recorded separately; equality is
required only between matching pinned-r4 and candidate profiles.

## Required Windows gate

`tests/windows/tracks/96-deep-compatibility.ps1` runs the production ART/native
fixture and requires all 23 complete Parcel byte hashes to match the pinned
Android16-r4 reference. Every Windows observation must report a successful
reader/writer round trip; writer-only Android evidence cannot pass this gate.
The provider assertion must precede the corpus, and both fixture completion
records must be present.

`tests/windows/WifiP2pValuesParcelChecks.ps1` exercises the checker's rejection
of incomplete, duplicated, mismatched and writer-only evidence. Its constructed
log lines test gate logic only. They are not native runtime evidence.

Actual final-source Windows execution remains a required merge gate. Canceled
CI workflows have not been rerun or manually dispatched.

See the [implementation report](../../docs/research/wifi-p2p-values-contracts-2026-11-18.md)
and the [native reference receipt](../../docs/research/wifi-p2p-values-android-reference-2026-11-18.json).
