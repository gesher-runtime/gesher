# SDK platform artifact comparison: 36 to 37.0

Offline SDK platform artifact changes; no runtime compatibility conclusion.

This report measures artifact changes. It does not complete A07 or change Gesher's API level.

## Separately pinned profiles

| Field | Before | After |
| --- | --- | --- |
| Package | platforms;android-36 | platforms;android-37.0 |
| API (raw) | 36 | 37.0 |
| Extension | 17 | 22 |
| SDK package revision | 2 | 2 |
| PreviewSdkInt (absent stays null) | absent (null) | 0 |
| Repository channel | stable | stable |

The complete profiles, observed properties, input hashes and generator hashes are in `summary.json`. The archive pins and official repository evidence are in `manifest.json`.
The project's `aosp.lock` tag is a separate source profile; neither SDK archive is claimed to equal that tag's module revisions.

## Measured changes

| Dimension | Before | After | Added | Removed | Changed | Unchanged |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Encoded class headers | 6,227 | 6,440 | 217 | 4 | 5 | 6,218 |
| Public/protected declarations | 90,630 | 93,798 | 3,206 | 38 | 98 | 90,494 |
| Preprocessed AIDL declarations | 1,538 | 1,709 | 173 | 2 | 0 | 1,536 |
| final public ids | 2,979 | 3,037 | 58 | 0 | 0 | 2,979 |
| active staging names | 48 | 43 | 37 | 42 | 6 | 0 |
| staging slots | 53 | 48 | 48 | 53 | 0 | 0 |
| finalized staging alias slots | 297 | 376 | 79 | 0 | 0 | 297 |
| groups | 39 | 45 | 24 | 18 | 0 | 21 |
| files | 11,158 | 11,199 | 97 | 56 | 265 | 10,837 |

Changed means differing encoded headers, raw member access flags, annotation lists, ID/slot/group records, or file bytes in the respective row. Added/removed declarations are membership in the public/protected declared set, including visibility transitions.
Resource views overlap: active names, slots, alias slots and file hashes are separate descriptions, not additive API counts.

## Lookup of every before declaration in the after SDK

| Outcome | Count |
| --- | ---: |
| declaring_class_absent | 32 |
| descriptor_found | 90,597 |
| descriptor_not_found | 1 |

Found descriptor locations: 90,592 declared, 5 inherited.
Removed declarations carry lookup candidates in `delta.jsonl`; inherited descriptor matches remain visible. A removed declaration is not automatically a binary incompatibility. This lookup uses SDK stubs and does not test runtime linkage.

## Interpretation

- Encoded classes include all classfile visibilities. Public/protected declarations are counted once on their encoded declaring class; inherited members are not expanded for every subclass.
- A declaration removed from one class can resolve in a superclass/interface. Removed rows include conservative lookup in the after SDK, with unresolved and ambiguous outcomes retained.
- Descriptor presence and flag differences do not establish binary or source compatibility. Classfile flags are raw JVM flags, not reconstructed InnerClasses source modifiers.
- All measured before declarations form the target-lookup denominator, independently of whether a changed row is emitted.
- Final public XML entries include declarations marked hidden/system in comments. Comments and resource annotations are not interpreted as a public SDK API filter.
- Active staging names exclude removed_ placeholders; slot inventories include every allocated placeholder. Finalized staging alias slots retain their old IDs and a final ID matched by type/name when present.
- Resource file changes compare complete bytes under data/res; hash changes alone are not semantic changes. Archives pin other files but those files are not interpreted.
- No aggregate coverage percentage combines Java, preprocessed AIDL, resources, NDK, CompatChanges or behavior. Unmeasured dimensions have no denominator.

## Explicitly unmeasured

- **java constant values:** ConstantValue attributes are not decoded.
- **java generics annotations exceptions:** Signature, annotation, annotation-default and Exceptions attributes are not decoded; data/annotations.zip is not compared.
- **java source access and full linkage:** InnerClasses source access, generic typing, protected receiver rules, class loaders, optional SDK jars and complete JVM linkage are not evaluated.
- **binder transactions and source aidl:** framework.aidl is a preprocessed declaration list; source methods, transaction IDs, parcel layouts and Binder behavior are not inputs.
- **compiled and runtime resources:** XML IDs and file hashes do not establish compiled resource tables, overlays, device configuration or runtime provisioning.
- **ndk exports:** No separately pinned NDK sysroot/export artifacts are inputs.
- **source compat changes:** No separately pinned source CompatChanges inventory is an input.
- **cts cdd and runtime behavior:** No CTS/CDD qualification, runtime API37 execution, services, permissions or behavior is measured.
- **aosp module revision alignment:** An SDK API/extension/revision does not identify the project's AOSP tag or individual module commits.

See [the tool guide](../../sdk-platform-diff.md) for acquisition, commands, categories, evidence boundaries and follow-up work.
