# Actual test79 bodies against immutable matching full-run runtime artifacts.
# Component evidence only: full Windows/package/e2e acceptance remains required.
param([Parameter(Mandatory)] [string] $Dir, [Parameter(Mandatory)] [string] $PathMode)
$ErrorActionPreference = 'Stop'
$failed = 0
$absoluteDist = (Resolve-Path $Dir).Path
function NoDrive([string] $path) { ($path -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:RUNNER_TEMP "gesher-muxer-component-$PathMode"
foreach ($folder in 'system/framework/x86_64','system/fonts','system/etc','art','data','i18n/etc/icu','tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $folder) | Out-Null
}
Copy-Item "$absoluteDist/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
Copy-Item "$absoluteDist/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$absoluteDist/build.prop" (Join-Path $root 'system')
Copy-Item "$absoluteDist/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$absoluteDist/fonts/fonts.xml" (Join-Path $root 'system/etc')
Copy-Item "$absoluteDist/framework/x86_64/*" (Join-Path $root 'system/framework/x86_64')
Copy-Item -Recurse -Force "$absoluteDist/tzdata/*" (Join-Path $root 'tzdata')
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
$env:GESHER_AUDIO = 'null'
$env:GESHER_SHORTCUTS = '0'
$bcp = (('core-oj','core-libart','core-icu4j') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
Write-Host "ACTUAL_MUXER_COMPONENT_PATH mode=$PathMode Dir=$Dir caller=$((Get-Location).ProviderPath)"
. "$PSScriptRoot/tracks/79-media4-muxer.ps1"
Write-Host "ACTUAL_MUXER_COMPONENT_SUMMARY mode=$PathMode $failed test(s) failed"
if ($failed) { exit 1 }
