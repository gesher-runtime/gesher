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

$deepSoftApOut = FixesRun ($dvmFixes + 'gesher.services.SoftApValuesContractTest') 90
$deepSoftApCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertSoftApValuesParcel.ps1"
$deepSoftApWireOK = $false
$deepSoftApWireFailure = ''
try {
  Assert-SoftApValuesParcelEvidence -Output $deepSoftApOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/softap-values-android-reference-2026-11-18.json')
  $deepSoftApWireOK = $true
} catch { $deepSoftApWireFailure = $_.Exception.Message }
Check 'framework: SoftAP/WPS values, target SDK guards and native Parcelable contracts' {
  $global:LASTEXITCODE = $deepSoftApCode
  $deepSoftApOut
  if ($deepSoftApWireOK) { 'SoftAP/WPS native Parcel bytes match pinned Android16-r4: 12 cases' }
  else { $global:LASTEXITCODE = 1; "SoftAP/WPS native Parcel evidence failed: $deepSoftApWireFailure" }
} @('SoftApValuesContractTest all passed:', 'parcel=true', 'profile=android16-r4',
    'SoftAP generated target-SDK table matches 4 boundaries',
    'SoftAP/WPS native Parcel bytes match pinned Android16-r4: 12 cases')

$deepP2pOut = FixesRun ($dvmFixes + 'gesher.services.WifiP2pValuesContractTest') 90
$deepP2pCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertWifiP2pValuesParcel.ps1"
$deepP2pWireOK = $false
$deepP2pWireFailure = ''
try {
  Assert-WifiP2pValuesParcelEvidence -Output $deepP2pOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/wifi-p2p-values-android-reference-2026-11-18.json')
  $deepP2pWireOK = $true
} catch { $deepP2pWireFailure = $_.Exception.Message }
Check 'framework: Wi-Fi Direct values, coherent boot providers and native Parcelable readers' {
  $global:LASTEXITCODE = $deepP2pCode
  $deepP2pOut
  if ($deepP2pWireOK) { 'P2P native Parcel readers and Android16-r4 bytes qualified: 23 cases' }
  else { $global:LASTEXITCODE = 1; "P2P native Parcel evidence failed: $deepP2pWireFailure" }
} @('WifiP2pValuesContractTest passed:', 'WifiP2pValuesExtendedTest passed:',
    'P2P_VALUES_PROVIDERS coherent production boot graph; Parcel=null-loader Device identity verified',
    'P2P native Parcel readers and Android16-r4 bytes qualified: 23 cases')

$deepP2pNsdOut = FixesRun ($dvmFixes + 'gesher.services.WifiP2pNsdValuesContractTest') 90
$deepP2pNsdCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertWifiP2pNsdValuesParcel.ps1"
$deepP2pNsdWireOK = $false
$deepP2pNsdWireFailure = ''
try {
  Assert-WifiP2pNsdValuesParcelEvidence -Output $deepP2pNsdOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/wifi-p2p-nsd-values-android-reference-2026-11-18.json')
  $deepP2pNsdWireOK = $true
} catch { $deepP2pNsdWireFailure = $_.Exception.Message }
Check 'framework: Wi-Fi Direct NSD values, coherent providers and complete native readers' {
  $global:LASTEXITCODE = $deepP2pNsdCode
  $deepP2pNsdOut
  if ($deepP2pNsdWireOK) { 'P2P NSD native readers and Android16-r4 initial/rewrite bytes qualified: 20 cases' }
  else { $global:LASTEXITCODE = 1; "P2P NSD native Parcel evidence failed: $deepP2pNsdWireFailure" }
} @('WifiP2pNsdValuesContractTest passed:', 'WifiP2pNsdValuesExtendedTest passed:',
    'P2P_NSD_PROVIDERS coherent production boot graph; complete original readers',
    'P2P_NSD_PROFILE android16-r4; bp4a; sdk36; declarations=40',
    'P2P NSD native readers and Android16-r4 initial/rewrite bytes qualified: 20 cases')

$deepP2pDiscoveryOut = FixesRun ($dvmFixes + 'gesher.services.WifiP2pDiscoveryValuesContractTest') 90
$deepP2pDiscoveryCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertWifiP2pDiscoveryValuesParcel.ps1"
$deepP2pDiscoveryWireOK = $false
$deepP2pDiscoveryWireFailure = ''
try {
  Assert-WifiP2pDiscoveryValuesParcelEvidence -Output $deepP2pDiscoveryOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/wifi-p2p-discovery-values-android-reference-2026-11-18.json')
  $deepP2pDiscoveryWireOK = $true
} catch { $deepP2pDiscoveryWireFailure = $_.Exception.Message }
Check 'framework: Wi-Fi Direct discovery values and complete native readers' {
  $global:LASTEXITCODE = $deepP2pDiscoveryCode
  $deepP2pDiscoveryOut
  if ($deepP2pDiscoveryWireOK) { 'P2P discovery native readers and pinned Android16-r4 bytes qualified: 17 cases' }
  else { $global:LASTEXITCODE = 1; "P2P discovery native Parcel evidence failed: $deepP2pDiscoveryWireFailure" }
} @('P2P_DISCOVERY_VALUES: PASS',
    'P2P_DISCOVERY_PROVIDERS coherent production boot graph; complete original readers',
    'P2P_DISCOVERY_PROFILE android16-r4; sdk36; declarations=41',
    'P2P discovery native readers and pinned Android16-r4 bytes qualified: 17 cases')

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
