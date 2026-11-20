# Full production reader/writer qualification, compared with genuine native
# Parcel bytes from the independent pinned AOSP Android16-r4/bp4a value graph.
function Assert-WifiP2pValuesParcelEvidence([string[]] $Output, [string] $Reference) {
  $wire = Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json
  if ($wire.wire_reference.profile -cne 'android16-r4' -or
      $wire.wire_reference.aosp_wifi_commit -cne 'e3f09203409da7a91d2c5cf9f0109da6d5243319' -or
      $wire.wire_reference.release_profile -cne 'bp4a') {
    throw 'The required independent Android16-r4/bp4a reference is missing'
  }
  $expectedNames = @('config_default', 'config_peer', 'config_unicode',
    'device_default', 'device_unicode', 'device_null_fields',
    'devicelist_empty', 'devicelist_public_two', 'group_default', 'group_default_owner_copy',
    'info_default', 'info_ipv4', 'info_ipv6', 'wfd_default', 'wfd_configured',
    'pairing_unicode', 'pairing_opportunistic', 'pairing_pin', 'config_pairing',
    'device_full_ipv4', 'device_full_ipv6', 'devicelist_two', 'group_full')
  $expected = @($wire.wire_cases)
  $text = $Output -join [Environment]::NewLine
  $options = [Text.RegularExpressions.RegexOptions]::Multiline
  $providers = [regex]::Matches($text,
    '^P2P_VALUES_PROVIDERS coherent production boot graph; Parcel=null-loader Device identity verified\r?$', $options)
  $public = [regex]::Matches($text,
    '^WifiP2pValuesContractTest passed: [1-9][0-9]* checks; parcel=true; profile=android16-r4; bootCoherentP2p=true\r?$', $options)
  $extended = [regex]::Matches($text,
    '^WifiP2pValuesExtendedTest passed: [1-9][0-9]* checks; parcel=true; bootCoherentP2p=true\r?$', $options)
  $observed = [regex]::Matches($text,
    '^P2P_VALUES_PARCEL (\S+) bytes=([0-9]+) sha256=([0-9a-f]{64}) roundtrip=true\r?$', $options)
  $markers = [regex]::Matches($text, '^P2P_VALUES_PARCEL(?:\s|$)', $options)
  if ($expected.Count -ne 23 -or $observed.Count -ne 23 -or $markers.Count -ne 23) {
    throw 'Expected twenty-three complete P2P native reader/writer observations'
  }
  if ($providers.Count -ne 1 -or $public.Count -ne 1 -or $extended.Count -ne 1 -or
      $providers[0].Index -ge $observed[0].Index -or
      $public[0].Index -le $observed[18].Index -or
      $extended[0].Index -le $observed[22].Index -or $text.Contains('P2P_VALUES_READER_PENDING')) {
    throw 'Production boot identities and complete native P2P readers must be verified'
  }
  foreach ($name in $expectedNames) {
    $items = @($expected | Where-Object { $_.case -ceq $name })
    if ($items.Count -ne 1 -or [long]$items[0].bytes -le 0 -or
        $items[0].sha256 -cnotmatch '^[0-9a-f]{64}$') {
      throw "Missing, duplicate or invalid reference case: $name"
    }
  }
  $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach ($item in $observed) {
    $name = $item.Groups[1].Value
    if (-not $seen.Add($name)) { throw "Duplicate P2P native Parcel observation: $name" }
    $match = @($expected | Where-Object { $_.case -ceq $name })
    if ($match.Count -ne 1 -or [long]$item.Groups[2].Value -ne [long]$match[0].bytes -or
        $item.Groups[3].Value -cne $match[0].sha256) {
      throw "P2P native Parcel differs from pinned Android16-r4: $name"
    }
  }
}
