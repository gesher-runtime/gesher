# Exercise the required gate's failure behavior without substituting any native execution.
param([string] $Reference = (Join-Path $PSScriptRoot '../../docs/research/wifi-suggestion-android-reference-2026-11-18.json'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/helpers/AssertWifiSuggestionValuesParcel.ps1"
$value = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
$valid = @($value.wire_cases | ForEach-Object {
  "SUGGESTION_WIRE name=$($_.case) bytes=$($_.bytes) sha256=$($_.sha256)"
})
$checks = 0
Assert-WifiSuggestionValuesParcelEvidence -Output $valid -Reference $Reference
$checks++
function Reject([string[]] $Lines, [string] $Path = $Reference) {
  $failed = $false
  try { Assert-WifiSuggestionValuesParcelEvidence -Output $Lines -Reference $Path }
  catch { $failed = $true }
  if (-not $failed) { throw 'Invalid wire evidence was accepted' }
  $script:checks++
}
Reject @()
Reject @('WifiSuggestionValuesContractTest all passed: 999 checks; parcel=true')
Reject $valid[0..22]
Reject (@($valid) + $valid[0])
Reject (@($valid[0..22]) + $valid[0])
Reject (@($valid) + 'SUGGESTION_WIRE malformed-extra')
$bad = @($valid); $bad[0] = $bad[0] -replace 'bytes=[0-9]+', 'bytes=1'; Reject $bad
$bad = @($valid); $bad[0] = $bad[0] -replace 'sha256=[0-9a-f]{64}', ('sha256=' + ('0' * 64)); Reject $bad
$bad = @($valid); $bad[0] = $bad[0].Replace('open', 'Open'); Reject $bad
$bad = @($valid); $bad[0] = $bad[0] -replace 'sha256=[0-9a-f]{64}', 'sha256=bad'; Reject $bad
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('suggestion-wire-negative-' + [guid]::NewGuid().ToString('N') + '.json')
try {
  $wrong = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  $wrong.wire_reference.profile = 'stock-api36'
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary
  Reject $valid $temporary
  $wrong = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  $wrong.wire_reference.aosp_wifi_commit = 'unqualified'
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary
  Reject $valid $temporary
  $wrong = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  $wrong.wire_cases[23] = $wrong.wire_cases[0]
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary
  Reject $valid $temporary
} finally {
  if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
"Suggestion/Passpoint wire-evidence checker passed: $checks checks"
