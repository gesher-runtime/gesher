# android-16.0.0_r4 to android-17.0.0_r1 selected source comparison

A bounded A07 source evidence increment: release reference profiles, core/java compatibility annotations and complete source-AIDL file bytes.

## Measured source profiles

| Profile | Selected platform SDK | Build ID | Java files parsed / total | ChangeId field occurrences | Unique resolved IDs | Source AIDL files |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| android-16.0.0_r4 | 36.1 | BP4A.251205.006 | 4,358 / 4,358 | 137 | 137 | 1,532 |
| android-17.0.0_r1 | 37.0 | CP2A.260605.016 | 4,638 / 4,638 | 153 | 152 | 1,706 |

Counts are source occurrences and distinct resolved IDs, not compiled configuration membership or working runtime features.

A parser error, unresolved annotation identity or unsupported value remains explicit. Observed counts are not a complete denominator when the corresponding JSON completeness field is null/false.

## Changed observations

| Category | Status | Rows |
| --- | --- | ---: |
| aidl_source_file | added | 195 |
| aidl_source_file | bytes_changed | 95 |
| aidl_source_file | removed | 21 |
| compat_source_field | annotation_syntax_or_order_changed | 1 |
| compat_source_field | source_occurrence_added | 17 |
| compat_source_field | source_occurrence_removed | 1 |
| selected_repository_reference | commit_changed | 117 |

The JSONL categories overlap; they are not additive API or defect counts. Duplicate declaring fields and IDs remain visible in profiles.json and compat-inventory.jsonl.

The selected source configs are separate from the SDK36/SDK37 platform archives, NDK30 API slices, July API36.0 oracle and current CTS release. Earlier artifact properties are not rewritten.

## Explicit limits

- **source selection:** core/java only; other framework directories/modules, build-selected membership and generated compat_config are unmeasured.
- **java semantics:** AST parsing and bounded lexical constant resolution are not full compiler attribution; inherited/external name resolution and unsupported expressions remain outside the measured rules.
- **aidl semantics:** Only complete source AIDL file bytes are compared. Imports, transactions, parcel layouts, Stable AIDL APIs and generated Binder ABI are unmeasured.
- **runtime:** Per-app compatibility enable state, overrides, runtime providers, permissions, capabilities, SDK_INT behavior and device execution are unmeasured.
- **module equivalence:** Repository/tag/manifest reference matching is not APEX, built module, runtime image or SDK/NDK artifact equivalence.
- **product build:** Selected platform version fields are traced in build/release only; vendor/product overlays and aconfig flag selection are not evaluated.
- **qualification:** CTS/CDD links and release names are reference metadata only. No CTS/CDD or API37 runtime qualification was performed.

See [the tool guide](../../source-release-diff.md) for pinned inputs, exact selection, parser rules, replay and validation.
