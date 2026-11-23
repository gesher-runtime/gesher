# Gate logic only. Constructed log lines below do not qualify native execution.
param([string] $Reference = (Join-Path $PSScriptRoot '../../docs/research/wifi-p2p-nsd-values-android-reference-2026-11-18.json'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/helpers/AssertWifiP2pNsdValuesParcel.ps1"
$value = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
$records = @($value.wire_cases | ForEach-Object {
  "P2P_NSD_PARCEL $($_.case) bytes=$($_.bytes) sha256=$($_.sha256) rewrittenBytes=$($_.rewritten_bytes) rewrittenSha256=$($_.rewritten_sha256) roundtrip=true"
})
$provider = 'P2P_NSD_PROVIDERS coherent production boot graph; complete original readers'
$profile = 'P2P_NSD_PROFILE android16-r4; bp4a; sdk36; declarations=40'
$public = 'WifiP2pNsdValuesContractTest passed: 1 checks; parcel=true; usd=true'
$extended = 'WifiP2pNsdValuesExtendedTest passed: 1 checks; parcel=true; usd=true'
$valid = @($provider, $profile) + $records[0..4] + @($public) + $records[5..19] + @($extended)
$checks = 0
Assert-WifiP2pNsdValuesParcelEvidence -Output $valid -Reference $Reference
$checks++
function Reject([string[]] $Lines, [string] $Path = $Reference) {
  $failed = $false
  try { Assert-WifiP2pNsdValuesParcelEvidence -Output $Lines -Reference $Path }
  catch { $failed = $true }
  if (-not $failed) { throw 'Incomplete NSD reader/writer/rewrite evidence was accepted' }
  $script:checks++
}
Reject @()
Reject @($provider, $profile, $public, $extended)
Reject $valid[1..23]
Reject (@($provider) + $valid[2..23])
Reject (@($valid) + $records[0])
Reject (@($valid) + 'P2P_NSD_PARCEL incomplete')
Reject (@($valid) + 'P2P_NSD_READER_PENDING info_usd')
Reject (@($valid) + 'P2P_NSD_WRITER_ONLY info_usd')
Reject (@($valid) + 'P2P_NSD_PROFILE unqualified')
$bad = @($valid); $bad[2] = $bad[2].Replace('roundtrip=true', 'roundtrip=false'); Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' bytes=[0-9]+', ' bytes=1'; Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' sha256=[0-9a-f]{64}', (' sha256=' + ('0' * 64)); Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' rewrittenBytes=[0-9]+', ' rewrittenBytes=1'; Reject $bad
$bad = @($valid); $bad[2] = $bad[2] -creplace ' rewrittenSha256=[0-9a-f]{64}', (' rewrittenSha256=' + ('0' * 64)); Reject $bad
$bad = @($valid); $bad[2] = $records[1]; Reject $bad
$bad = @($valid); $bad[2] = $bad[2].Replace('usd_config_noarg', 'Usd_config_noarg'); Reject $bad
$bad = @($valid); $bad[7] = $public.Replace('usd=true', 'usd=false'); Reject $bad
$bad = @($valid); $bad[23] = $extended.Replace('parcel=true', 'parcel=false'); Reject $bad
Reject (@($provider, $profile, $public) + $records + @($extended))
Reject (@($records) + @($provider, $profile, $public, $extended))
$bad = @($valid); $bad[8] = $records[6]; $bad[9] = $records[5]; Reject $bad
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('p2p-nsd-wire-gate-' + [guid]::NewGuid().ToString('N') + '.json')
try {
  foreach ($field in @('profile', 'release_profile', 'aosp_wifi_commit', 'aosp_connectivity_commit')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
    $wrong.wire_reference.$field = 'unqualified'
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid $temporary
  }
  foreach ($field in @('full_original_readers', 'writer_only_cases')) {
    $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($field -eq 'full_original_readers') { $wrong.wire_reference.$field = $false }
    else { $wrong.wire_reference.$field = 1 }
    $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Reject $valid $temporary
  }
  $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
  $wrong.wire_cases[19].rewritten_sha256 = $wrong.wire_cases[19].sha256
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
  Reject $valid $temporary
  $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
  $wrong.wire_cases[19] = $wrong.wire_cases[0]
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
  Reject $valid $temporary
  $wrong = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
  $wrong.wire_cases[0].roundtrip = $false
  $wrong | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
  Reject $valid $temporary
} finally {
  if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
"P2P NSD wire-evidence checker passed: $checks checks"
