# Phase 7: arm64 translation (docs/arm64.md, ADR 0047). Dot-sourced by
# run-tests.ps1, so Check, NoDrive, $Dir, $root and $bcp are in scope.
#
# gesher_arm64.dll (the dynarmic-based translator) is built by
# .github/workflows/arm64.yml and downloaded into $Dir with the step-1 smoke
# test. The smoke test loads the DLL the way the real loader does and runs
# hand-assembled AArch64 functions: host->guest through call() and a host_entry
# System V stub, and guest->host through a bound trampoline (the whole AAPCS64
# <-> host-ABI round trip).

# gesher_arm64.dll is loaded by base name from the executable's directory
# ($Dir). Absolute paths: $Dir may be relative to the starting directory.
$a64 = (Resolve-Path $Dir).Path
Check 'arm64: translator DLL round-trips (host<->guest, dynarmic)' {
  & "$a64/arm64_smoke_test.exe"
} @('arm64_smoke_test all passed')

# Step 1 (guest ELF): the loader maps a self-contained arm64-v8a .so (NDK,
# -nostdlib), applies its relocations, runs its constructor through the
# translator and calls its exports; host_entry stubs enter the guest.
Check 'arm64: loader runs a self-contained arm64-v8a .so' {
  & "$a64/arm64_elf_test.exe" (NoDrive "$a64/trivial_arm64.so")
} @('arm64_elf_test all passed')

# Step 2 (guest libc): an arm64-v8a .so built against bionic's libc/libm/libdl/
# liblog/pthread; its imports are bound to guest trampolines that call the host
# shim across AAPCS64 <-> System V. Runs the library's own checks.
Check 'arm64: an arm64-v8a libc/pthread .so runs its checks through the shim' {
  & "$a64/arm64_elf_test.exe" (NoDrive "$a64/libc_test_arm64.so") (NoDrive "$a64")
} @('libc_test: all passed', 'arm64_elf_test all passed')

# Step 3/4 (guest JNI): dalvikvm runs gesher.Arm64Jni, whose System.load of an
# arm64-v8a .so goes through ART's host libnativeloader path to the ELF loader,
# the translator and the JNI bridge's guest proxy tables: JNI_OnLoad,
# RegisterNatives, natives by name, and calls back into ART both ways.
Check 'arm64: System.load of an arm64-v8a .so; JNI_OnLoad, RegisterNatives, Java_* and callbacks into ART' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/arm64-jni.dex") gesher.Arm64Jni `
    (NoDrive (Resolve-Path "$Dir/jni_test_arm64.so").Path)
} @('JNI_OnLoad ran: ok', 'Arm64Jni all passed')
