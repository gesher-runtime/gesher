# SDK36 to SDK37 resource attribute metadata

Explicit SDK attribute/styleable source metadata; not compiled resources, public API availability, or runtime compatibility.

## Measured occurrences

| Dimension | Before | After | Added | Removed | Changed | Unchanged |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| resource_roots | 3 | 3 | 0 | 0 | 0 | 3 |
| styleables | 365 | 373 | 9 | 1 | 19 | 345 |
| attributes | 2,802 | 2,842 | 47 | 7 | 7 | 2,788 |
| symbols | 1,524 | 1,571 | 55 | 8 | 0 | 1,516 |

These overlapping source views are not additive API or compatibility counts.

## Complete selected family

| File | Before bytes | After bytes |
| --- | ---: | ---: |
| data/res/values/attrs.xml | 608653 | 613438 |
| data/res/values/attrs_car.xml | 984 | 984 |
| data/res/values/attrs_manifest.xml | 243533 | 260254 |

The full archives and every selected XML file are pinned in manifest.json; profile evidence is retained in summary.json.

## Interpretation

- Declarations are scoped XML occurrences. Repeated names in different files or styleables are not collapsed into one effective resource definition.
- Every element attribute and namespace declaration is retained. Format unions, numeric spellings, localization hints and parent names remain raw; missing values are not synthesized.
- Styleable attribute order and enum/flag order are source order. Neither is claimed to equal generated R.styleable indices, compiled IDs or runtime iteration order.
- Styleable parent labels are not expanded. Name-only attribute occurrences are not resolved to other declarations, packages or runtime providers.
- Comments, processing instructions and formatting are bound by whole-file hashes but not interpreted. Documentation visibility, annotations in comments and global top-level declaration order are outside the semantic views.
- Attributes, their ordered symbol identities and symbol values are overlapping views; row counts are not additive API, defect or implementation counts.
- The selected family is data/res/values/attrs*.xml only. Other resource values, configurations, overlays, compiled tables and permission enforcement are not evaluated.

Each dimension obeys before = removed + changed + unchanged and after = added + changed + unchanged.
A07 and issue #56 remain open. No runtime code or reported capability is changed.

See [the tool guide](../../sdk-resource-attrs-diff.md) for reproduction and evidence boundaries.
