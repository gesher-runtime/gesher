# Exact pinned AOSP r4 writer observations on Android native Parcel.
function Assert-WifiNetworkSpecifierParcelEvidence([string[]] $Output, [string] $Reference) {
  $specifierReference = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  if ($specifierReference.wire_profile -cne 'aosp-wifi-android-16.0.0_r4-pinned-writers-on-api36' -or
      $specifierReference.aosp_wifi_commit -cne 'e3f09203409da7a91d2c5cf9f0109da6d5243319') {
    throw 'Expected the pinned Android Wi-Fi r4 writer profile'
  }
  $specifierExpected = @($specifierReference.wire_cases)
  $specifierObserved = [regex]::Matches(($Output -join "`n"), '^WIFI_SPECIFIER_PARCEL (\S+) bytes=([0-9]+) sha256=([0-9a-f]{64})\r?$', [Text.RegularExpressions.RegexOptions]::Multiline)
  if ($specifierExpected.Count -ne 12 -or $specifierObserved.Count -ne 12) { throw 'Expected twelve complete native Parcel observations' }
  $specifierSeen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach ($specifierCase in $specifierObserved) {
    $specifierName = $specifierCase.Groups[1].Value
    if (-not $specifierSeen.Add($specifierName)) { throw "Duplicate native Parcel observation: $specifierName" }
    $specifierMatch = @($specifierExpected | Where-Object { $_.case -ceq $specifierName })
    if ($specifierMatch.Count -ne 1 -or [long]$specifierCase.Groups[2].Value -ne [long]$specifierMatch[0].bytes -or
        $specifierCase.Groups[3].Value -cne $specifierMatch[0].sha256) { throw "Native Parcel differs from pinned AOSP r4 reference: $specifierName" }
  }
}
