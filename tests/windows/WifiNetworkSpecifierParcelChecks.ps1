# Checker contracts only; constructed lines do not qualify native Parcel execution.
param(
  [string] $Reference = (Join-Path $PSScriptRoot '../../docs/research/wifi-network-specifier-android-reference-2026-11-18.json'),
  [string] $HelperPath = (Join-Path $PSScriptRoot 'helpers/AssertWifiNetworkSpecifierParcel.ps1')
)
$ErrorActionPreference = 'Stop'
. $HelperPath
$value = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
$valid = @($value.wire_cases | ForEach-Object {
  "WIFI_SPECIFIER_PARCEL $($_.case) bytes=$($_.bytes) sha256=$($_.sha256)"
})
$checks = 0
foreach ($lines in @(
  @{ Value = $valid },
  @{ Value = @($valid -join "`r`n") },
  @{ Value = @('Unrelated runtime diagnostic') + $valid + @('Runtime finished') },
  @{ Value = $valid[11..0] }
)) {
  Assert-WifiNetworkSpecifierParcelEvidence -Output $lines.Value -Reference $Reference
  $checks++
}
function Reject([string[]] $Lines, [string] $Reason, [string] $Path = $Reference) {
  $failed = $false
  try { Assert-WifiNetworkSpecifierParcelEvidence -Output $Lines -Reference $Path }
  catch { $failed = $true }
  if (-not $failed) { throw "Invalid specifier native Parcel evidence accepted: $Reason" }
  $script:checks++
}
# These extra records used to be ignored when twelve well-formed records preceded them.
Reject (@($valid) + 'WIFI_SPECIFIER_PARCEL malformed-extra') 'malformed extra record'
Reject (@($valid) + 'WIFI_SPECIFIER_PARCEL') 'bare extra marker'
Reject (@($valid) + "WIFI_SPECIFIER_PARCEL`ttruncated") 'tab-delimited malformed extra record'
Reject (@($valid) + ($valid[0] + ' trailing-field')) 'extra record with trailing field'
Reject @() 'missing output'
Reject @('WifiNetworkSpecifierContractTest all passed: 999 checks; parcel=true') 'summary without records'
Reject $valid[0..10] 'missing record'
Reject (@($valid) + $valid[0]) 'duplicate extra record'
$bad = @($valid); $bad[0] = $valid[1]; Reject $bad 'duplicate replacing another record'
$bad = @($valid); $bad[0] = $bad[0] -creplace ' bytes=[0-9]+', ' bytes=1'; Reject $bad 'wrong byte count'
$bad = @($valid); $bad[0] = $bad[0] -creplace ' sha256=[0-9a-f]{64}', (' sha256=' + ('0' * 64)); Reject $bad 'wrong digest'
$bad = @($valid); $bad[0] = $bad[0] -creplace ' sha256=[0-9a-f]{64}', ' sha256=bad'; Reject $bad 'malformed digest'
$bad = @($valid); $bad[0] = $bad[0] -creplace '^WIFI_SPECIFIER_PARCEL \S+', 'WIFI_SPECIFIER_PARCEL unknown'; Reject $bad 'unknown case'
$bad = @($valid); $bad[0] = $bad[0].Replace($value.wire_cases[0].case, $value.wire_cases[0].case.ToUpperInvariant()); Reject $bad 'case identity changed'
$bad = @($valid); $bad[0] += ' trailing-field'; Reject $bad 'replacement record with trailing field'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('wifi-specifier-wire-gate-' + [guid]::NewGuid().ToString('N') + '.json')
try {
  foreach ($field in @('wire_profile', 'aosp_wifi_commit')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
    $wrong.$field = 'unqualified'
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid "unqualified reference $field" $temporary
  }
  $wrong = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  $wrong.wire_cases[11] = $wrong.wire_cases[0]
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
  Reject $valid 'duplicate reference case' $temporary
} finally {
  if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
"Wi-Fi specifier wire-evidence checker passed: $checks checks"
