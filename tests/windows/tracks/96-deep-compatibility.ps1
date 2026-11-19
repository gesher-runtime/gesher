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

$deepSuggestionOut = FixesRun ($dvmFixes + 'gesher.services.WifiSuggestionValuesNativeTest', '--require-parcel') 90
$deepSuggestionCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertWifiSuggestionValuesParcel.ps1"
$deepSuggestionWireOK = $false
$deepSuggestionWireFailure = ''
try {
  Assert-WifiSuggestionValuesParcelEvidence -Output $deepSuggestionOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/wifi-suggestion-android-reference-2026-11-18.json')
  $deepSuggestionWireOK = $true
} catch { $deepSuggestionWireFailure = $_.Exception.Message }
Check 'framework: suggestion and Passpoint values, copy ownership and native Parcelable contracts' {
  $global:LASTEXITCODE = $deepSuggestionCode
  $deepSuggestionOut
  if ($deepSuggestionWireOK) { 'Suggestion/Passpoint native Parcel bytes match pinned Android16-r4: 24 cases' }
  else { $global:LASTEXITCODE = 1; "Suggestion/Passpoint native Parcel evidence failed: $deepSuggestionWireFailure" }
} @('Wifi suggestion values:', 'parcel=true', 'WIFI_SUGGESTION_NATIVE_PARCEL_REQUIRED_OK',
    'Suggestion/Passpoint dependency parameter and return identities verified',
    'Suggestion/Passpoint native Parcel bytes match pinned Android16-r4: 24 cases')

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
