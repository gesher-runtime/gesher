# SDK resource attribute and styleable comparison

`tools/sdk_resource_attrs_diff.py` compares explicit declarations in the complete
`data/res/values/attrs*.xml` family of two pinned official SDK platform archives.
The [SDK36 r02 → SDK37.0 r02 report](research/sdk-resource-attrs-36-to-37/README.md)
is a bounded A07 increment for
[issue #56](https://github.com/gesher-runtime/gesher/issues/56).

The existing [SDK platform comparator](sdk-platform-diff.md) interprets public
and staging resource IDs and hashes the other resource files. This tool adds
scoped attribute declarations, styleable groups and enum/flag metadata. It does
not duplicate Java/API-history, NDK, source AIDL or CompatChanges inventories.
No runtime implementation, reported API level, permission or capability changes.

## Complete selected inputs

Both official archives contain three files in this bounded family:

| File | SDK36 bytes | SDK37 bytes |
| --- | ---: | ---: |
| `data/res/values/attrs.xml` | 608,653 | 613,438 |
| `data/res/values/attrs_manifest.xml` | 243,533 | 260,254 |
| `data/res/values/attrs_car.xml` | 984 | 984 |

The [manifest](research/sdk-resource-attrs-36-to-37/manifest.json) retains each
archive's official URL, byte size, SHA-1 and SHA-256, the selected repository
package record, source.properties, and the size and SHA-256 of every selected
XML file. Archive pins are checked against the official repository records,
and all directly measured files are checked against their own pins. Archives
are hashed again after reading.

Selection is derived from the actual ZIP member list rather than a hard-coded
three-file list. A newly present `values/attrs_new.xml` must have a manifest pin
and be parsed, or the comparison fails. An omitted selected file or an unrelated
file inserted into the manifest cannot silently narrow or widen the scope.
The ZIP is read directly; the tool never extracts or executes its contents.

Only the default `values` directory and filenames beginning `attrs` and ending
`.xml` are selected. Other value files, configuration-qualified directories,
overlays and compiled resource tables are outside this increment. The whole
SDK resource corpus is not claimed to be semantically compared.

## Explicit source metadata

| View | Stable identity | Preserved value |
| --- | --- | --- |
| Resource root | Relative XML filename | Every root element attribute and its namespace declarations. |
| Styleable | File, literal name, occurrence number | All element attributes, namespace declarations and ordered attribute identities. |
| Attribute occurrence | File, owning styleable or top level, owner occurrence, literal attribute name, occurrence number | All raw element attributes, namespace declarations and ordered enum/flag identities. |
| Enum/flag symbol | Owning attribute identity, kind, literal name, occurrence number | All element attributes, including the original `value` spelling, and namespace declarations. |

Names repeated across styleables or files are independent occurrences. Repeated
identities within one scope are also retained with an occurrence number and
reported in `repeated_scoped_identities`; nothing is silently overwritten. This
preservation does not claim that repeated declarations would pass the resource
compiler. The actual selected files have no such repeated scoped identities.

All XML element attributes are preserved, including an unfamiliar attribute
name. Formats such as `reference|integer`, `min`/`max`, localization hints,
styleable parents and numeric enum/flag spellings remain strings. Empty values
remain empty and absent values remain absent. A missing format is not replaced
by another declaration's format. Namespace declarations retain the element on
which they occur, including a nested prefix rebinding; namespace-qualified
element attribute keys use the XML parser's expanded URI form.

The Android17-tagged
[AAPT2 ResourceParser.cpp](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-17.0.0_r1/tools/aapt2/ResourceParser.cpp)
provides useful format context: `ParseAttrImpl` processes formats, integer
boundaries and enum/flag symbols, while `ParseDeclareStyleable` processes
attribute references and weak definitions. That source was read independently;
its 73,568-byte SHA-256 is
`a0cfeeb389deef114fdf88e642a1e0b263f15875f5492b8357f384faefc5d3ea`.
The manifest records this source context without claiming that it identifies
the compiler used to produce either SDK archive.

The report performs none of that compiler resolution. It does not expand parent
styleables, resolve package prefixes, choose an effective definition, normalize
numeric values to 32-bit flags, infer visibility, or generate resources.
Source attribute/symbol order is preserved as metadata; it is not represented
as a generated R.styleable index or a compiled resource ID.

Comments, processing instructions and formatting are covered by the input file
hashes but are not interpreted. In particular, annotations and visibility hints
inside comments do not establish public API classification. Global top-level
declaration order is not one of the semantic views. Empty `eat-comment` markers
are counted; a marker carrying unexpected metadata is rejected rather than
discarded. Unsupported element shapes, non-whitespace text, missing names,
missing symbol values and XML files exceeding 32 MiB fail explicitly.

## Counts and interpretation

| Dimension | Before | After | Added | Removed | Changed | Unchanged |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Resource roots | 3 | 3 | 0 | 0 | 0 | 3 |
| Styleable occurrences | 365 | 373 | 9 | 1 | 19 | 345 |
| Attribute occurrences | 2,802 | 2,842 | 47 | 7 | 7 | 2,788 |
| Enum/flag occurrences | 1,524 | 1,571 | 55 | 8 | 0 | 1,516 |

The output contains 153 delta rows. All seven changed common attribute records
change their ordered symbol identities; their raw element attributes remain
identical. A newly recorded styleable or enum is an input for follow-up source
and behavioral work, not evidence that Gesher implements it.

Each dimension satisfies `before = removed + changed + unchanged` and
`after = added + changed + unchanged`. The views overlap: adding an enum can
change its owning attribute's symbol list as well as add a symbol row. Counts
must not be summed into distinct APIs, defects, implementation coverage or a
compatibility percentage. No A07 or issue #56 checkbox is closed by this report.

## Reproduce and validate

Use Python 3.10 or newer with the standard library. Obtain the two exact official
archives named by the manifest, keeping the SDK's applicable license. The
comparison itself is offline:

```sh
python3 tools/sdk_resource_attrs_diff.py \
  --manifest docs/research/sdk-resource-attrs-36-to-37/manifest.json \
  --before-archive "$SDK_PLATFORM_INPUTS/platform-36_r02.zip" \
  --after-archive "$SDK_PLATFORM_INPUTS/platform-37.0_r02.zip" \
  --out-dir /tmp/gesher-sdk-resource-attrs

python3 -m unittest discover -s tools/tests \
  -p test_sdk_resource_attrs_diff.py -v
```

The output is `summary.json`, `delta.jsonl` and `README.md`. Run the comparison
twice into separate directories and compare those files with each other and
the committed report. Their bytes are deterministic for the same manifest,
inputs and generator sources. No local input path or current time is embedded.
JSONL uses canonical ASCII JSON with sorted keys and compact separators;
`delta_rows_sha256` hashes the exact bytes including LF endings. All outputs
explicitly use LF, including on Windows. Manifest hashing uses canonical JSON.

Twenty maintained tests exercise scoped names, duplicate preservation, raw
metadata and omissions, nested namespaces, source order, complete-family
selection, input pins, count identities, repeatability and CLI failure behavior
using isolated miniature archives. The normal CI compatibility-inventory step
invokes them explicitly while preserving the main-push CI pause. Actual official
SDK comparisons were additionally repeated with identical outputs. These are
inventory-tool checks, not Android or Windows runtime qualification.
