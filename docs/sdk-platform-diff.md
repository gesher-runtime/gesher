# Separately pinned SDK platform comparisons

`tools/sdk_platform_diff.py` compares two official Android SDK platform archives
offline. The [SDK36 r02 → SDK37.0 r02 report](research/sdk-platform-36-to-37/README.md)
is the first bounded A07 evidence increment for
[issue #56](https://github.com/gesher-runtime/gesher/issues/56).
It provides measured input changes for planning subsequent compatibility work.
It does not complete A07, establish Android 17 runtime support, or change
`SDK_INT`, build selection, application behavior, permissions, or capabilities.

## Inputs and distinct profiles

The [manifest](research/sdk-platform-36-to-37/manifest.json) pins these artifacts:

| Field | Before | After |
| --- | --- | --- |
| Official package | `platforms;android-36` | `platforms;android-37.0` |
| Archive | `platform-36_r02.zip` | `platform-37.0_r02.zip` |
| Archive size | 65,878,410 bytes | 67,281,901 bytes |
| API property, verbatim | `36` | `37.0` |
| Normalized major/minor | 36 / 0 | 37 / 0 |
| Base extension | 17 | 22 |
| SDK package revision | 2 | 2 |
| Official repository channel | `channel-0` / stable | `channel-0` / stable |
| `AndroidVersion.PreviewSdkInt` | absent | `0` |

An omitted minor component is recorded as such even though its normalized
numeric value is zero. Absent codename, preview and beta properties stay `null`;
an observed empty string stays empty. In particular, the missing SDK36 preview
property is not manufactured as `0`. The stable labels come from the selected
packages' channel references and the channel definition in the official
repository metadata; they are separate from the observed preview properties.
The complete `source.properties` mappings are preserved.

The archive SHA-256 pins are:

```text
36 r02:   37607369a28c5b640b3a7998868d45898ebcb777565a0e85f9acf36f29631d2e
37.0 r02: 840b23e827f96e64aea4c89a1194aac3dc5f6bad37edb231c5c795d890330e8d
```

The manifest also preserves the official archive SHA-1 checksums, sizes,
package metadata, and hashes of each directly interpreted file. Its provenance
records the 436,016-byte official `repository2-3.xml` snapshot observed on
2026-11-18, SHA-256
`8db7cc1bd65bf8c9dffc3678a70d6dc98b02781a24d1ef0d31ecf111a55cc2d5`.
The selected package records and channel labels were extracted from that
snapshot and the downloaded archive SHA-1 values matched it. The large binary
archives and full SDK trees are not committed.

The comparison verifies the supplied archives against the manifest's SHA-256,
SHA-1 and byte size, verifies consistency with its selected official archive
records, and verifies hashes and properties of the directly interpreted files.
It does not make a network request to authenticate or refresh the historical
repository snapshot. A reviewer can separately compare the selected records
with the original snapshot or the current official repository metadata. A
different future repository snapshot can legitimately have a different hash;
it must not silently replace this comparison's provenance.

Gesher's [`aosp.lock`](../aosp.lock) pins `android-16.0.0_r4`. That is a separate
source profile. This report records its source context and lock hash, without
assuming the SDK36 archive equals that tag's framework or module revisions.
No Android 17 AOSP module profile has been established here. Google's
[build and tag reference](https://source.android.com/docs/setup/reference/build-numbers)
lists Android 16/API36 and Android 17/API37, while the
[Android 17 setup guide](https://developer.android.com/about/versions/17/setup-sdk)
uses compile SDK37. Those general references do not identify the exact SDK
revision, extension profile, or source commits in these archives; the pinned
package metadata provides that artifact identity.

## Acquire and reproduce

Use Python 3.10 or newer with its standard library. No Android device, SDK
installation, Java runtime, network access during comparison, or Gesher build
is required. Download the two named platform archives from Google's official
repository into an external directory, retaining the SDK's applicable license:

```sh
mkdir -p /tmp/gesher-sdk-platform-inputs
curl --fail --location \
  https://dl.google.com/android/repository/platform-36_r02.zip \
  --output /tmp/gesher-sdk-platform-inputs/platform-36_r02.zip
curl --fail --location \
  https://dl.google.com/android/repository/platform-37.0_r02.zip \
  --output /tmp/gesher-sdk-platform-inputs/platform-37.0_r02.zip
```

From the repository root:

```sh
python3 tools/sdk_platform_diff.py \
  --manifest docs/research/sdk-platform-36-to-37/manifest.json \
  --before-archive /tmp/gesher-sdk-platform-inputs/platform-36_r02.zip \
  --after-archive /tmp/gesher-sdk-platform-inputs/platform-37.0_r02.zip \
  --out-dir /tmp/gesher-sdk-platform-report
```

The command reads the archives without unpacking the complete platform trees.
It temporarily writes each `android.jar` to reuse the existing bounded archive
parser and removes those temporary copies on exit. An archive or directly
measured artifact that differs from its pin fails the comparison. Archives are
hashed again after reading to detect changes during collection. Unfamiliar
AIDL or public-resource forms fail explicitly; this is a deliberately bounded
artifact reader, not a general source parser.

The output is deterministic for the same manifest, archives and generator
files. It includes no current timestamp or local input path. Run the command
again into a second output directory and compare its three files with the
first run. To compare with the committed report:

```sh
cmp /tmp/gesher-sdk-platform-report/summary.json \
  docs/research/sdk-platform-36-to-37/summary.json
cmp /tmp/gesher-sdk-platform-report/delta.jsonl \
  docs/research/sdk-platform-36-to-37/delta.jsonl
cmp /tmp/gesher-sdk-platform-report/README.md \
  docs/research/sdk-platform-36-to-37/README.md
```

The manifest digest uses canonical JSON (sorted keys, ASCII escapes, compact
separators), so insignificant manifest formatting does not change the digest.
`delta_rows_sha256` hashes the exact ASCII JSONL bytes, including each trailing
newline. The resource inventory digest hashes a canonical object mapping every
relative `data/res/` file path to its byte size and SHA-256. These definitions
are independent of archive directory-entry order. A changed generator file
changes `summary.json` even if the measured delta rows remain identical.

## Output and denominators

The report contains:

- `summary.json`: both complete artifact profiles, all category denominators,
  changes, generator hashes, provenance, interpretation and unmeasured areas.
- `delta.jsonl`: every changed record, with its category, before/after evidence
  and deterministic ordering. Unchanged records are counted but not repeated.
- `README.md`: a reviewable overview generated from the same result.

For each ordinary mapping, `before = removed + changed + unchanged` and
`after = added + changed + unchanged`. For Java declarations, `flags_changed`
takes the place of `changed`. The dimensions below have separate denominators;
they are never summed into a compatibility or implementation percentage.

### JVM declarations and hierarchy lookup

`java_member_inventory.py` supplies the unchanged archive/class parser and
conservative hierarchy resolver. `java_api_inventory.py` supplies its existing
bounded archive validation. All encoded classes in `android.jar` are counted,
including classes with nonpublic classfile visibility. Member counts include
public/protected fields, methods and constructors on those encoded classes,
once per declaration. They do not expand each inherited method across all
subclasses, nor claim that every encoded class is source-accessible public API.

Class header rows compare raw classfile access flags, superclass names and
ordered interface lists. Member identity includes declaring class, field or
method kind, name, and exact JVM descriptor, including a method's return type.
Common public/protected declarations compare raw access flags. A visibility
change into or out of that declaration set is an addition or removal; a removed
row's target lookup can still expose the remaining nonpublic declaration.

Every before declaration is also looked up in the after SDK. Constructors are
not inherited; private/static interface methods are excluded from inherited
method candidates. Unknown parents and ambiguous interface candidates remain
uncertain. Removed rows include candidate owners and flags when a descriptor
is still found. The lookup's `possible_flag_differences` checks a narrow subset
of restrictions, such as staticness, reduced visibility, a newly final member
or a newly abstract method. It does not replace the raw flag-change count or
the separate class-header comparison.

In this pair, the 38 removed declarations comprise five inherited descriptor
matches, 32 declarations on four absent fingerprint classes, and the absent
`Context.FINGERPRINT_SERVICE` descriptor. For example,
`AppWidgetHostView.generateLayoutParams` still resolves in `FrameLayout` in
the after SDK. These are observations about the selected stub archives, not
claims that Android 17 runtime classes disappeared or that an APK fails to
link. The 98 raw modifier changes include relaxing `final` and changing
`native` flags; they are not 98 diagnosed application failures.

Constant values, generic signatures, annotations, annotation defaults and
checked-exception metadata are explicitly **unmeasured**. The parser skips
their attributes; hashing an archive containing them does not measure their
semantic changes. `InnerClasses` source access, code bodies, optional SDK jars,
class-loader constraints, protected receiver rules and full binary/source
compatibility are also outside this result.

### Preprocessed framework AIDL

The selected `framework.aidl` files contain 1,538 and 1,709 preprocessed
parcelable declarations, respectively, each with the marker annotation
`@JavaOnlyStableParcelable`. The reader retains declaration kind, qualified
name and ordered marker annotations. Addition/removal is keyed by kind/name;
annotation changes on the same declaration have a separate `changed` result.
This pair has 173 additions, two removals, and no changed annotation lists.

The reader accepts one semicolon-terminated preprocessed `parcelable` or
`interface` declaration per nonempty line, with zero or more marker
annotations. Source interface bodies, annotation arguments and other forms
require a separate extension and currently fail. This public SDK preprocessed
list contains no Binder transaction inventory, source methods, parcel layout
or service implementation denominator.

### Final resource IDs, staging slots and file bytes

`public-final.xml` explicit top-level `<public>` entries are keyed by type/name
and compared by normalized numeric ID. This is the file's declaration set,
including entries described as hidden or system API in comments. Comments,
resource annotations and public Java `R` accessibility are not interpreted.
All 2,979 before final entries keep their IDs in this pair; 58 entries are added.

Staging resources have several intentionally separate views:

- Active staging names exclude `removed_` placeholders and compare each
  type/name's current staging ID.
- Staging slots include every `<public>` allocation in declaration order,
  including repeated `removed_` placeholders that reserve an ID.
- Finalized staging alias slots from `staging-public-group-final` retain their
  historical staging ID and the final ID matched by type/name, or `null` for a
  placeholder or a name with no final entry.
- Group records preserve group kind, type, first ID and slot count, including
  empty groups, so changes in allocation ranges remain visible.

This follows the slot allocation and staged-alias distinction in AOSP's
[ResourceParser](https://android.googlesource.com/platform/frameworks/base/+/9c3ba59c5475d2c2bf103c22e4d1db7dd0ec4dc4/tools/aapt2/ResourceParser.cpp).
The reference explains the XML encoding; it is not asserted to be the source
commit that produced either SDK archive. A staging name removed from one file
may have moved to a finalized alias and final ID. The overlapping resource
views are not independent API additions/removals to add together.

All file entries under `data/res/` are hashed by decompressed contents and
compared by relative path. The pair has 11,158 → 11,199 files: 97 added,
56 removed, 265 byte-changed and 10,837 unchanged. XML whitespace, comments,
configuration variants and image bytes all count as byte changes. Compiled
resource tables, overlays, rendering, device configuration and actual runtime
resource provisioning are unmeasured.

## Tests and remaining A07 work

```sh
python3 -m unittest discover -s tools/tests -p test_sdk_platform_diff.py
python3 -m unittest discover -s tools/tests -p test_java_member_inventory.py
python3 -m unittest discover -s tools/tests -p test_java_api_inventory.py
```

The new fixtures exercise profile separation and absent metadata, inherited
declaration movement and constructors, raw modifier and class-header changes,
unknown/ambiguous hierarchy outcomes, AIDL annotation distinctions, final IDs,
staging promotion, repeated removed slots, complete resource-file counting,
actual classfile/ZIP reading and byte-identical replay from different paths.
Fixtures are isolated test inputs; they do not supply capabilities to apps.

Subsequent A07 increments still need separately acquired, pinned and reviewed
NDK export profiles, source AIDL contracts, source CompatChanges, a future
Android 17 AOSP/module profile, richer Java metadata, and actual behavioral
qualification. No denominator is invented for these missing inputs. CTS/CDD
requirements and runtime behavior have not been tested by this comparison.
The author checks here are local/offline; no CI run, CI rerun, device action or
runtime version switch is part of the workflow.
