# Automatic tests for the A07 report tools

The normal `ci` workflow runs `api37-report-tests` on Ubuntu 24.04, Windows 2022
and Windows 2025. The job is a required dependency of `ci-required`. It checks
the offline SDK, NDK and selected-source report tools; it does not execute the
Android runtime or establish API37 compatibility.

The job installs Python 3.12 and Temurin JDK21.0.13. Git is the independent source
tree/archive oracle. The Linux profile also requires ordinary x86 ELF32/ELF64
C/C++ linking and GNU `readelf`. The selected ELF fixtures are compiled and read
as data; their code is never loaded or executed.

## Selected evidence

| Test module | Linux | Windows |
| --- | --- | --- |
| `test_sdk_platform_diff.py` | All 7 tests | All 7 tests |
| `test_ndk_platform_diff.py` | All 6 tests | All 6 tests |
| `test_elf_symbol_inventory.py` | All 5 tests | 1 static ELF table test |
| `test_source_compat_inventory.py` | All 10 tests | All 10 tests |
| `test_source_release_diff.py` | All 6 tests | All 6 tests |
| Total selected report tests | 34 | 30 |

The Windows selection explicitly excludes these four Linux ELF toolchain tests:

- `test_compiled_elf32_and_elf64_symbols_objects_tls_and_undefined_refs`
- `test_version_aliases_parent_definitions_and_absolute_symbols`
- `test_versioned_undefined_reference_retains_provider_and_needed_library`
- `test_gnu_unique_object_is_retained`

They remain required on Linux. A named exclusion is not a passing or skipped
test. Each invocation prints every selected identity, the excluded identities,
the observed host and its final counts. Missing or empty modules, duplicate
identities, unexpected skips, expected failures and failed tests leave the gate
unsuccessful. A profile for a different operating system is rejected before
running tests. Twelve separate gate tests check these selection and failure rules.

The source fixtures use the installed Git to write blobs and index entries with
real regular, executable and symlink modes, then generate an actual Git archive.
The production reader must independently reconstruct the same tree, modes and
link bytes. This preserves the original archive semantics on Windows without
requiring filesystem symlink privileges or treating its executable bit as a Unix
mode. Synthetic fixture inputs are isolated from ordinary application behavior.

## Reproduce locally

With the prerequisites available on `PATH`, run from the repository root:

```sh
python -m unittest discover -s tools/ci -p test_run_api37_report_tests.py -v
python tools/ci/run_api37_report_tests.py --profile linux
```

On Windows, use `--profile windows`. An explicit JDK executable can be selected
with `GESHER_SOURCE_DIFF_JAVA`. Missing JDK21, Git or Linux ELF tools do not turn
skipped tests into successful CI evidence. Windows CI disables Python's optional
UTF-8 mode so normal host file defaults are exercised; each report writer still
has to choose its documented UTF-8/LF output itself.

These normal fixtures include complete report replays and the NDK regression
distinguishing unique export identities from duplicate symbol records. They do
not download or replay the pinned real SDK, NDK and source packages in CI. The
committed reports retain their separate source/artifact hashes and independent
qualification. Local Linux success does not establish Windows execution; that
requires an actual completed Windows job with its exact source identity.

See [SDK artifact comparison](sdk-platform-diff.md),
[NDK platform exports](ndk-platform-diff.md) and
[selected source comparison](source-release-diff.md) for their evidence and limits.
