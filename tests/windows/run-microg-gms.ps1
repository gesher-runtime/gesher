# CI-only actual ART/APK execution for GMS & microG compatibility.
param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'
$failed = 0
function NoDrive([string] $Path) { ($Path -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:RUNNER_TEMP 'gesher-microg-gms'
foreach ($dirName in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata', 'logs') {
  New-Item -ItemType Directory -Force (Join-Path $root $dirName) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
if (Test-Path "$Dir/tzdata") { Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $root 'tzdata') }
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"

$bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$bcpApp = $bcp

. "$PSScriptRoot/../helpers/CloseProcessReaders.ps1"
. "$PSScriptRoot/tracks/50-services.ps1"
. "$PSScriptRoot/tracks/52-microg.ps1"

Write-Host 'INFO  microg-gms: running 93-microg-gms.ps1 track'
. "$PSScriptRoot/tracks/93-microg-gms.ps1"
Write-Host "$failed test(s) failed"
if ($failed) { exit 1 }
