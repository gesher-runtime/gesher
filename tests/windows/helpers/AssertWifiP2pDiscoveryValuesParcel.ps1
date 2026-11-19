# Exact complete-reader evidence for the pinned five-file discovery value graph.
# The final shared counter is printed after both public and hidden suites finish.
function Assert-WifiP2pDiscoveryValuesParcelEvidence([string[]] $Output, [string] $Reference) {
  $referenceData = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
  $profile = $referenceData.wire_reference
  if ($profile.profile -cne 'android16-r4' -or
      $profile.aosp_wifi_commit -cne 'e3f09203409da7a91d2c5cf9f0109da6d5243319' -or
      $profile.aosp_modules_utils_commit -cne 'e945bbce0ccb83e1111b6031a3bba89e6b5cac6f' -or
      $profile.sdk_artifact_sha256 -cne 'd9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a' -or
      $profile.sdk_declarations -ne 41 -or
      $profile.full_original_readers -isnot [bool] -or $profile.full_original_readers -ne $true -or
      $null -eq $profile.writer_only_cases -or $profile.writer_only_cases -ne 0 -or
      $profile.final_check_count -le 0 -or $profile.full_reader_cases -ne 17) {
    throw 'The required complete Android16-r4 discovery reader reference is missing'
  }
  $expectedNames = @('discovery_full', 'discovery_social', 'discovery_single', 'discovery_full_frequency',
    'advertisement_default', 'advertisement_explicit', 'usd_discovery_default', 'usd_discovery_multiple',
    'usd_discovery_maximum', 'dir_zero', 'dir_bytes', 'dir_aliased', 'discovery_vendor_one',
    'discovery_vendor_duplicate_oui', 'response_null', 'response_empty', 'response_bytes')
  $p2p = 'android.net.wifi.p2p.'
  $expectedReaders = @(
    ($p2p + 'WifiP2pDiscoveryConfig'), ($p2p + 'WifiP2pDiscoveryConfig'),
    ($p2p + 'WifiP2pDiscoveryConfig'), ($p2p + 'WifiP2pDiscoveryConfig'),
    ($p2p + 'WifiP2pUsdBasedLocalServiceAdvertisementConfig'), ($p2p + 'WifiP2pUsdBasedLocalServiceAdvertisementConfig'),
    ($p2p + 'WifiP2pUsdBasedServiceDiscoveryConfig'), ($p2p + 'WifiP2pUsdBasedServiceDiscoveryConfig'),
    ($p2p + 'WifiP2pUsdBasedServiceDiscoveryConfig'), ($p2p + 'WifiP2pDirInfo'),
    ($p2p + 'WifiP2pDirInfo'), ($p2p + 'WifiP2pDirInfo'),
    ($p2p + 'WifiP2pDiscoveryConfig'), ($p2p + 'WifiP2pDiscoveryConfig'),
    ($p2p + 'nsd.WifiP2pUsdBasedServiceResponse'), ($p2p + 'nsd.WifiP2pUsdBasedServiceResponse'),
    ($p2p + 'nsd.WifiP2pUsdBasedServiceResponse'))
  $text = $Output -join [Environment]::NewLine
  $options = [Text.RegularExpressions.RegexOptions]::Multiline
  $providers = [regex]::Matches($text,
    '^P2P_DISCOVERY_PROVIDERS coherent production boot graph; complete original readers\r?$', $options)
  $profiles = [regex]::Matches($text,
    '^P2P_DISCOVERY_PROFILE android16-r4; sdk36; declarations=41\r?$', $options)
  $complete = [regex]::Matches($text,
    '^P2P_DISCOVERY_VALUES: PASS checks=([1-9][0-9]*) nativeParcel=true fullReaders=17 modern=true\r?$', $options)
  $observed = [regex]::Matches($text,
    '^P2P_DISCOVERY_PARCEL (\S+) bytes=([0-9]+) sha256=([0-9a-f]{64}) rewrite_sha256=([0-9a-f]{64}) roundtrip=true reader=(\S+)\r?$', $options)
  $markers = [regex]::Matches($text, '^P2P_DISCOVERY_PARCEL(?:\s|$)', $options)
  $expected = @($referenceData.wire_cases)
  if ($expected.Count -ne 17 -or $observed.Count -ne 17 -or $markers.Count -ne 17) {
    throw 'Expected seventeen complete discovery native reader/writer/rewrite observations'
  }
  if ($providers.Count -ne 1 -or $profiles.Count -ne 1 -or $complete.Count -ne 1 -or
      [regex]::Matches($text, '^P2P_DISCOVERY_PROVIDERS(?:\s|$)', $options).Count -ne 1 -or
      [regex]::Matches($text, '^P2P_DISCOVERY_PROFILE(?:\s|$)', $options).Count -ne 1 -or
      [regex]::Matches($text, '^P2P_DISCOVERY_VALUES:', $options).Count -ne 1 -or
      $providers[0].Index -ge $profiles[0].Index -or $profiles[0].Index -ge $observed[0].Index -or
      $complete[0].Index -le $observed[16].Index -or
      [long]$complete[0].Groups[1].Value -ne [long]$profile.final_check_count -or
      $text.Contains('P2P_DISCOVERY_READER_PENDING') -or $text.Contains('P2P_DISCOVERY_WRITER_ONLY')) {
    throw 'Coherent production providers, exact profile and the final complete native suite are required'
  }
  for ($i = 0; $i -lt $expectedNames.Count; $i++) {
    $name = $expectedNames[$i]
    $referenceCase = $expected[$i]
    if ($referenceCase.case -cne $name -or $referenceCase.reader -cne $expectedReaders[$i] -or
        $referenceCase.roundtrip -isnot [bool] -or $referenceCase.roundtrip -ne $true -or
        [long]$referenceCase.bytes -le 0 -or $referenceCase.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $referenceCase.rewrite_sha256 -cne $referenceCase.sha256) {
      throw "Missing, out-of-order or invalid full-reader reference case: $name"
    }
    $item = $observed[$i]
    if ($item.Groups[1].Value -cne $name -or [long]$item.Groups[2].Value -ne [long]$referenceCase.bytes -or
        $item.Groups[3].Value -cne $referenceCase.sha256 -or $item.Groups[4].Value -cne $referenceCase.rewrite_sha256 -or
        $item.Groups[5].Value -cne $referenceCase.reader) {
      throw "Discovery native Parcel initial/rewrite bytes or original reader differ from pinned Android16-r4: $name"
    }
  }
}
