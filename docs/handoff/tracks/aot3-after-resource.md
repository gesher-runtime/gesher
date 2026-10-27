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

## Runtime harness source added (still unexecuted)

73 now retains original before PCL/DLC and adds after-resource PCL/DLC cases,
independently checks the actual runtime resource array, ~PCL context marker,
APK-vs-library duplicate resolution, original hot method/image and same-path
stale rejection/refresh. Original15 graph controls are repeated, not relabeled
as new controls. A new shutdown wrapper retains only device PID+birth-matched
kernel process handles, requires actual processes gone BEFORE resource swap,
and records real exit codes. Failed graceful shutdown stays a failure even
if finite owned cleanup later succeeds. The outermost finally restores exact
resource bytes/SHA256 after existing policy/jar cleanup, including exceptions.
PowerShell parsing and diff-check only passed; all execution is CI-only.

Remaining: coherent current host/Java+boot-image component workflow and strict
producer/package/consumer inventory, source peer/audit, actual scoped runtime
and then changed-union full/package/e2e qualification. Current Web3fc component
runs separately and remains the primary shared gate; this source is not merged
or dispatched. StaticSDK/secondary dex/factory remain explicitly open.

Read-only peer of4511 found two shutdown cleanup exception paths: a process
whose Handle/StartTime threw before transfer could leak its wrapper, and one
cleanup/Dispose exception could skip later retained owners. Source now uses
per-untransferred-object finally plus independent cleanup/Dispose attempts
for every retained owner, aggregates errors afterward, and records the
original failed shutdown before cleanup. Actual CI remains entirely pending.
