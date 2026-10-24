# Phase 7: arm64 translation (docs/arm64.md, ADR 0047). Dot-sourced by
# run-tests.ps1, so Check, NoDrive, $Dir, $root and $bcp are in scope.
#
# gesher_arm64.dll (the dynarmic-based translator) is built by
# .github/workflows/arm64.yml and downloaded into $Dir with the step-1 smoke
# test. The smoke test loads the DLL the way the real loader does and runs
# hand-assembled AArch64 functions: host->guest through call() and a host_entry
# System V stub, and guest->host through a bound trampoline (the whole AAPCS64
# <-> host-ABI round trip).

# The DLL is loaded by base name; run the test from $Dir so it is found.
Check 'arm64: translator DLL round-trips (host<->guest, dynarmic)' {
  Push-Location $Dir
  try { & "$Dir/arm64_smoke_test.exe" } finally { Pop-Location }
} @('arm64_smoke_test all passed')
