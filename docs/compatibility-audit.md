# Central compatibility evidence audit

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
The full static API/APK scan is a follow-up, not implemented by this tool.
