# Full original-reader qualification for the pinned Android16-r4/bp4a value
# profile. Both the initial write and the reader's rewrite are exact evidence.
function Assert-WifiP2pNsdValuesParcelEvidence([string[]] $Output, [string] $Reference) {
  $wire = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
  if ($wire.wire_reference.profile -cne 'android16-r4' -or
      $wire.wire_reference.release_profile -cne 'bp4a' -or
      $wire.wire_reference.aosp_wifi_commit -cne 'e3f09203409da7a91d2c5cf9f0109da6d5243319' -or
      $wire.wire_reference.aosp_connectivity_commit -cne 'f930245ec39a510f37a9a7dfaded96d287491a61' -or
      $wire.wire_reference.full_original_readers -ne $true -or
      $null -eq $wire.wire_reference.writer_only_cases -or
      $wire.wire_reference.writer_only_cases -ne 0) {
    throw 'The required complete Android16-r4/bp4a NSD reader reference is missing'
  }
  $expectedNames = @('usd_config_noarg', 'usd_config_generic', 'usd_config_null_info',
    'usd_config_empty_info', 'usd_config_bytes_info', 'request_all', 'request_vendor_upper',
    'request_empty', 'request_dns_all', 'request_dns_ptr', 'request_dns_txt',
    'request_upnp_all', 'request_upnp_target', 'info_dns_empty', 'info_dns_txt',
    'info_upnp_base', 'info_upnp_services', 'info_list', 'request_usd', 'info_usd')
  $expected = @($wire.wire_cases)
  $text = $Output -join [Environment]::NewLine
  $options = [Text.RegularExpressions.RegexOptions]::Multiline
  $providers = [regex]::Matches($text,
    '^P2P_NSD_PROVIDERS coherent production boot graph; complete original readers\r?$', $options)
  $profiles = [regex]::Matches($text,
    '^P2P_NSD_PROFILE android16-r4; bp4a; sdk36; declarations=40\r?$', $options)
  $public = [regex]::Matches($text,
    '^WifiP2pNsdValuesContractTest passed: [1-9][0-9]* checks; parcel=true; usd=true\r?$', $options)
  $extended = [regex]::Matches($text,
    '^WifiP2pNsdValuesExtendedTest passed: [1-9][0-9]* checks; parcel=true; usd=true\r?$', $options)
  $observed = [regex]::Matches($text,
    '^P2P_NSD_PARCEL (\S+) bytes=([0-9]+) sha256=([0-9a-f]{64}) rewrittenBytes=([0-9]+) rewrittenSha256=([0-9a-f]{64}) roundtrip=true\r?$', $options)
  $markers = [regex]::Matches($text, '^P2P_NSD_PARCEL(?:\s|$)', $options)
  if ($expected.Count -ne 20 -or $observed.Count -ne 20 -or $markers.Count -ne 20) {
    throw 'Expected twenty complete NSD native reader/writer/rewrite observations'
  }
  if ($providers.Count -ne 1 -or $profiles.Count -ne 1 -or $public.Count -ne 1 -or $extended.Count -ne 1 -or
      [regex]::Matches($text, '^P2P_NSD_PROVIDERS(?:\s|$)', $options).Count -ne 1 -or
      [regex]::Matches($text, '^P2P_NSD_PROFILE(?:\s|$)', $options).Count -ne 1 -or
      $providers[0].Index -ge $profiles[0].Index -or $profiles[0].Index -ge $observed[0].Index -or
      $public[0].Index -le $observed[4].Index -or $public[0].Index -ge $observed[5].Index -or
      $extended[0].Index -le $observed[19].Index -or
      $text.Contains('P2P_NSD_READER_PENDING') -or $text.Contains('P2P_NSD_WRITER_ONLY')) {
    throw 'Production providers, exact profile and both complete native NSD readers must be verified'
  }
  for ($i = 0; $i -lt $expectedNames.Count; $i++) {
    $name = $expectedNames[$i]
    $referenceCase = $expected[$i]
    if ($referenceCase.case -cne $name -or $referenceCase.roundtrip -ne $true -or
        [long]$referenceCase.bytes -le 0 -or [long]$referenceCase.rewritten_bytes -ne [long]$referenceCase.bytes -or
        $referenceCase.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $referenceCase.rewritten_sha256 -cnotmatch '^[0-9a-f]{64}$') {
      throw "Missing, out-of-order or invalid full-reader reference case: $name"
    }
    if (($name -ceq 'info_usd') -eq ($referenceCase.sha256 -ceq $referenceCase.rewritten_sha256)) {
      throw "Unexpected initial/rewrite relation in reference case: $name"
    }
    $item = $observed[$i]
    if ($item.Groups[1].Value -cne $name -or
        [long]$item.Groups[2].Value -ne [long]$referenceCase.bytes -or
        $item.Groups[3].Value -cne $referenceCase.sha256 -or
        [long]$item.Groups[4].Value -ne [long]$referenceCase.rewritten_bytes -or
        $item.Groups[5].Value -cne $referenceCase.rewritten_sha256) {
      throw "NSD native Parcel initial or rewritten bytes differ from pinned Android16-r4: $name"
    }
  }
}
