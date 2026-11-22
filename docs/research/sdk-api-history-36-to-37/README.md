# SDK36 to SDK37 API-history metadata

Explicit SDK API-history metadata; not current declarations, installed providers, or runtime compatibility.

## Measured records

| Dimension | Before | After | Added | Removed | Changed | Unchanged |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| root_metadata | 1 | 1 | 0 | 0 | 1 | 0 |
| extension_catalog | 7 | 8 | 1 | 0 | 0 | 7 |
| class | 6,735 | 6,952 | 217 | 0 | 346 | 6,389 |
| method | 50,132 | 51,590 | 1,458 | 0 | 60 | 50,072 |
| field | 32,191 | 33,573 | 1,382 | 0 | 71 | 32,120 |
| extends | 6,777 | 6,994 | 217 | 0 | 0 | 6,777 |
| implements | 2,533 | 2,607 | 74 | 0 | 0 | 2,533 |

These are historical XML identities and explicit metadata changes, not live API counts.

## Input profiles

| Field | Before | After |
| --- | --- | --- |
| Package | platforms;android-36 | platforms;android-37.0 |
| API property (raw) | 36 | 37.0 |
| XML schema | 3 | 4 |
| XML bytes | 5674817 | 5911928 |
| XML SHA-256 | 923f7e6367759e127679b6b5a4156ee801100e3cc07dc1d99cb811e791e45435 | 9fd47aaab29946a5f5d179797365b2d04e38132d2e7e942f5ea8504e41e6c8e5 |

Both XML files and source.properties are read from fully pinned official platform archives.
The input manifest retains archive hashes, selected repository package records, and metadata provenance.

## Interpretation

- A historical record can describe an API removed before either selected SDK; record additions are not proof of API introduction in the later release.
- An omitted since/deprecated/removed/sdks attribute is retained as absent. Parent metadata is not expanded; absent does not mean zero, unsupported, or never deprecated.
- Module and jar labels are verbatim metadata. They do not identify a Gesher class loader, installed Mainline module, provider revision, or runtime capability.
- SDK extension catalog entries and sdks membership versions are metadata, not observations of SdkExtensions.getExtensionVersion on a device.
- Method identity includes the complete XML name/descriptor. Fields and inheritance edges retain their encoded identities; JVM access, linkage and behavior are not tested.
- The selected SDK packages, their XML schema versions, platform major/minor versions, extension levels and the project's AOSP source tag are distinct profiles.

Each dimension obeys before = removed + changed + unchanged and after = added + changed + unchanged.
No compatibility percentage is calculated. Issue #56 and A07 remain open.

See [the tool guide](../../sdk-api-history-diff.md) for reproduction, metadata absence and version handling.
