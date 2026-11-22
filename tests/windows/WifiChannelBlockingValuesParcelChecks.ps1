# Checker logic only; these constructed lines do not qualify native execution.
param([string] $Reference = (Join-Path $PSScriptRoot '../../docs/research/wifi-channel-blocking-values-android-reference-2026-11-18.json'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/helpers/AssertWifiChannelBlockingValuesParcel.ps1"
$value = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
$records = @($value.wire_cases | ForEach-Object {
  "WIFI_CHANNEL_BLOCKING_PARCEL $($_.case) bytes=$($_.bytes) sha256=$($_.sha256) rewrite_sha256=$($_.rewrite_sha256) roundtrip=true reader=$($_.reader)"
})
$provider = 'WIFI_CHANNEL_BLOCKING_PROVIDERS coherent production boot graph; complete original readers'
$profile = 'WIFI_CHANNEL_BLOCKING_PROFILE android16-r4; sdk36; declarations=27'
$complete = "WIFI_CHANNEL_BLOCKING_VALUES: PASS checks=$($value.wire_reference.final_check_count) nativeParcel=true fullReaders=16"
$valid = @($provider, $profile) + $records + @($complete)
$checks = 0
Assert-WifiChannelBlockingValuesParcelEvidence -Output $valid -Reference $Reference
$checks++
function Reject([string[]] $Lines, [string] $Path = $Reference) {
  $failed = $false
  try { Assert-WifiChannelBlockingValuesParcelEvidence -Output $Lines -Reference $Path }
  catch { $failed = $true }
  if (-not $failed) { throw 'Incomplete channel-blocking native reader evidence was accepted' }
  $script:checks++
}
Reject @()
Reject @($provider, $profile, $complete)
Reject $valid[1..18]
Reject (@($provider) + $valid[2..18])
Reject $valid[0..17]
Reject (@($valid) + $records[0])
Reject (@($valid) + 'WIFI_CHANNEL_BLOCKING_PARCEL incomplete')
Reject (@($valid) + 'WIFI_CHANNEL_BLOCKING_READER_PENDING response_bytes')
Reject (@($valid) + 'WIFI_CHANNEL_BLOCKING_WRITER_ONLY response_bytes')
Reject (@($valid) + 'WIFI_CHANNEL_BLOCKING_PROFILE unqualified')
Reject (@($valid) + $provider)
Reject (@($valid) + $complete)
$bad = @($valid); $bad[2] = $bad[2].Replace('roundtrip=true', 'roundtrip=false'); Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' bytes=[0-9]+', ' bytes=1'; Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' sha256=[0-9a-f]{64}', (' sha256=' + ('0' * 64)); Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' rewrite_sha256=[0-9a-f]{64}', (' rewrite_sha256=' + ('0' * 64)); Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' reader=\S+', ' reader=java.lang.Object'; Reject $bad
$bad = @($valid); $bad[2] = $records[1]; Reject $bad
$bad = @($valid); $bad[2] = $bad[2].Replace('channel_sta20', 'Channel_sta20'); Reject $bad
$bad = @($valid); $bad[2] = $records[1]; $bad[3] = $records[0]; Reject $bad
$bad = @($valid); $bad[18] = $complete.Replace('nativeParcel=true', 'nativeParcel=false'); Reject $bad
$bad = @($valid); $bad[18] = $complete.Replace('fullReaders=16', 'fullReaders=6'); Reject $bad
$bad = @($valid); $bad[18] = $complete -creplace ' checks=[0-9]+', ' checks=1'; Reject $bad
Reject (@($provider, $profile, $complete) + $records)
Reject (@($records) + @($provider, $profile, $complete))
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('channel-blocking-wire-gate-' + [guid]::NewGuid().ToString('N') + '.json')
try {
  foreach ($field in @('profile', 'aosp_wifi_commit', 'sdk_artifact_sha256')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
    $wrong.wire_reference.$field = 'unqualified'
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid $temporary
  }
  foreach ($field in @('sdk_declarations', 'full_reader_cases', 'final_check_count', 'writer_only_cases')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
    $wrong.wire_reference.$field = 1
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid $temporary
  }
  foreach ($wrongBoolean in @($false, 'true')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
    $wrong.wire_reference.full_original_readers = $wrongBoolean
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid $temporary
  }
  foreach ($field in @('case', 'reader', 'sha256', 'rewrite_sha256')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
    $wrong.wire_cases[15].$field = 'unqualified'
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid $temporary
  }
  foreach ($wrongBoolean in @($false, 'true')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
    $wrong.wire_cases[0].roundtrip = $wrongBoolean
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid $temporary
  }
  $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
  $wrong.wire_cases[15] = $wrong.wire_cases[0]
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
  Reject $valid $temporary
} finally {
  if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
"Wi-Fi channel/blocking wire-evidence checker passed: $checks checks"
