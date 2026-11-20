# Gate logic only. These constructed log lines are not native runtime evidence.
param([string] $Reference = (Join-Path $PSScriptRoot '../../docs/research/wifi-p2p-values-android-reference-2026-11-18.json'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/helpers/AssertWifiP2pValuesParcel.ps1"
$value = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
$records = @($value.wire_cases | ForEach-Object {
  "P2P_VALUES_PARCEL $($_.case) bytes=$($_.bytes) sha256=$($_.sha256) roundtrip=true"
})
$provider = 'P2P_VALUES_PROVIDERS coherent production boot graph; Parcel=null-loader Device identity verified'
$public = 'WifiP2pValuesContractTest passed: 1 checks; parcel=true; profile=android16-r4; bootCoherentP2p=true'
$extended = 'WifiP2pValuesExtendedTest passed: 1 checks; parcel=true; bootCoherentP2p=true'
$valid = @($provider) + $records[0..18] + @($public) + $records[19..22] + @($extended)
$checks = 0
Assert-WifiP2pValuesParcelEvidence -Output $valid -Reference $Reference
$checks++
function Reject([string[]] $Lines, [string] $Path = $Reference) {
  $failed = $false
  try { Assert-WifiP2pValuesParcelEvidence -Output $Lines -Reference $Path }
  catch { $failed = $true }
  if (-not $failed) { throw 'Incomplete P2P reader/writer evidence was accepted' }
  $script:checks++
}
Reject @()
Reject @($provider, $public, $extended)
Reject $valid[1..25]
Reject (@($valid) + $records[0])
Reject (@($valid) + 'P2P_VALUES_PARCEL incomplete')
Reject (@($valid) + 'P2P_VALUES_READER_PENDING devicelist_two')
$bad = @($valid); $bad[1] = $bad[1] -replace 'roundtrip=true', 'roundtrip=false'; Reject $bad
$bad = @($valid); $bad[1] = $bad[1] -replace 'bytes=[0-9]+', 'bytes=1'; Reject $bad
$bad = @($valid); $bad[1] = $bad[1] -replace 'sha256=[0-9a-f]{64}', ('sha256=' + ('0' * 64)); Reject $bad
$bad = @($valid); $bad[1] = $bad[1].Replace('config_default', 'Config_Default'); Reject $bad
$bad = @($valid); $bad[1] = $records[1]; Reject $bad
$bad = @($valid); $bad[20] = $public.Replace('bootCoherentP2p=true', 'bootCoherentP2p=false'); Reject $bad
$bad = @($valid); $bad[25] = $extended.Replace('parcel=true', 'parcel=false'); Reject $bad
Reject (@($public, $provider) + $records + @($extended))
Reject (@($records) + @($provider, $public, $extended))
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('p2p-wire-gate-' + [guid]::NewGuid().ToString('N') + '.json')
try {
  foreach ($field in @('profile', 'release_profile', 'aosp_wifi_commit')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
    $wrong.wire_reference.$field = 'unqualified'
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary
    Reject $valid $temporary
  }
  $wrong = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  $wrong.wire_cases[22] = $wrong.wire_cases[0]
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary
  Reject $valid $temporary
} finally {
  if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
"P2P wire-evidence checker passed: $checks checks"
