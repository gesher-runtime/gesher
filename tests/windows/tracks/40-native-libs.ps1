# Phase 6: native x86-64 libraries. Dot-sourced by run-tests.ps1, so Check,
# NoDrive, $Dir, $root, $bcp and $failed are in scope.

# Can Windows user code set the %fs base (WRFSBASE), for bionic's TLS and the
# stack-protector cookie? Decides docs/decisions/0014-native-libs-fs-base.md.
Check 'fsbase: %fs usable for bionic TLS' { & "$Dir/fsbase_probe.exe" } @(
  'fsbase probe: all passed')

# The ELF loader maps a self-contained x86-64 Android .so (built by the NDK in
# native-libs.yml), applies its relocations, runs its constructors and calls
# its exports.
Check 'ELF loader: load a trivial .so and call it' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/trivial.so")
} @('elf_load_test all passed')
