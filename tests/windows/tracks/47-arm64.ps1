# Phase 7: arm64 translation (docs/arm64.md, ADR 0047). Dot-sourced by
# run-tests.ps1, so Check, NoDrive, $Dir, $root and $bcp are in scope.
#
# gesher_arm64.dll (the dynarmic-based translator), its smoke test and the
# arm64-v8a guest test libraries are built by .github/workflows/arm64.yml and
# downloaded into $Dir.
#
# Every arm64 run goes through A64Run, which kills the process after a timeout:
# a translated guest must never hang the test job (a fault in guest code must
# end the process with an Android-style "Fatal signal" report instead).

# gesher_arm64.dll is loaded by base name from the executable's directory
# ($Dir). Absolute paths: $Dir may be relative to the starting directory.
$a64 = (Resolve-Path $Dir).Path

function A64Run([string] $Exe, [string[]] $Arguments, [int] $Seconds = 300) {
  $out = Join-Path $env:RUNNER_TEMP "a64-$([guid]::NewGuid()).out"
  $err = "$out.err"
  $quoted = $Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
  $p = Start-Process -FilePath $Exe -ArgumentList $quoted -NoNewWindow -PassThru `
    -RedirectStandardOutput $out -RedirectStandardError $err
  if (-not $p.WaitForExit($Seconds * 1000)) {
    $p.Kill()
    $p.WaitForExit()
    Write-Output "TIMEOUT after $Seconds s: $Exe $($Arguments -join ' ')"
    $global:LASTEXITCODE = 124
  } else {
    $global:LASTEXITCODE = $p.ExitCode
  }
  Get-Content $out -ErrorAction SilentlyContinue
  Get-Content $err -ErrorAction SilentlyContinue
  Remove-Item $out, $err -ErrorAction SilentlyContinue
}

# A run that must crash the way Android reports a native crash: the expected
# lines, and an exit code (128 + signal) rather than success or a timeout.
function A64Crash([string] $Name, [string] $Exe, [string[]] $Arguments, [string[]] $Expect,
                  [int] $ExitCode) {
  $out = A64Run $Exe $Arguments 60 | Out-String
  $missing = $Expect | Where-Object { -not $out.Contains($_) }
  if ($LASTEXITCODE -eq $ExitCode -and -not $missing) {
    Write-Host "PASS  $Name"
  } else {
    Write-Host "FAIL  $Name (exit $LASTEXITCODE, wanted $ExitCode; missing: $($missing -join ', '))"
    Write-Host $out
    $script:failed++
  }
}

# Step 1: the translator itself. Hand-assembled AArch64: host->guest through
# call() and a host_entry System V stub, guest->host through a bound
# trampoline, a host->guest callback through the DEP-fault redirect, guest
# LL/SC atomics from four threads, and the overhead of a register-only loop.
Check 'arm64: translator DLL round-trips (host<->guest, dynarmic)' {
  A64Run "$a64/arm64_smoke_test.exe" @()
} @('arm64_smoke_test all passed')

# Guest faults end the process Android-style (never a hang).
A64Crash 'arm64: a guest null dereference is a SIGSEGV crash report' "$a64/arm64_smoke_test.exe" `
  @('crash-segv') @('Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0') 139
A64Crash 'arm64: an undefined guest instruction is a SIGILL crash report' "$a64/arm64_smoke_test.exe" `
  @('crash-ill') @('Fatal signal 4 (SIGILL)') 132
A64Crash 'arm64: a guest jump to no code is a SIGSEGV crash report' "$a64/arm64_smoke_test.exe" `
  @('crash-jump') @('Fatal signal 11 (SIGSEGV)', 'fault addr 0x40') 139

# Step 1 (guest ELF): the loader maps a self-contained arm64-v8a .so (NDK,
# -nostdlib), applies its relocations, runs its constructor through the
# translator and calls its exports; host_entry stubs enter the guest.
Check 'arm64: loader runs a self-contained arm64-v8a .so' {
  A64Run "$a64/arm64_elf_test.exe" @((NoDrive "$a64/trivial_arm64.so"))
} @('arm64_elf_test all passed')

# Step 2 (guest libc): an arm64-v8a .so built against bionic's libc/libm/libdl/
# liblog/pthread; its imports are bound to guest trampolines that call the host
# shim across AAPCS64 <-> System V. Runs the library's own checks.
Check 'arm64: an arm64-v8a libc/pthread .so runs its checks through the shim' {
  A64Run "$a64/arm64_elf_test.exe" @((NoDrive "$a64/libc_test_arm64.so"), (NoDrive "$a64"))
} @('libc_test: all passed', 'arm64_elf_test all passed')

# The guest-ABI checks (tests/native/arm64_abi_test.c): ELF TLS through
# TLSDESC in several threads, the stack protector, setjmp/longjmp, struct stat
# and epoll_event layouts, arm64 syscall numbers, AT_HWCAP, C11 atomics,
# va_list conversion, a host pointer called by guest code; and the overhead
# of the translated compiled kernel against the same code built natively.
Check 'arm64: guest ABI (TLS, stack protector, setjmp, stat, epoll, syscall, atomics, va_list)' {
  A64Run "$a64/arm64_elf_test.exe" @((NoDrive "$a64/arm64_abi_test.so"), (NoDrive "$a64"))
} @('arm64_abi_test: all passed', 'arm64_elf_test all passed', 'INFO  arm64 perf: compiled C kernel')

# Step 3/4 (guest JNI): dalvikvm runs gesher.Arm64Jni, whose System.load of an
# arm64-v8a .so goes through ART's host libnativeloader path to the ELF loader,
# the translator and the JNI bridge's guest proxy tables: JNI_OnLoad,
# RegisterNatives, natives by name, calls back into ART both ways, the
# va_list forms, nesting, and a guest pthread attached to the VM.
Check 'arm64: System.load of an arm64-v8a .so; JNI both ways, Call*MethodV, nesting, an attached guest thread' {
  A64Run "$Dir/dalvikvm.exe" @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
    '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/arm64-jni.dex"), 'gesher.Arm64Jni',
    (NoDrive (Resolve-Path "$Dir/jni_test_arm64.so").Path))
} @('JNI_OnLoad ran: ok', 'Arm64Jni all passed')

# An arm64-only APK through gesher-app: tests/apps/nativeactivity built with
# only lib/arm64-v8a (arm64.yml). gesher-app picks arm64-v8a (NativeLibs.abiFor,
# ApplicationInfo.primaryCpuAbi), NativeActivity loads the guest .so, and its
# android_native_app_glue thread, libandroid (ALooper, AInputQueue, AAssetManager,
# AChoreographer), EGL/GLES 2 and ANativeWindow_lock all run translated: the
# same checks and pixels as the x86_64 app (tracks/41-native-activity.ps1).
if (Get-Command NativeActivityCheck -ErrorAction SilentlyContinue) {
  NativeActivityCheck 'arm64: gesher-app runs nativeactivity-arm64.apk (only lib/arm64-v8a, translated): the ABI picked, ANativeActivity_onCreate, the glue thread, GLES 2 (glReadPixels, PrintWindow), AChoreographer, a click and a key through AInputQueue, an ANativeWindow_lock frame; WM_CLOSE finishes it' `
    @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/nativeactivity-arm64.apk")) 'close' `
    ($nativeActivityLines + @('native library (arm64-v8a)', 'closed by the user', 'gesher-app: done'))
} else {
  Write-Host 'FAIL  arm64: NativeActivity (tracks/41-native-activity.ps1 did not define NativeActivityCheck)'
  $failed++
}

# The perf lines, for the job summary (docs/arm64.md records them).
A64Run "$a64/arm64_smoke_test.exe" @() | Select-String 'INFO  arm64 perf' | ForEach-Object { Write-Host $_.Line }
A64Run "$a64/arm64_elf_test.exe" @((NoDrive "$a64/arm64_abi_test.so"), (NoDrive "$a64")) |
  Select-String 'INFO  arm64 perf' | ForEach-Object { Write-Host $_.Line }
