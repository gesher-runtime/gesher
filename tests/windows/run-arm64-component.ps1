# Guarded component run; no broad suite/package/e2e acceptance.
param([Parameter(Mandatory)] [string] $Dir, [switch] $ContinueAfterComponent)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path $Dir).Path
$failed = 0
function NoDrive([string] $path) { ($path -replace '\\', '/') -replace '^[A-Za-z]:', '' }
function Check([string] $Name, [scriptblock] $Run, [string[]] $Expect) {
  $out = & $Run 2>&1 | Out-String
  $missing = $Expect | Where-Object { -not $out.Contains($_) }
  if ($LASTEXITCODE -eq 0 -and -not $missing) { Write-Host "PASS  $Name" }
  else { Write-Host "FAIL  $Name (exit $LASTEXITCODE; missing: $($missing -join ', '))"; Write-Host $out; $script:failed++ }
}
$root = Join-Path $env:RUNNER_TEMP 'gesher-arm64-component'
foreach ($folder in 'system/framework/x86_64','system/fonts','system/etc','art','data','i18n/etc/icu','tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $folder) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
Copy-Item "$Dir/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$Dir/fonts/fonts.xml" (Join-Path $root 'system/etc')
Copy-Item "$Dir/framework/x86_64/*" (Join-Path $root 'system/framework/x86_64')
Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $root 'tzdata')
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
$env:GESHER_TRACE_FAULTS = '1'
$bcp = (('core-oj','core-libart','core-icu4j') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$a64 = $Dir

# Preserve every original Native2 body/assertion, including both APK modes.
. "$PSScriptRoot/tracks/44-native2.ps1"
# The positive variadic runner supplies the bounded process helper below.
. "$PSScriptRoot/run-arm64-varargs.ps1" -Dir $Dir
function A64Run([string] $Exe, [string[]] $Arguments, [int] $Seconds = 300) {
  $text = InvokeVarargs $Exe $Arguments
  $global:LASTEXITCODE = 0
  return $text
}
. "$PSScriptRoot/tracks/69-arm64-fpcr.ps1"
. "$PSScriptRoot/tracks/75-arm64-fpsr.ps1"
Check 'arm64 component: original JNI primitive/mixed V plus independent copied lists through actual ART' {
  A64Run "$Dir/dalvikvm.exe" @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
    '-Xint','-Xusejit:false','-cp',(NoDrive "$Dir/arm64-jni.dex"),'gesher.Arm64Jni',(NoDrive "$Dir/jni_test_arm64.so"))
} @('Arm64Jni all passed','CallStaticIntMethodV independent va_copy','CallStaticObjectMethodV independent va_copy')
foreach ($record in $arm64FpcrEvidence) {
  if ($record.Exit -ne 0 -or $record.Report -notmatch 'native2 varargs: ok; literal/few/many GP/FP; ordered shared overflow; va_copy; nested/thread; wide and binary128 source; gzvprintf retry') {
    Write-Host "FAIL arm64 component varargs: $($record.Abi)/$($record.Mode) exact current report absent"
    $failed++
  } else { Write-Host "PASS arm64 component varargs: $($record.Abi)/$($record.Mode) exact current consumer report" }
}
if ($arm64FpcrEvidence.Count -ne 6) { throw 'Missing original two-ABI three-context Native2 evidence' }
Write-Host "ACTUAL_ARM64_COMPONENT_SUMMARY $failed test(s) failed; current API3 runtime/DLL and guarded boot jars"
if ($failed) { exit 1 }
if (-not $ContinueAfterComponent) { exit 0 }
