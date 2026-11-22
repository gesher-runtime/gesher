# Additional compatibility contracts beyond PRs #10, #11 and #12.
# FixesRun and the actual ART boot arguments are provided by 94-framework-fixes.
$deepWifiOut = FixesRun ($dvmFixes + 'gesher.services.WifiDataContractTest') 90
$deepWifiCode = $LASTEXITCODE
Check 'framework: Wi-Fi data APIs and real native Parcelable round trips' {
  $global:LASTEXITCODE = $deepWifiCode
  $deepWifiOut
} @('WifiDataContractTest all passed:', 'parcel=true')

$deepSafetyCenterOut = FixesRun ($dvmFixes + 'gesher.services.SafetyCenterContractTest') 90
$deepSafetyCenterCode = $LASTEXITCODE
Check 'framework: android.safetycenter values on the boot class path, no Safety Center service' {
  $global:LASTEXITCODE = $deepSafetyCenterCode
  $deepSafetyCenterOut
} @('SafetyCenterContractTest: no Safety Center service, getSystemService returns null',
  'SafetyCenterContractTest: values build, validate and round-trip through Parcel',
  'SafetyCenterContractTest all passed:')

$deepOsIdentityOut = FixesRun ($dvmFixes + 'gesher.services.OsIdentityContractTest') 60
$deepOsIdentityCode = $LASTEXITCODE
Check 'framework: os.name and Os.uname() report Linux; the release claims no kernel version' {
  $global:LASTEXITCODE = $deepOsIdentityCode
  $deepOsIdentityOut
} @('OsIdentityContractTest: os.name=Linux', 'OsIdentityContractTest all passed:')

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

$deepChannelBlockingOut = FixesRun ($dvmFixes + 'gesher.services.WifiChannelBlockingValuesContractTest') 90
$deepChannelBlockingCode = $LASTEXITCODE
. "$PSScriptRoot/../helpers/AssertWifiChannelBlockingValuesParcel.ps1"
$deepChannelBlockingWireOK = $false
$deepChannelBlockingWireFailure = ''
try {
  Assert-WifiChannelBlockingValuesParcelEvidence -Output $deepChannelBlockingOut -Reference (Join-Path $PSScriptRoot '../../../docs/research/wifi-channel-blocking-values-android-reference-2026-11-18.json')
  $deepChannelBlockingWireOK = $true
} catch { $deepChannelBlockingWireFailure = $_.Exception.Message }
Check 'framework: Wi-Fi channel and blocking values with complete native readers' {
  $global:LASTEXITCODE = $deepChannelBlockingCode
  $deepChannelBlockingOut
  if ($deepChannelBlockingWireOK) { 'Channel/blocking native readers match pinned Android16-r4: 16 cases' }
  else { $global:LASTEXITCODE = 1; "Channel/blocking native Parcel evidence failed: $deepChannelBlockingWireFailure" }
} @('WIFI_CHANNEL_BLOCKING_VALUES: PASS',
    'WIFI_CHANNEL_BLOCKING_PROVIDERS coherent production boot graph; complete original readers',
    'WIFI_CHANNEL_BLOCKING_PROFILE android16-r4; sdk36; declarations=27',
    'Channel/blocking native readers match pinned Android16-r4: 16 cases')
