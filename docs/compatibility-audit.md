# Central compatibility evidence audit

For the current-source crash-contract repairs and their validation limits, see
the [November 8 Android compatibility audit](research/android-crash-contracts-2026-11-08.md).
The older inventories below retain their original snapshots; their counts do
not describe all current failures or establish universal APK compatibility.

`tools/compatibility_audit.py` aggregates technical diagnostics from existing
Gesher runtime logs without opening apps, connecting to the network or changing
the logs. It writes deduplicated JSON and Markdown under `out/compat-audit`:

```powershell
python tools/compatibility_audit.py "$env:LOCALAPPDATA/Gesher/logs" --out-dir out/compat-audit
python -m unittest discover -s tools/tests -p test_compatibility_audit.py
```

Only top-level `.log` files are read. `.log.logcat` mirrors are excluded to
avoid counting a second logging channel as another app. Multiple launches and
repeated exception/cause lines remain separate **observations**, grouped under
the same stable gap identifier. App counts are unique package-shaped log
filenames, not process counts or confirmed app failures. Shared server logs
have no inferred package; unknown filenames are replaced with numbered aliases.

## What the report contains

The parser recognizes these diagnostic formats and retains only their technical
identifier, bounded file/line evidence, observation kinds and optional `.so` name:

- Missing Java class descriptors, `NoClassDefFoundError` and
  `ClassNotFoundException`; slash descriptors normalize to dotted class names.
- Missing JNI implementations reported by `UnsatisfiedLinkError`, with a
  validated Java-style return type, class, method and argument types.
- Native imports bound to an unresolved stub, **separately** from a call to an
  unresolved function or access to unresolved data. Import presence alone does
  not establish that a function was called. A failed invocation is stronger
  evidence, but does not establish whether an app caught the failure or crashed.
- Stand-in `IFoo.method not implemented` diagnostics and unresolved
  `android.settings.*` actions in activity-resolution failures.

No raw lines, intent extras, URLs, arbitrary exception messages, input text,
phone numbers, credentials or environment values are emitted. The report does
not contain the input directory's absolute path. Package/class/symbol names are
grammar checked, common token formats are rejected, and unrecognized text is
discarded. This is deliberately a narrow parser: do not extend it by copying raw
lines or generic exception payloads into evidence.

Every gap is classified as **historical runtime log evidence**, with
`current_source_status: not_assessed`. Old logs can describe code already fixed
in the working tree, and a running process may still use older installed bytes.
The tool hashes its own source and each scanned log prefix; those hashes do not
identify the runtime executable that produced the log. It never clears an old
finding merely because a current source file declares the same symbol.

## Bounds and limitations

Defaults: 128 files, 32 MiB per file, 128 MiB total, 500,000 lines, 64 KiB per
line, 2,048 gap identifiers, 100,000 aggregated observations and 12 evidence
samples per gap. CLI switches can lower or explicitly change these bounds.
Oversized lines are skipped, truncated diagnostics are ignored, and truncation,
read errors, omitted observations and evidence caps are recorded. Input is
UTF-8 (an optional BOM is accepted); UTF-16 files are explicitly reported as
unsupported. Symlinks/reparse files are skipped and subdirectories are not read.
Reports must be outside the input directory. When the file cap is reached, the
selected set is bounded by directory enumeration rather than a complete listing.

This is not an exhaustive API inventory. Reflection without a named diagnostic,
dynamic code, methods that return incorrect values, silent hangs, crashes without
a recognized message, missing features hidden behind runtime guards, hardware
behavior and never-exercised paths remain outside its evidence. The VLC abort
that required a debugger to name `tsearch` illustrates why an import-only log
must not be upgraded to a proven causal finding by this tool.

## Complementary static coverage

The repository already has two useful building blocks:

- `docs/handoff/native-api-coverage.json` records a historical baseline from
  CI run 40296960603/source `d380a3e0190f5b418cfa3ee042691db601092e25`.
  It contains 56 runner/ABI/library contexts and 1,157 missing-symbol rows.
  In the inspected file, 220 rows carry subsequent `verification` metadata;
  their exact scope/status must be read individually. These are export-inventory
  rows, not 1,157 current semantic defects or a complete API-coverage denominator.
- `tests/native2/symbols.py` uses `llvm-readelf` to collect undefined dynamic ELF
  symbols, subtracts definitions supplied by the other ELF files in a supplied
  set, and preserves GLOBAL versus WEAK imports. Its `--api` mode lists exports
  from supplied NDK API stub libraries. `__libc_init` is marked LAUNCHER only in
  an executable with PT_INTERP. These distinctions must survive a broader scan.

A broader central audit can pin an actual runtime/build and API level, inventory
framework/module classes and native registrations, and compare them with APK
DEX references plus each ABI's ELF imports and dependency closure. Optional,
weak and guarded references need separate states; unresolved references alone
do not prove startup failure. Correlating that static inventory with this tool's
runtime observations gives a prioritized work list. Behavioral regression tests,
actual app interaction and hardware checks still establish whether an API works.
The log parser itself does not implement a static API/APK scan. Separate
inventories now provide broader measurements, described below.

## Systematic inventories independent of app selection

`audit_services_jni.py` distinguishes exact upstream registration names from
four reviewed `register_gesher_*` aliases for MemoryFile, MemoryIntArray,
LocalSocketImpl and Scrypt. A local reference clears the corresponding
missing-source claim, but remains `implemented_unverified`: registration
reachability and behavior require separate runtime tests. The original
inventory counts below describe their recorded snapshot.

Start at the [central compatibility map](research/systematic-compatibility-2026-11-05.md).
It links measured reference differences, reviewed source limitations and the
remaining behavioral/hardware requirements. Counts from different inventories
must not be added together or presented as a percentage of Android support.

`tools/java_api_inventory.py` compares class definitions in an SDK JAR with
class definitions in actual DEX boot JARs. It reads the class-file `this_class`
and DEX `class_defs`, not every referenced type, and unions multidex files.
Use repeated `--boot-jar` arguments for core-oj, core-libart, core-icu4j,
framework and gesher-app, `--sdk-jar` for SDK 36 android.jar, and optionally
`--candidate-replace gesher-app.jar=PATH`. It records file hashes, separates
module/annotation families, and correlates named class observations when
`--observations-json` is supplied. A class missing from that boot path can
have a module/APK/shared-library provider elsewhere. The tool does not test
methods, native registration or behavior.

`tools/java_member_inventory.py` adds public/protected fields, constructors and
methods with exact JVM descriptors. It resolves definitions through supplied
boot-order superclass/interface chains; constructors are not inherited.
Members on absent classes are counted separately from descriptors absent on
present classes. Unresolved ancestry and possible access/modifier differences
have separate states. The summary, hashed inputs and complete JSONL are under
`out/compat-audit/java-members`; this remains structural comparison, not method
body, constant-value, generic/source-access or behavioral conformance.

`tools/native_api_inventory.py` compares pinned NDK API 36 exports with actual
`dlopen`/`dlsym` results in disposable x86_64 and ARM64 guest VMs. It requires
the verified reference subset, runtime/boot inputs and an NDK compiler. The
probe looks up names without calling the inventoried APIs. Its report retains
GLOBAL/WEAK/type/version names and library-open outcomes. Gesher's handle and
global resolver can share a table, so a successful lookup is not proof of
per-library namespace correctness. API 36 linker-script inputs are explicitly
skipped rather than silently treated as empty ELF libraries.

`tools/audit_services_jni.py` inventories current source Default/Stub services,
direct registrations and generated/attach JNI fallbacks against a supplied
framework Java-class ZIP and pinned AOSP source. The [detailed report](research/systematic-services-jni-2026-11-05.md)
separates reviewed families from raw registration-name candidates. Alternate
registrations, app-local service injection and later native overrides require
individual review. AIDL inherited-method comparison is by name, not full type
or overload resolution; explicit bodies remain semantically unverified.

`tools/compatibility_contracts.py` indexes all Markdown documentation under
`docs`, including architecture decisions and old handoffs, with content hashes:

```powershell
python tools/compatibility_contracts.py
python -m unittest discover -s tools/tests -p 'test_compatibility_*.py'
python -m unittest discover -s tools/tests -p test_java_api_inventory.py
python -m unittest discover -s tools/tests -p test_java_member_inventory.py
```

It emits section/line navigation for explicit gap/remaining/unsupported/limit
markers. Historical, superseded and negative statements can match: every row
is `unreviewed_navigation_candidate`, not a current defect assertion. The
central map reconciles selected cross-cutting families with newer source and
bounded evidence instead of declaring all old gaps absent or closed.

APK reference/dependency scans, generic/source-access details, constant values,
reflection/dynamic code, callback completion, resource/ownership semantics and
physical hardware need additional layers. Keep those limits visible even when
all parser tests pass.
