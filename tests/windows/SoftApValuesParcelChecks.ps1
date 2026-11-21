# Exercise the required gate's failure behavior without substituting any native execution.
param([string] $Reference = (Join-Path $PSScriptRoot '../../docs/research/softap-values-android-reference-2026-11-18.json'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/helpers/AssertSoftApValuesParcel.ps1"
$value = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
$valid = @($value.wire_cases | ForEach-Object {
  "SOFTAP_VALUES_PARCEL $($_.case) bytes=$($_.bytes) sha256=$($_.sha256)"
})
$checks = 0
Assert-SoftApValuesParcelEvidence -Output $valid -Reference $Reference
$checks++
function Reject([string[]] $Lines, [string] $Path = $Reference) {
  $failed = $false
  try { Assert-SoftApValuesParcelEvidence -Output $Lines -Reference $Path }
  catch { $failed = $true }
  if (-not $failed) { throw 'Invalid wire evidence was accepted' }
  $script:checks++
}
Reject @()
Reject @('SoftApValuesContractTest all passed: 999 checks; parcel=true')
Reject $valid[0..10]
Reject (@($valid) + $valid[0])
Reject (@($valid[0..10]) + $valid[0])
Reject (@($valid) + 'SOFTAP_VALUES_PARCEL malformed-extra')
$bad = @($valid); $bad[0] = $bad[0] -replace 'bytes=[0-9]+', 'bytes=1'; Reject $bad
$bad = @($valid); $bad[0] = $bad[0] -replace 'sha256=[0-9a-f]{64}', ('sha256=' + ('0' * 64)); Reject $bad
$bad = @($valid); $bad[0] = $bad[0].Replace('softap_default', 'Softap_Default'); Reject $bad
$bad = @($valid); $bad[0] = $bad[0] -replace 'sha256=[0-9a-f]{64}', 'sha256=bad'; Reject $bad
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('softap-wire-negative-' + [guid]::NewGuid().ToString('N') + '.json')
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
  $wrong.wire_cases[11] = $wrong.wire_cases[0]
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary
  Reject $valid $temporary
} finally {
  if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
"SoftAP/WPS wire-evidence checker passed: $checks checks"
