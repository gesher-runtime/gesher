# After-resource AOT slice: source preparation only

Owned wip/aot3-after-resource / C:/work/gesher-aot3-after-resource, baseline
21a25c2c = qualified AOT450 code plus accepted Libcorea788. The qualified
leaf450/full40557661255 remains immutable; this is no runtime claim.

Existing73/ADR0065 approved next slice now has initial producer source:
optional framework resource overlay selects gesher.shared.entry after the
APK, generated R/Manifest Java must compare byte-identical to default;
actual default/after resource APK SHA256 must differ. Two additional PCL/DLC
APK oracles expect their existing duplicate Marker="application", while
original before APKs expect actual shared Marker="library-before".
All original jars/hot methods and original PCL/DLC fixtures remain.

Not dispatched. Remaining: extend73 to both real resource policies with
independent ~PCL oatdump contexts, selected compiled hot code/app image,
Main/device actual fresh process completion, same-path dependency stale
rejection/refresh, actual full device shutdown BEFORE every resource swap,
and try/finally restoration of exact resource bytes/hashes even on failure.
Producer/package/consumer allowlist and complete artifact presence audit,
scoped CI then qualifying changed-union full/package/e2e remain required.
StaticSDK, secondary dex and app factory stay open.
No local C/C++/Java/C# compilation or executable fixture was run.
