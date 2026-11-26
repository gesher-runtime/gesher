# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Execute the production addon script. Substitute network and final APK launcher
# only; profile discovery, file promotion, failure propagation and environment
# restoration run against real files. No downloads or Android runtime required.
param([string] $PackageDirectory = (Join-Path $PSScriptRoot '../../../tools/package'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
$addon = Join-Path $PackageDirectory 'gesher-app-microg.ps1'
$script:checks = 0
function Assert-Addon([bool] $condition, [string] $message) {
    if (-not $condition) { throw $message }
    $script:checks++
}
$saved = @{}
foreach ($name in 'GESHER_PROFILE', 'GESHER_PORTABLE', 'GESHER_PANEL_UI_LANGUAGE', 'LOCALAPPDATA', 'TEMP', 'GESHER_ADDON_CHECK_RECORD', 'GESHER_ADDON_CHECK_EXIT') {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-addon-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $portable = Join-Path $work 'USB'
    $runtime = Join-Path $portable 'App'
    $normal = Join-Path $work 'Installed'
    foreach ($directory in $runtime, $normal) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $PackageDirectory 'gesher-runtime-files.ps1') -Destination $directory
        [IO.File]::WriteAllText((Join-Path $directory 'gesher-app.ps1'), @'
param([Parameter(ValueFromRemainingArguments)][string[]] $Arguments)
. (Join-Path $PSScriptRoot 'gesher-runtime-files.ps1')
$profile = Set-GesherProfileEnvironment $PSScriptRoot
@{ Profile = $profile; Portable = $env:GESHER_PORTABLE; Arguments = $Arguments } |
    ConvertTo-Json | Set-Content -LiteralPath $env:GESHER_ADDON_CHECK_RECORD -Encoding UTF8
exit ([int]$env:GESHER_ADDON_CHECK_EXIT)
'@)
    }
    [IO.File]::WriteAllText((Join-Path $portable 'portable.marker'), '')
    $env:GESHER_ADDON_CHECK_RECORD = Join-Path $work 'launched.json'
    $env:GESHER_ADDON_CHECK_EXIT = '0'
    $env:GESHER_PANEL_UI_LANGUAGE = 'en'
    $env:LOCALAPPDATA = Join-Path $work 'HostProfile'
    $env:TEMP = Join-Path $work 'HostTemp'
    $env:GESHER_PROFILE = Join-Path $work 'CallerProfile'
    $callerProfile = $env:GESHER_PROFILE
    $env:GESHER_PORTABLE = '0'
    $targetProfile = Join-Path $work 'ExplicitProfile'
    $apk = Join-Path $work 'supplied.apk'
    [IO.File]::WriteAllText($apk, 'APK boundary fixture')

    $probe = @{ Downloads = 0; FailDownload = $true; DownloadUri = '' }
    function Invoke-WebRequest {
        param([string] $Uri, [string] $OutFile, [switch] $UseBasicParsing)
        $probe.Downloads++
        $probe.DownloadUri = $Uri
        [IO.File]::WriteAllText($OutFile, 'APK boundary fixture')
        if ($probe.FailDownload) { throw [IO.IOException]::new('Interrupted fixture download') }
    }

    & $addon -StoreOnly -StoreApk $apk -ProfileRoot $targetProfile -RuntimeDir $runtime
    $record = Get-Content -LiteralPath $env:GESHER_ADDON_CHECK_RECORD -Raw | ConvertFrom-Json
    Assert-Addon ($record.Profile -ceq $targetProfile) 'Store install ignored its explicit new profile'
    Assert-Addon ($record.Portable -ceq '1') 'Portable store install enabled host integration'
    Assert-Addon ($record.Arguments[0] -ceq 'install' -and $record.Arguments[1] -ceq $apk) 'Supplied APK did not reach the launcher'
    Assert-Addon ($probe.Downloads -eq 0) 'Supplied APK unexpectedly triggered network access'
    Assert-Addon ($env:GESHER_PROFILE -ceq $callerProfile -and $env:GESHER_PORTABLE -ceq '0') 'Store launch leaked its environment to the caller'

    $env:GESHER_ADDON_CHECK_EXIT = '7'
    $failed = $false
    try { & $addon -StoreOnly -StoreApk $apk -ProfileRoot $targetProfile -RuntimeDir $runtime } catch { $failed = $true }
    Assert-Addon $failed 'Failed APK launcher was reported as successful'
    Assert-Addon ($env:GESHER_PROFILE -ceq $callerProfile -and $env:GESHER_PORTABLE -ceq '0') 'Failed store launch leaked its environment'
    $env:GESHER_ADDON_CHECK_EXIT = '0'

    $failed = $false
    try { & $addon -StoreOnly -ProfileRoot $targetProfile -RuntimeDir $runtime } catch { $failed = $true }
    $cache = Join-Path $targetProfile 'downloads/AuroraStore-4.8.4.apk'
    Assert-Addon $failed 'Interrupted download unexpectedly succeeded'
    Assert-Addon (-not (Test-Path -LiteralPath $cache)) 'Interrupted download was cached as a complete APK'
    Assert-Addon (-not (Test-Path -LiteralPath ($cache + '.partial'))) 'Interrupted download left a partial file'
    Assert-Addon (-not (Test-Path -LiteralPath $env:TEMP)) 'Portable addon download wrote host TEMP'

    $probe.FailDownload = $false
    & $addon -StoreOnly -ProfileRoot $targetProfile -RuntimeDir $runtime
    $record = Get-Content -LiteralPath $env:GESHER_ADDON_CHECK_RECORD -Raw | ConvertFrom-Json
    Assert-Addon ($probe.Downloads -eq 2) 'Retry reused interrupted bytes instead of downloading'
    Assert-Addon ($probe.DownloadUri -ceq 'https://auroraoss.com/downloads/AuroraStore/Release/AuroraStore-4.8.4.apk') 'Aurora download regressed from the pinned version'
    Assert-Addon ((Test-Path -LiteralPath $cache) -and -not (Test-Path -LiteralPath ($cache + '.partial'))) 'Complete download was not atomically promoted'
    Assert-Addon ($record.Arguments[1] -ceq $cache) 'Portable cached APK did not reach the launcher'
    & $addon -StoreOnly -ProfileRoot $targetProfile -RuntimeDir $runtime
    Assert-Addon ($probe.Downloads -eq 2) 'Complete cached APK was downloaded again'

    $env:GESHER_PROFILE = $null
    & $addon -StoreOnly -StoreApk $apk -RuntimeDir $runtime
    $record = Get-Content -LiteralPath $env:GESHER_ADDON_CHECK_RECORD -Raw | ConvertFrom-Json
    Assert-Addon ($record.Profile -ceq (Join-Path $portable 'Data')) 'Implicit addon profile diverged from the portable launcher'
    Assert-Addon (-not $env:GESHER_PROFILE) 'Implicit addon profile leaked into the caller'
    $env:GESHER_PORTABLE = '1'
    & $addon -StoreOnly -StoreApk $apk -RuntimeDir $normal
    $record = Get-Content -LiteralPath $env:GESHER_ADDON_CHECK_RECORD -Raw | ConvertFrom-Json
    Assert-Addon ($record.Profile -ceq (Join-Path $env:LOCALAPPDATA 'Gesher') -and $record.Portable -ceq '0') 'Regular addon launch inherited portable state'
    Assert-Addon ($env:GESHER_PORTABLE -ceq '1') 'Regular addon launch changed caller environment'
    Assert-Addon (-not (Test-Path -LiteralPath $env:LOCALAPPDATA)) 'Fixture unexpectedly wrote host profile data'
    Write-Host "Portable addon checks passed ($script:checks)."
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    Remove-Item -LiteralPath $work -Recurse -Force
}
