# Separately pinned source releases and selected compatibility declarations

`tools/source_release_diff.py` compares selected source evidence at
`android-16.0.0_r4` and `android-17.0.0_r1`. The
[generated report](research/source-16r4-to-17r1/README.md) is another bounded A07
increment for [issue #56](https://github.com/gesher-runtime/gesher/issues/56),
following the [SDK artifact comparison](sdk-platform-diff.md) and
[NDK platform-export comparison](ndk-platform-diff.md).

It measures release reference mappings, the selected platform release
configuration, Java compatibility-change declarations in **frameworks/base/core/java**,
and complete AIDL source-file bytes in that same directory. It does not complete
A07 or establish API37 runtime support. The runtime compatibility generator,
SDK level, permissions, capabilities and application workflows are unchanged.

## Exact profiles and denominators

| Observation | Before | After |
| --- | --- | --- |
| Source release tag | `android-16.0.0_r4` | `android-17.0.0_r1` |
| Source build ID | `BP4A.251205.006` | `CP2A.260605.016` |
| Platform `aosp_current` alias | `bp4a` | `cp2a` |
| Selected major SDK flag | `36` | `37` |
| Selected full SDK flag | **`36.1`** | **`37.0`** |
| Selected codename flag | `REL` | `REL` |
| Preview SDK build-flag declaration | Absent in this release-config corpus | Present; selected value `0` |
| Project selectors / unique repositories | 123 / 121 | 123 / 121 |
| Release manifest projects | 1,013 | 1,087 |
| Files in selected core/java tree | 6,400 | 6,896 |
| Java files parsed without errors / total | 4,358 / 4,358 | 4,638 / 4,638 |
| Source AIDL files | 1,532 | 1,706 |
| ChangeId field occurrences | 137 | 153 |
| Distinct qualified declaring fields | 137 | 152 |
| Distinct resolved change IDs | 137 | 152 |

These are different denominators. The release manifest is not a downloaded
source corpus. The project's selectors are not a list of built APEX modules.
The selected core/java tree is not all of frameworks/base, all Android modules
or the build-selected source set.

The [manifest](research/source-16r4-to-17r1/manifest.json) pins exact repository
refs, input URLs, byte sizes, SHA-256 digests, Git blob/tree IDs and parser version.
The frameworks/base commits are
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4` and
`94b4c163b7dfe5ce3607f7bb8456f9573f7de57d`. Their selected core/java tree IDs are
`0edfa9f43f2496a8642aa65843e1208da314f9cd` and
`570612732d2db0069d80a2778ad4991c166530c9`.

The tool reads all sixteen selected source archives, reconstructs every archived
Git blob and directory tree, and requires exact agreement with the pins. It
checks eight additional decoded source/manifest blobs. Executable source-file
modes and symlink contents participate in Git tree identity; no archived file is
executed and no symlink is followed. The complete source-file inventory is
hashed; every AIDL file and every observed compatibility field has explicit
file-level provenance in the report. Source archives and binaries are not
committed.

The pinned release-tag records cover every aosp.lock selector. The two media
checkout aliases refer to the actual frameworks/av and frameworks/native
repositories and are retained as aliases. They are not missing release modules.
The release manifests independently match those repository names and inherited
tag revisions. Of 121 unique repositories, 117 selected commits differ and four
are unchanged. Neither observation establishes built-module equivalence.
The initial public Android17 superproject tag lookup was unavailable; no
substitute superproject profile is inferred.

The offline tool verifies the supplied bytes and their consistency with the
pinned reference mapping. It does not reauthenticate historical tag responses
over the network. A later moved tag or changed upstream snapshot must not
silently replace the exact commits and trees in this manifest.

## Why the source comparison is 36.1 to 37.0

The source build IDs come directly from each selected
`build/make/core/build_id.mk`. The release aliases come from each selected
`build/release/release_config_map.textproto`. The version values come from their
flag declarations and the actual inherited config graph, including local
assignments. They are not inferred from tag spelling, SDK stubs, compiled
providers or a directory name in a source cache.

For bp4a, the chain is bp4a → bp3a → bp2a → bp1a → ap4a → ap3a. The full SDK
flag is assigned `36.0` at bp2a and `36.1` at bp4a. For cp2a, the ordered parents
are `mainline_2026_04` and `cp1a`; that mainline chain itself includes cp1a and
bp4a. The local cp2a assignments set the major/full values to `37`/`37.0`.

The exact Soong resolver sources are pinned alongside the data. For these
selected string-valued version flags, the implementation starts with a
declaration default, inherits **assigned** values in parent order, and then
applies local assignments. An unassigned parent's declaration default does not
erase an earlier parent's assigned value. The report records every ordered
inheritance step, its before/parent values, whether it applies, any conflict,
and all assignment sources. It also retains each evaluated ancestor's trace,
so repeated inheritance through a shared ancestor is visible.

The reader handles the selected schema forms and the relevant MANUAL and
MANUAL_NO_INHERIT workflows. Unrecognized syntax, redaction, aconfig-only
inheritance and unsupported selected-value forms fail explicitly. Product/vendor
overlays, general build flags and aconfig selection are outside this evaluator.
Selected-field assignments in build-variant configs are detected and prevent a
complete platform-SDK resolution. There are no such assignments for the five
measured version fields in the actual inputs.

In Android16's pinned declarations, RELEASE_PLATFORM_PREVIEW_SDK_INT does not
exist. Its report status is `undeclared`, its value is null and its assignment
list is empty. This is an observation about this selected release-config
corpus; it is not a claim that the source release has a nonzero runtime preview
SDK. Android17 declares the flag and selects `0` through actual assignments.

The SDK36r02 and SDK37.0r02 archive properties, NDK30 API slices and compiler,
the July API36.0 runtime oracle, these source tags, and the current CTS release
remain separate profiles. This report does not rewrite earlier raw SDK minor
properties, infer equality from a shared major API, or equate a source tag with
a particular installed module set.

## Java source measurement

`tools/SourceCompatInventory.java` uses the JDK21 compiler's public
[`JavacTask.parse()`](https://docs.oracle.com/en/java/javase/21/docs/api/jdk.compiler/com/sun/source/util/JavacTask.html)
and tree interfaces. It parses **every Java file** in the selected tree. It does
not select declarations by regex or by finding annotation text in comments.
Annotation processing, compiler attribution, code generation and preview syntax
are disabled. The selected parser version is JDK21.0.13.

Every source-file parse has a completeness status and retained diagnostics.
Errors, unsupported preview syntax, erroneous AST nodes or parser exceptions
make that file incomplete. A failed file is not silently removed from the
denominator. Missing parse records or a mismatch with the complete source-file
set fail the comparison. Complete field denominators become null when parsing
or annotation identity is unresolved. The actual 8,996-file pass has no parse
diagnostics or erroneous trees.

For each observed candidate, the inventory retains source path, SHA-256, Git
blob, package, enclosing scopes, field/type, modifier spelling, initializer,
ordered annotation syntax/arguments and source location. Offsets are explicitly
Java UTF-16 code units; line and column values come from the compiler's line map.

### Names, modifiers and constants

Annotation names are resolved only from explicit platform-qualified spelling
or an unambiguous explicit type import, with the resolution basis recorded.
The reader conservatively preserves unresolved names if the file declares a
matching type/type-parameter name. Wildcards, unbound same-package names, static
annotation imports and unsupported relative qualification are not guessed.
This is bounded lexical evidence, not full compiler attribution; inherited or
external name resolution remains unmeasured.

Source-spelled modifiers and Java-effective field modifiers are separate.
For example, an interface field without written modifiers is implicitly public,
static and final under [JLS §9.3](https://docs.oracle.com/javase/specs/jls/se21/html/jls-9.html#jls-9.3).
The report retains the empty syntax-modifier list, the three effective modifiers
and their implicit origin. Local/anonymous declaring classes keep their scope
locations and null canonical field identities. They are not assigned invented
qualified names, dropped, or paired as ordinary named fields. Non-field ChangeId
uses, including parameters, remain separate observations.

The constant reader accepts AST integral literals, parenthesized forms,
supported unary operations, and unambiguous references to constants in the
same pinned Build.java source. It preserves Java integral unary overflow rules.
Matching source-declared names prevent an imported constant from being guessed
through shadowing. Other expressions remain visible with null values and
reasons, even when a full Java compiler could evaluate them. There is no
fallback to zero, current SDK values or constants from another artifact.

All 290 actual change-ID initializers are direct int/long literals. All measured
target-SDK references resolve through the selected Build.java constants or
literal values. There are no unresolved identities, IDs or thresholds in these
particular source-field observations. Other Build.VERSION_CODES_FULL expressions
are retained as source metadata and can remain unevaluated without becoming
claims about the selected build's runtime properties.

### Processor metadata and source alternatives

The exact tools/platform-compat annotation definitions, ChangeIdProcessor,
Change.java, XmlWriter.java and frameworks/base compatibility-config XSD are
pinned. The measured annotations are:

| Annotation | Retained observation |
| --- | --- |
| ChangeId | Field occurrence, declared name and constant ID |
| Disabled | Independent presence flag |
| LoggingOnly | Independent presence flag |
| NoLogging | Independent presence flag |
| EnabledAfter | Original expression and resolved threshold, if supported |
| EnabledSince | Original expression and resolved threshold, if supported |
| Overridable | Independent presence flag |

Annotation order remains in the raw evidence. It does not choose a winning
policy. The diagnostic layer checks the upstream processor's static constant
long requirement and its conflicting combinations: Disabled with target gating,
LoggingOnly with Disabled/target gating, and EnabledAfter with EnabledSince.
Other annotations remain in the raw list. Source comments/descriptions are not
extracted into compiled config metadata, and no per-app effective enable state
is calculated.

The observed processor change between these tags is its supported Java source
version from 17 to 21. Change.java, XmlWriter.java and the config XSD are
unchanged. The schema's unique-ID constraint describes generated config files;
it does not authorize collapsing source alternatives into one row.

Android17 contains two occurrences of
`android.os.MessageQueue#USE_NEW_MESSAGEQUEUE`, both with ID `421623328` and
EnabledAfter threshold 36, in:

- `android/os/CombinedDeliMessageQueue/MessageQueue.java`
- `android/os/CombinedMessageQueue/MessageQueue.java`

Both are retained. The report therefore distinguishes **153 source occurrences**
from **152 qualified field identities and unique IDs**. It does not select which
source enters a particular build. Duplicate groups are explicit arrays, never
last-writer-wins dictionaries.

Across the selected corpus there are 17 added field occurrences, one removed
occurrence and one raw annotation-list change. There are 16 added unique IDs
and one ID absent from the after corpus. No existing paired occurrence changes
its measured compatibility-processor metadata. The raw annotation change adds
TestApi and FlaggedApi to
`ActivityInfo#OVERRIDE_EXCLUDE_CAPTION_INSETS_FROM_APP_BOUNDS`; its measured
Disabled/Overridable compatibility annotations remain the same. The removed
occurrence is
`ActivityInfo#OVERRIDE_CAMERA_COMPAT_ENABLE_FREEFORM_WINDOWING_TREATMENT`.
Absence here is not a claim that a runtime change ID was globally removed.

Occurrence matching uses both source path and qualified declaring field, with
lists retained for duplicates. Source moves can therefore appear as an
occurrence removal/addition; resolved-ID sets provide a separate observation.
The change report distinguishes measured processor metadata from raw annotation
syntax/order. Other Java file-body changes are outside this comparison.

## AIDL source files

All 1,532 before and 1,706 after `.aidl` files are inventoried from the verified
core/java trees. There are **195 added files, 21 removed files and 95 files with
changed bytes**. File identity includes path, Git blob, SHA-256, byte length and
mode. Comments and whitespace participate in these byte hashes.

These are source AIDL files, including the selected tree's non-public sources.
They are distinct from the SDK comparison's public preprocessed
framework.aidl declarations. This tool does not parse AIDL API semantics,
resolve imports, generate Binder code, calculate transaction numbers or parcel
layouts, compare Stable AIDL API/version hashes, or validate permission/runtime
behavior. A changed source file is not labelled an ABI break.

## Acquire and reproduce

Requirements are Python 3.10+ and JDK21.0.13 with the jdk.compiler module.
The tool is offline. To acquire the pinned public source inputs separately,
the following example reads only the input URLs recorded in the manifest and
decodes Gitiles text blobs. Retain the upstream source licenses with the inputs.

```sh
python3 - <<'PY'
import base64, json, pathlib, urllib.request
manifest = json.loads(pathlib.Path(
    'docs/research/source-16r4-to-17r1/manifest.json').read_text())
destination = pathlib.Path('/tmp/gesher-source-release-inputs')
destination.mkdir(parents=True, exist_ok=True)
for profile in manifest['profiles']:
    pins = profile['archives'] + list(profile['blobs'].values()) + [profile['release_manifest']]
    for pin in pins:
        with urllib.request.urlopen(pin['url'], timeout=60) as response:
            data = response.read()
        if pin.get('download_encoding') == 'base64':
            data = base64.b64decode(data)
        (destination / pin['filename']).write_bytes(data)
PY

python3 tools/source_release_diff.py \
  --manifest docs/research/source-16r4-to-17r1/manifest.json \
  --input-dir /tmp/gesher-source-release-inputs \
  --java /path/to/jdk-21.0.13/bin/java \
  --out-dir /tmp/gesher-source-release-report

for name in summary.json profiles.json compat-inventory.jsonl delta.jsonl README.md; do
  cmp "/tmp/gesher-source-release-report/$name" \
      "docs/research/source-16r4-to-17r1/$name"
done
```

The project lock hash must match the pinned selector profile. The parser patch
version must match the manifest. The tool compiles its own Java helper into a
temporary directory and parses temporary copies of the verified Java source
files. It never loads or executes Android source classes, stubs or libraries.

`profiles.json` contains complete selected repository mappings, release-config
graphs/traces, archive verification results, parser completeness/diagnostics,
duplicate groups, Build constants, schema metadata and the full AIDL file
inventory. `compat-inventory.jsonl` retains every observed candidate and its
measured/unresolved state. `delta.jsonl` contains all changed observations;
`summary.json` and README summarize the same data. Input and generator hashes
are retained. Canonical JSON digests use sorted keys, compact separators and
ASCII escapes; JSONL digests include trailing LF newlines. All text reports
explicitly use UTF-8 and LF on every host, including Windows. Outputs contain no
current timestamp or machine-local path.

## Verification and remaining scope

Run the normal tests:

```sh
GESHER_SOURCE_DIFF_JAVA=/path/to/jdk-21.0.13/bin/java \
  python3 -m unittest discover -s tools/tests -p test_source_compat_inventory.py -v
GESHER_SOURCE_DIFF_JAVA=/path/to/jdk-21.0.13/bin/java \
  python3 -m unittest discover -s tools/tests -p test_source_release_diff.py -v
```

The thirteen tests include an independent javac Element/constant oracle for
normal fixtures. It checks interface modifiers, constant values, qualified
annotation identities and arguments through compiler attribution, separately
from the production AST reader. Other fixtures cover comments/strings,
parameters, local/anonymous scopes, shadowing, duplicate source alternatives,
unsupported expressions, valid preview-source diagnostics and upstream policy
conflicts. Fixture application classes are never executed.

The installed Git independently supplies expected tree IDs for ordinary source
archive fixtures, including executable modes, symlinks and directory ordering.
Configuration tests exercise ordered conflicts, declaration defaults, absent
preview metadata, non-inheriting flags and build-variant limitations. An offline
fixture runs the complete tool twice with real Java AST parsing and Git-bound
source inputs and compares all five output files byte for byte.

The actual artifact run reconstructs all sixteen source archive trees, checks
the additional source blobs, parses every selected Java file and retains every
AIDL source file. Actual-output replay and exact-freeze independent review are
separate validation steps; host fixtures are not Android runtime qualification.
No CI rerun/dispatch, ADB/device action or target execution is required.

Source build membership, general Java attribution, AIDL semantics, runtime
compatibility decisions, Android17 module alignment and full behavioral API37
support remain unmeasured. The
[Android17 CDD](https://source.android.com/docs/compatibility/17/android-17-cdd)
and [CTS downloads](https://source.android.com/docs/compatibility/cts/downloads)
are separate references. The observed CTS tag is `android-cts-17.0_r3`; it is
not the selected Android17 r1 source release, and no CTS/CDD qualification is
claimed.
