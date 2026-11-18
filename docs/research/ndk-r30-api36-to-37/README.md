# NDK r30 API36 to API37 platform-stub comparison

Static public platform-stub ELF export metadata; no runtime implementation or complete C/C++ ABI conclusion.

This is a bounded A07 evidence increment for issue #56. SDK artifact profiles, NDK/compiler identity and the project's AOSP source tag remain separate.

## Measured profiles

| ABI | Metadata default | API36 status / exports | API37 status / exports | Export additions | Removals | Changed metadata |
| --- | --- | --- | --- | ---: | ---: | ---: |
| arm64-v8a | True | available / 4,720 | available / 4,774 | 54 | 0 | 0 |
| armeabi-v7a | True | available / 4,808 | available / 4,862 | 54 | 0 | 0 |
| riscv64 | False | below_abi_minimum / null | available / 4,774 | not compared | not compared | not compared |
| x86 | True | available / 4,781 | available / 4,835 | 54 | 0 | 0 |
| x86_64 | True | available / 4,720 | available / 4,774 | 54 | 0 | 0 |

Counts describe defined, externally visible dynamic-symbol identities in the selected public platform stubs. Version aliases remain distinct. The full report also records alias-row counts, every selected library/hash, exclusions, version definitions/requirements, binding, visibility and object/TLS sizes.
An unavailable before baseline is null, not zero. The after profile's observations are not called additions. No library or target code was executed.

## Interpretation

- Every ABI declared by this package's abis.json is considered; default=false and minimum OS versions are retained. Packaged ABI presence is not device/runtime qualification.
- Only complete, supported API slices are paired. An unavailable or incomplete baseline has a null denominator and no symbol delta; an API37-only profile is not counted as additions from zero.
- The measured library set comes from system_libs.json and the effective API. Every file in a selected slice is hashed, including excluded linker scripts, CRT objects and archives.
- Defined exports are named dynamic definitions with GLOBAL/WEAK/GNU_UNIQUE binding and DEFAULT/PROTECTED visibility. Undefined, local, hidden/internal, null and absolute version-label records have separate categories.
- Aliases retain symbol name, resolved version, version role and provider. Numeric version indices are kept in library metadata but not used as cross-file identities.
- The GNU version hidden bit controls default-version selection and is distinct from ELF visibility. Nondefault versions remain defined exports; unknown forms fail explicitly.
- Per-symbol object_size is raw st_size for OBJECT/TLS only. Neither that size nor symbol presence specifies a C/C++ type, value, call signature or working implementation.
- Version names, including deprecated or private-looking names, are preserved. No leading-underscore or private-name heuristic silently removes symbols; source/header publicness is not independently measured.
- Comparison before/after/changed counts are identity groups. before_records/after_records and library summaries also retain actual alias-record counts; duplicates are never overwritten.
- Library byte changes, version records and export changes overlap and must not be added together as independent APIs or defects.

## Explicitly unmeasured

- **runtime implementation:** Stub exports do not implement functions or prove runtime availability, correctness, permissions, services or hardware support.
- **header and language abi:** Headers, types, prototypes, layouts, inline functions, macros, constants, C++ mangling semantics and full C/C++ ABI compatibility are not parsed or qualified.
- **data values and function bodies:** Object/TLS sizes are raw ELF st_size observations. Data values, pointers, function addresses, instructions and function sizes are not compared.
- **private source api classification:** ELF has no Android-public API bit. The platform-stub library set comes from system_libs.json; no source/header API-visibility filter or name-prefix heuristic is applied.
- **loader and linker execution:** Relocations, loader search order, symbol-version matching at runtime, linker scripts, CRT behavior, static archives and C++ runtime providers are not executed or resolved.
- **source revision alignment:** NDK release/compiler identity, API slices, SDK artifact profiles and Gesher's AOSP source tag are separate evidence dimensions; source/module equivalence is not inferred.
- **other android contracts:** Source AIDL, CompatChanges, CTS/CDD, Java behavior and API37 runtime support are not measured by this NDK comparison.

See [the tool guide](../../ndk-platform-diff.md) for acquisition, profile resolution, symbol identities, replay and validation limits.
