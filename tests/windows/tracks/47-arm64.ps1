# Phase 7: arm64 translation (docs/arm64.md, ADR 0047). Dot-sourced by
# run-tests.ps1, so Check, NoDrive, $Dir, $root and $bcp are in scope.
#
# gesher_arm64.dll (the dynarmic-based translator) is built by
# .github/workflows/arm64.yml and downloaded into $Dir with the step-1 smoke
# test. The smoke test loads the DLL the way the real loader does and runs
# hand-assembled AArch64 functions: host->guest through call() and a host_entry
# System V stub, and guest->host through a bound trampoline (the whole AAPCS64
# <-> host-ABI round trip).

# The DLL is loaded by base name; run from $Dir so it is found.
Check 'arm64: translator DLL round-trips (host<->guest, dynarmic)' {
  Push-Location $Dir
  try { & "$Dir/arm64_smoke_test.exe" } finally { Pop-Location }
} @('arm64_smoke_test all passed')

# Step 1 (guest ELF): the loader maps a self-contained arm64-v8a .so (NDK,
# -nostdlib), applies its relocations, runs its constructor through the
# translator and calls its exports; host_entry stubs enter the guest.
Check 'arm64: loader runs a self-contained arm64-v8a .so' {
  Push-Location $Dir
  try { & "$Dir/arm64_elf_test.exe" (NoDrive "$Dir/trivial_arm64.so") } finally { Pop-Location }
} @('arm64_elf_test all passed')

# Step 2 (guest libc): an arm64-v8a .so built against bionic's libc/libm/libdl/
# liblog/pthread; its imports are bound to guest trampolines that call the host
# shim across AAPCS64 <-> System V. Runs the library's own checks.
Check 'arm64: an arm64-v8a libc/pthread .so runs its checks through the shim' {
  Push-Location $Dir
  try { & "$Dir/arm64_elf_test.exe" (NoDrive "$Dir/libc_test_arm64.so") (NoDrive "$Dir") } finally { Pop-Location }
} @('libc_test: all passed', 'arm64_elf_test all passed')
