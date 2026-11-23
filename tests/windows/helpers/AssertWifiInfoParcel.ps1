# Whole native Parcel evidence from the same SDK36 corpus on actual Android.
function Assert-WifiInfoParcelEvidence([string[]] $Output, [string] $Reference) {
  $wifiWireReference = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  $wifiWireExpected = @($wifiWireReference.wire_cases)
  $wifiWireObserved = [regex]::Matches(($Output -join "`n"), '^WIFI_INFO_PARCEL (\S+) bytes=([0-9]+) sha256=([0-9a-f]{64})\r?$', [Text.RegularExpressions.RegexOptions]::Multiline)
  if ($wifiWireExpected.Count -ne 9 -or $wifiWireObserved.Count -ne 9) { throw 'Expected nine complete native Parcel observations' }
  $wifiWireSeen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach ($wifiWireCase in $wifiWireObserved) {
    $wifiWireName = $wifiWireCase.Groups[1].Value
    if (-not $wifiWireSeen.Add($wifiWireName)) { throw "Duplicate native Parcel observation: $wifiWireName" }
    $wifiWireMatch = @($wifiWireExpected | Where-Object { $_.case -ceq $wifiWireName })
    if ($wifiWireMatch.Count -ne 1 -or [long]$wifiWireCase.Groups[2].Value -ne [long]$wifiWireMatch[0].bytes -or
        $wifiWireCase.Groups[3].Value -cne $wifiWireMatch[0].sha256) { throw "Native Parcel differs from Android reference: $wifiWireName" }
  }
}
