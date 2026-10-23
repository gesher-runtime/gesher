# Runs on a Windows machine against the CI build output.
#   pwsh tests/windows/run-tests.ps1 <dir with *.exe, hello.dex, hello.apk>
param([Parameter(Mandatory)] [string] $Dir)

$ErrorActionPreference = 'Stop'
$failed = 0

function Check([string] $Name, [scriptblock] $Run, [string[]] $Expect) {
  $out = & $Run 2>&1 | Out-String
  $missing = $Expect | Where-Object { -not $out.Contains($_) }
  if ($LASTEXITCODE -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
  } else {
    Write-Host "FAIL  $Name (exit $LASTEXITCODE; missing: $($missing -join ', '))"
    Write-Host $out
    $script:failed++
  }
}

Check 'platform probe' { & "$Dir/platform_probe.exe" } @('0 probe(s) failed')
Check 'socket probe' { & "$Dir/socket_probe.exe" } @('all passed')
Check 'epoll probe' { & "$Dir/epoll_probe.exe" } @('all passed')
Check 'libutils probe' { & "$Dir/libutils_probe.exe" } @('all passed')

Check 'dexdump: disassemble .dex' { & "$Dir/dexdump.exe" -d "$Dir/hello.dex" } @(
  "Class descriptor  : 'Lgesher/Hello;'",
  "name          : 'main'",
  'const-string v0, "Hello from ART on Windows"')

Check 'dexdump: read .dex from compressed .apk' { & "$Dir/dexdump.exe" -f "$Dir/hello.apk" } @(
  "hello.apk', DEX version '038'",
  'class_defs_size     : 1')

# The runtime starts.
Check 'dalvikvm: runtime starts' { & "$Dir/dalvikvm.exe" -showversion } @('ART version')

# dalvikvm runs Hello with the interpreter: without a boot image, the runtime
# defines and links the core classes from the boot jars, loads libcore's
# native libraries (libicu_jni maps ICU's data) and runs gesher.Hello.main.
# ART separates class path entries with ':', so paths are given without the
# drive letter.
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:RUNNER_TEMP 'gesher-root'
foreach ($d in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata') { New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null }
# ICU's data, where the i18n module keeps it on Android.
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$bcp = (('core-oj', 'core-libart', 'core-icu4j') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$env:GESHER_TRACE_FAULTS = '1'  # report a fault where it happens (crash.cc)
$out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' '-verbose:jni' -cp (NoDrive "$Dir/hello.dex") gesher.Hello 2>&1 | Out-String
$code = $LASTEXITCODE
Remove-Item Env:GESHER_TRACE_FAULTS
if ($code -eq 0 -and $out.Contains('Hello from ART on Windows')) {
  Write-Host 'PASS  dalvikvm: runs Hello with the interpreter'
} else {
  Write-Host ('FAIL  dalvikvm: runs Hello with the interpreter (exit 0x{0:X8})' -f $code)
  # Addresses as symbols, from the linker map (tools/symbolize.py).
  if (Test-Path "$Dir/dalvikvm.map") {
    $out | python "$PSScriptRoot/../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
  } else {
    Write-Host $out
  }
  $failed++
}

# Runs a class from natives.dex with the interpreter; passes when it exits 0
# and prints "all passed" (tests/dex-natives).
function RunDex([string] $Name, [string] $Main, [string[]] $Extra = @()) {
  $out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' @Extra -cp (NoDrive "$Dir/natives.dex") $Main 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0 -and $out.Contains('all passed')) {
    Write-Host "PASS  dalvikvm: $Name"
  } else {
    Write-Host ("FAIL  dalvikvm: $Name (exit 0x{0:X8})" -f $LASTEXITCODE)
    if (Test-Path "$Dir/dalvikvm.map") {
      $out | python "$PSScriptRoot/../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $out
    }
    $script:failed++
  }
}

# The framework natives (docs/framework-natives.md): libandroid_runtime's
# registrations and the @CriticalNative ABI canary (step 2), then the
# framework's message loop over its MessageQueue natives with both queue
# implementations (step 3).
RunDex 'framework natives (libandroid_runtime)' gesher.NativeTest
RunDex 'Looper, Handler, HandlerThread (legacy MessageQueue)' gesher.LooperTest
RunDex 'Looper, Handler, HandlerThread (concurrent MessageQueue)' gesher.LooperTest @('-Dgesher.mq.concurrent=true')

Write-Host "`n$failed test(s) failed"
exit $failed
