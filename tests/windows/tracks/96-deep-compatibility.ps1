# Additional compatibility contracts beyond PRs #10, #11 and #12.
# FixesRun and the actual ART boot arguments are provided by 94-framework-fixes.
$deepWifiOut = FixesRun ($dvmFixes + 'gesher.services.WifiDataContractTest') 90
$deepWifiCode = $LASTEXITCODE
Check 'framework: Wi-Fi data APIs and real native Parcelable round trips' {
  $global:LASTEXITCODE = $deepWifiCode
  $deepWifiOut
} @('WifiDataContractTest all passed:', 'parcel=true')

$deepWifiConfigurationOut = FixesRun ($dvmFixes + 'gesher.services.WifiConfigurationContractTest') 90
$deepWifiConfigurationCode = $LASTEXITCODE
Check 'framework: Wi-Fi configuration, enterprise certificates and native Parcelable contracts' {
  $global:LASTEXITCODE = $deepWifiConfigurationCode
  $deepWifiConfigurationOut
} @('WifiConfigurationContractTest all passed:', 'parcel=true')

$deepWifiInfoOut = FixesRun ($dvmFixes + 'gesher.services.WifiInfoContractTest') 90
$deepWifiInfoCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertWifiInfoParcel.ps1"
$deepWifiInfoWireOK = $false
$deepWifiInfoWireFailure = ''
try {
  Assert-WifiInfoParcelEvidence -Output $deepWifiInfoOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/wifi-info-android-reference-2026-11-17.json')
  $deepWifiInfoWireOK = $true
} catch { $deepWifiInfoWireFailure = $_.Exception.Message }
Check 'framework: WifiInfo Builder, MLO, redaction and native Parcelable contracts' {
  $global:LASTEXITCODE = $deepWifiInfoCode
  $deepWifiInfoOut
  if ($deepWifiInfoWireOK) { 'WifiInfo native Parcel bytes match Android reference: 9 cases' }
  else { $global:LASTEXITCODE = 1; "WifiInfo native Parcel evidence failed: $deepWifiInfoWireFailure" }
} @('WifiInfoContractTest all passed:', 'parcel=true', 'WifiInfo native Parcel bytes match Android reference: 9 cases')

$deepWifiSpecifierOut = FixesRun ($dvmFixes + 'gesher.services.WifiNetworkSpecifierContractTest') 90
$deepWifiSpecifierCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertWifiNetworkSpecifierParcel.ps1"
$deepWifiSpecifierWireOK = $false
$deepWifiSpecifierWireFailure = ''
try {
  Assert-WifiNetworkSpecifierParcelEvidence -Output $deepWifiSpecifierOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/wifi-network-specifier-android-reference-2026-11-18.json')
  $deepWifiSpecifierWireOK = $true
} catch { $deepWifiSpecifierWireFailure = $_.Exception.Message }
Check 'framework: Wi-Fi specifier validation, matching and native NetworkRequest Parcel' {
  $global:LASTEXITCODE = $deepWifiSpecifierCode
  $deepWifiSpecifierOut
  if ($deepWifiSpecifierWireOK) { 'WifiNetworkSpecifier native Parcel bytes match pinned AOSP r4 reference: 12 cases' }
  else { $global:LASTEXITCODE = 1; "WifiNetworkSpecifier native Parcel evidence failed: $deepWifiSpecifierWireFailure" }
} @('WifiNetworkSpecifierContractTest all passed:', 'parcel=true', 'WifiNetworkSpecifier boot NetworkRequest Parcel passed: 4 checks', 'WifiNetworkSpecifier native Parcel bytes match pinned AOSP r4 reference: 12 cases')

$deepNetworkOut = FixesRun ($dvmFixes + 'gesher.services.NetworkSnapshotContractTest') 90
$deepNetworkCode = $LASTEXITCODE
Check 'framework: network route, address, snapshot and proxy contracts with native Parcel' {
  $global:LASTEXITCODE = $deepNetworkCode
  $deepNetworkOut
} @('NetworkSnapshotContractTest all passed:', 'parcel=true')

$deepSurfaceOut = RunTimed "$Dir/media_surface_geometry_contract_test.exe" @() 90
$deepSurfaceCode = $LASTEXITCODE
Check 'graphics: actual QueueSurface and Win32Surface geometry and damage contracts' {
  $global:LASTEXITCODE = $deepSurfaceCode
  $deepSurfaceOut
} @('surface geometry contracts:', 'native window dispatch contracts:', 'checks, 0 failures')

$deepSurfaceFramesOut = RunTimed "$Dir/media_surface_frame_recycling_test.exe" @() 90
$deepSurfaceFramesCode = $LASTEXITCODE
Check 'graphics: actual QueueSurface frame allocation and immutable-reader lifetimes' {
  $global:LASTEXITCODE = $deepSurfaceFramesCode
  $deepSurfaceFramesOut
} @('native surface frame recycling:', 'checks, 0 failures')

$deepAudioEnvironment = @{}
foreach ($name in 'ANDROID_DATA', 'GESHER_AUDIO') {
  $deepAudioEnvironment[$name] = @{
    Present = Test-Path "Env:$name"
    Value = [Environment]::GetEnvironmentVariable($name, 'Process')
  }
}
$deepAudioRoot = Join-Path $env:RUNNER_TEMP ('audio-state-contracts-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $deepAudioRoot | Out-Null
try {
  $env:ANDROID_DATA = NoDrive $deepAudioRoot
  $env:GESHER_AUDIO = 'null'
  $deepAudioOut = FixesRun ($dvmFixes + 'gesher.services.AudioStateContractTest') 90
  $deepAudioCode = $LASTEXITCODE
  Check 'media: actual AudioTrack JNI inactive writes, write interruption and static replay' {
    $global:LASTEXITCODE = $deepAudioCode
    $deepAudioOut
  } @('AudioStateContractTest all passed:')
} finally {
  foreach ($name in $deepAudioEnvironment.Keys) {
    $saved = $deepAudioEnvironment[$name]
    if ($saved.Present) { Set-Item "Env:$name" $saved.Value }
    else { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
  }
  Remove-Item -LiteralPath $deepAudioRoot -Recurse -Force -ErrorAction Stop
}
