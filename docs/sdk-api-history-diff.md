# SDK API-history metadata comparison

`tools/sdk_api_history_diff.py` compares the `data/api-versions.xml` files in
two separately pinned official Android SDK platform archives. This is a focused
A07 increment for [issue #56](https://github.com/gesher-runtime/gesher/issues/56).
It adds the history metadata dimension left uninterpreted by
[`sdk_platform_diff.py`](sdk-platform-diff.md). It does not duplicate source AIDL,
release configuration or CompatChanges auditing, modify runtime code, change
`SDK_INT`, or complete A07.

The committed [report](research/sdk-api-history-36-to-37/README.md) compares
SDK36 r02 with SDK37.0 r02. Its [manifest](research/sdk-api-history-36-to-37/manifest.json)
retains the official archive URLs, complete hashes and sizes, directly measured
file hashes, selected repository package metadata and original repository
snapshot provenance. The source platform manifest is separately identified by
its canonical JSON SHA-256. No SDK archive or XML payload is redistributed.

## Why history needs its own reader

The selected SDK36 XML has schema version `3`; SDK37 has version `4` and records
minor versions including `36.1` and `37.0`. These are XML schema versions, not
Android versions. The Android17-tagged
[Metalava XML printer](https://android.googlesource.com/platform/tools/metalava/+/refs/tags/android-17.0.0_r1/metalava/src/main/java/com/android/tools/metalava/apilevels/ApiXmlPrinter.kt)
selects schema 4 when its versioned input includes minor versions. Its printing
logic also omits certain attributes when their values match the parent API
element. Consequently, an omitted member `since` is not the assertion that the
member was introduced at version zero or that the history is unavailable.

This report deliberately compares **explicit metadata**. It preserves omitted
attributes and does not expand parent defaults or compute effective availability
intervals. A class-level metadata change therefore appears on its class row; it
is not duplicated across every descendant member whose XML attributes remain
unchanged. Effective-history expansion is a separate follow-up.

The related [API version representation](https://android.googlesource.com/platform/tools/metalava/+/refs/tags/android-17.0.0_r1/metalava/src/main/java/com/android/tools/metalava/apilevels/ApiVersion.kt)
and [extension mapping](https://android.googlesource.com/platform/tools/metalava/+/refs/tags/android-17.0.0_r1/metalava/src/main/java/com/android/tools/metalava/apilevels/ApiToExtensionsMap.kt)
provide the source context for keeping platform versions distinct from extension
versions. These tag references explain the metadata format; they do not establish
which Metalava binary generated either pinned SDK package.

Those three source files were read independently for this implementation. Their
SHA-256 hashes, in the same order, were:

```text
ApiXmlPrinter.kt:       4e1f806049688fb6b4b8a14c05b459c0f1dbf3f6aaa0cd236b4bad69038ac8d6
ApiVersion.kt:          1d225ff20116352fdb0df9971b18028efd85864eacdb80ace1d7dd6bea3507db
ApiToExtensionsMap.kt:  8f57cf6b984a3946ba5664c3b0aff02f2921e06afcc2a48a45acd6d3fee6a850
```

## Identities and evidence

| Dimension | Identity retained | Interpretation |
| --- | --- | --- |
| Class | Encoded class name | A historical class record, including records already marked removed. |
| Method | Declaring class and complete encoded name/descriptor | Return types, overloads and constructors remain distinct. |
| Field | Declaring class and encoded field name | The XML does not supply the classfile field descriptor. |
| Extends / implements | Declaring class, edge kind and encoded target name | Historical hierarchy edges; no current runtime lookup is attempted. |
| Extension catalog | Literal SDK ID | Raw name, short name and reference remain intact. |
| Root metadata | The `api` element's explicit attributes | Schema version and optional minimum version are separate fields. |

Each changed record contains its complete raw attributes. `since`, `deprecated`
and `removed` additionally have decoded components, with `null` for an absent
attribute. `36` keeps an absent minor component; `36.0` keeps an explicit zero;
`36.10` keeps integer minor 10. No version passes through floating-point arithmetic.
Comparisons are of raw metadata: changing `36` to `36.0` is visible even if a
particular downstream consumer considers them equivalent.

The `sdks` attribute retains its raw text and ordered memberships. SDK ID `0`
is represented as a platform version, while other IDs carry integer extension
versions. Missing `sdks` stays `null`; an explicitly empty attribute stays an
empty list. Membership IDs without a catalog declaration remain visible in
`unresolved_extension_catalog_memberships`; the tool does not invent their names.

`module` and `jar` remain separate raw labels, including empty values. Neither
label is converted into an installed provider, filename, class-loader mapping,
module revision or claim of capability. Both real input files have `module`
attributes and no `jar` attributes. The observations retain that absence.

The parser accepts the bounded schema 3/4 structure used here, with optional
root `min`, catalogs, classes and the four known child kinds. Unrecognized
elements, attributes, nested member content, ambiguous identities and duplicate
membership IDs fail explicitly. Inputs larger than 32 MiB per XML file are
outside this reader's current scope. It is not a general XML history importer.

## Reproduce

Use Python 3.10 or newer and its standard library. Fetch the named archives from
their official manifest URLs, keeping the SDK's applicable license. The
comparison itself is offline and reads the ZIP members without unpacking the
SDK trees. For example, from the repository root:

```sh
python3 tools/sdk_api_history_diff.py \
  --manifest docs/research/sdk-api-history-36-to-37/manifest.json \
  --before-archive "$SDK_PLATFORM_INPUTS/platform-36_r02.zip" \
  --after-archive "$SDK_PLATFORM_INPUTS/platform-37.0_r02.zip" \
  --out-dir /tmp/gesher-api-history

python3 -m unittest discover -s tools/tests \
  -p test_sdk_api_history_diff.py -v
```

Both complete archives are checked against their SHA-256, SHA-1 and byte-size
pins and selected repository records. `source.properties` and `api-versions.xml`
are separately checked against their direct pins. Archive hashes are checked
again after reading. The tool does not fetch the mutable repository metadata
URL or treat a new snapshot as equivalent to the historical one.

The output consists of `summary.json`, `delta.jsonl` and `README.md`. It is
deterministic for the same manifest, archives and generator sources. It contains
no current time or local input paths. `delta_rows_sha256` hashes the exact ASCII
JSONL bytes with LF endings; `manifest_sha256` hashes canonical JSON with sorted
keys, ASCII escaping and compact separators. The two generator files are recorded
by their exact source SHA-256.

Run the command twice into separate directories and compare those three files,
or compare them with the committed report. A platform newline conversion cannot
silently change the JSONL hash: each output is explicitly written with LF.

## Validation and remaining work

Nineteen focused tests cover real comparison behavior using isolated miniature
archive fixtures: minor-version precision and absence; parent metadata remaining
unexpanded; extension/platform separation; missing catalog declarations; raw
module versus jar labels; old removed records; overload/edge identities; counted
metadata changes; input pins; package consistency; repeatability; and failing
the CLI before a partial report is published. These are report-tool tests, not
Android/Windows runtime tests. The existing CI compatibility-inventory step
invokes this suite explicitly; the temporary pause on main-push CI is preserved.

Both actual official XML files were also parsed and compared. All reported
dimensions satisfy `before = removed + changed + unchanged` and
`after = added + changed + unchanged`. Repetition, wall time and peak process
memory are measured in the author handoff; they are observations of that host,
not project performance budgets.

The resulting 3,827 changed metadata rows include 217 newly recorded classes,
1,458 methods, 1,382 fields and one extension catalog entry. These are historical
record differences. For example, there are no deleted method records even
though both XML files retain methods with `removed` attributes. The report
does not claim that no Android APIs were removed or that the newly recorded
members are all introduced in API37.

A07 still needs the remaining source/NDK/module/platform comparisons, ownership
and behavior follow-up for the resulting differences, and actual Windows and
Android qualification. This report does not establish providers, permission
enforcement, CTS/CDD compliance, API37 runtime support or app compatibility.
