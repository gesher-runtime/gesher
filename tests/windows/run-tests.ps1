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

Check 'dexdump: disassemble .dex' { & "$Dir/dexdump.exe" -d "$Dir/hello.dex" } @(
  "Class descriptor  : 'Lgesher/Hello;'",
  "name          : 'main'",
  'const-string v0, "Hello from ART on Windows"')

Check 'dexdump: read .dex from compressed .apk' { & "$Dir/dexdump.exe" -f "$Dir/hello.apk" } @(
  "hello.apk', DEX version '038'",
  'class_defs_size     : 1')

# The runtime starts.
Check 'dalvikvm: runtime starts' { & "$Dir/dalvikvm.exe" -showversion } @('ART version')

# Progress check, until dalvikvm runs Hello: with the boot jars, the runtime
# defines and links the core classes without a boot image, loads libcore's
# native libraries and runs libopenjdk's JNI_OnLoad as far as System's static
# initializer, which needs the ICU natives (not ported yet). ART separates
# class path entries with ':', so paths are given without the drive letter.
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:RUNNER_TEMP 'gesher-root'
foreach ($d in 'system/framework', 'art', 'data') { New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null }
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$bcp = (NoDrive "$Dir/core-oj.jar") + ':' + (NoDrive "$Dir/core-libart.jar")
$env:GESHER_TRACE_FAULTS = '1'  # report a fault where it happens (crash.cc)
$out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' '-verbose:jni' -cp (NoDrive "$Dir/hello.dex") gesher.Hello 2>&1 | Out-String
$code = $LASTEXITCODE
Remove-Item Env:GESHER_TRACE_FAULTS
if ($out.Contains('Calling JNI_OnLoad in "libopenjdk.so"') -and
    $out.Contains('No implementation found for java.lang.String libcore.icu.ICU.getIcuVersion()')) {
  Write-Host 'PASS  dalvikvm: links the core classes from the boot jars (stops at the ICU natives)'
} else {
  Write-Host ('FAIL  dalvikvm: links the core classes from the boot jars (exit 0x{0:X8})' -f $code)
  # Addresses as symbols, from the linker map (tools/symbolize.py).
  if (Test-Path "$Dir/dalvikvm.map") {
    $out | python "$PSScriptRoot/../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
  } else {
    Write-Host $out
  }
  $failed++
}

Write-Host "`n$failed test(s) failed"
exit $failed
