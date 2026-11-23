# Whole native Parcel evidence from an independent pinned AOSP Android16-r4 writer.
function Assert-SoftApValuesParcelEvidence([string[]] $Output, [string] $Reference) {
  $wire = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  if ($wire.wire_reference.profile -cne 'android16-r4' -or
      $wire.wire_reference.aosp_wifi_commit -cne 'e3f09203409da7a91d2c5cf9f0109da6d5243319') {
    throw 'The required independent Android16-r4 reference is missing'
  }
  $expectedNames = @('softap_default', 'softap_single6', 'softap_pair', 'softap_composite',
    'softap_full', 'softap_binary_ssid', 'softap_owe', 'softap_vendor',
    'wps_default', 'wps_null_copy', 'wps_full', 'wps_unchecked')
  $expected = @($wire.wire_cases)
  $observed = [regex]::Matches(($Output -join [Environment]::NewLine),
    '^SOFTAP_VALUES_PARCEL (\S+) bytes=([0-9]+) sha256=([0-9a-f]{64})\r?$',
    [Text.RegularExpressions.RegexOptions]::Multiline)
  $markers = [regex]::Matches(($Output -join [Environment]::NewLine),
    '^SOFTAP_VALUES_PARCEL(?:\s|$)', [Text.RegularExpressions.RegexOptions]::Multiline)
  if ($expected.Count -ne 12 -or $observed.Count -ne 12 -or $markers.Count -ne 12) {
    throw 'Expected twelve complete SoftAP/WPS native Parcel observations'
  }
  foreach ($name in $expectedNames) {
    if (@($expected | Where-Object { $_.case -ceq $name }).Count -ne 1) {
      throw "Missing or duplicate reference case: $name"
    }
  }
  $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach ($item in $observed) {
    $name = $item.Groups[1].Value
    if (-not $seen.Add($name)) { throw "Duplicate SoftAP/WPS native Parcel observation: $name" }
    $match = @($expected | Where-Object { $_.case -ceq $name })
    if ($match.Count -ne 1 -or [long]$item.Groups[2].Value -ne [long]$match[0].bytes -or
        $item.Groups[3].Value -cne $match[0].sha256) {
      throw "SoftAP/WPS native Parcel differs from pinned Android16-r4: $name"
    }
  }
}
