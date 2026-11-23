# Whole native Parcel evidence from an independent pinned AOSP Android16-r4 writer.
function Assert-WifiSuggestionValuesParcelEvidence([string[]] $Output, [string] $Reference) {
  $wire = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  if ($wire.wire_reference.profile -cne 'android16-r4' -or
      $wire.wire_reference.aosp_wifi_commit -cne 'e3f09203409da7a91d2c5cf9f0109da6d5243319') {
    throw 'The required independent Android16-r4 reference is missing'
  }
  $expectedNames = @('open', 'owe', 'wpa2', 'wpa3',
    'wapi', 'enterprise', 'enterprise192', 'binary_restricted',
    'passpoint_suggestion', 'passpoint_full_suggestion', 'passpoint_empty', 'passpoint_basic',
    'passpoint_full', 'home', 'credential_empty', 'credential_user',
    'credential_certificate', 'credential_sim', 'user', 'certificate',
    'sim', 'policy', 'roaming_partner', 'update')
  $expected = @($wire.wire_cases)
  $observed = [regex]::Matches(($Output -join [Environment]::NewLine),
    '^SUGGESTION_WIRE name=(\S+) bytes=([0-9]+) sha256=([0-9a-f]{64})\r?$',
    [Text.RegularExpressions.RegexOptions]::Multiline)
  $markers = [regex]::Matches(($Output -join [Environment]::NewLine),
    '^SUGGESTION_WIRE(?:\s|$)', [Text.RegularExpressions.RegexOptions]::Multiline)
  if ($expected.Count -ne 24 -or $observed.Count -ne 24 -or $markers.Count -ne 24) {
    throw 'Expected twenty-four complete Suggestion/Passpoint native Parcel observations'
  }
  foreach ($name in $expectedNames) {
    if (@($expected | Where-Object { $_.case -ceq $name }).Count -ne 1) {
      throw "Missing or duplicate reference case: $name"
    }
  }
  $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach ($item in $observed) {
    $name = $item.Groups[1].Value
    if (-not $seen.Add($name)) { throw "Duplicate Suggestion/Passpoint native Parcel observation: $name" }
    $match = @($expected | Where-Object { $_.case -ceq $name })
    if ($match.Count -ne 1 -or [long]$item.Groups[2].Value -ne [long]$match[0].bytes -or
        $item.Groups[3].Value -cne $match[0].sha256) {
      throw "Suggestion/Passpoint native Parcel differs from pinned Android16-r4: $name"
    }
  }
}
