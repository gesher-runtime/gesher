# Phase 6: native x86-64 libraries. Dot-sourced by run-tests.ps1, so Check,
# NoDrive, $Dir, $root, $bcp and $failed are in scope.

# Can Windows user code set the %fs base (WRFSBASE), for bionic's TLS and the
# stack-protector cookie? This is the open question of ADR 0014. It is reported
# as INFO, not a pass/fail gate: a .so that does not touch %fs (like the step-1
# trivial one) loads and runs regardless, and the next session resolves %fs
# before a libc++ / stack-protected .so needs it. The probe is crash-safe and
# prints its own verdict.
$fsOut = & "$Dir/fsbase_probe.exe" 2>&1 | Out-String
if ($fsOut -match 'all passed') {
  Write-Host 'INFO  fsbase: %fs IS usable via WRFSBASE on this runner'
} else {
  Write-Host 'INFO  fsbase: %fs NOT usable via WRFSBASE here (ADR 0014; loader must patch %fs)'
}
($fsOut -split "`n" | Where-Object { $_ -match 'ok|FAIL|verdict|#UD' } | Select-Object -First 12) |
  ForEach-Object { Write-Host "      $_" }

# The ELF loader maps a self-contained x86-64 Android .so (built by the NDK in
# native-libs.yml), applies its relocations, runs its constructors and calls
# its exports. This is the step-1 gate.
Check 'ELF loader: load a trivial .so and call it' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/trivial.so")
} @('elf_load_test all passed')
