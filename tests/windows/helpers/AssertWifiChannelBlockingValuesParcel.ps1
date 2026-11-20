# Exact complete-reader evidence for the pinned two-file channel/blocking value graph.
# The final shared counter is printed after both public and hidden suites finish.
function Assert-WifiChannelBlockingValuesParcelEvidence([string[]] $Output, [string] $Reference) {
  $referenceData = Get-Content -LiteralPath $Reference -Raw -Encoding UTF8 | ConvertFrom-Json
  $profile = $referenceData.wire_reference
  if ($profile.profile -cne 'android16-r4' -or
      $profile.aosp_wifi_commit -cne 'e3f09203409da7a91d2c5cf9f0109da6d5243319' -or
      $profile.sdk_artifact_sha256 -cne 'd9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a' -or
      $profile.sdk_declarations -ne 27 -or
      $profile.full_original_readers -isnot [bool] -or $profile.full_original_readers -ne $true -or
      $null -eq $profile.writer_only_cases -or $profile.writer_only_cases -ne 0 -or
      $profile.final_check_count -le 0 -or $profile.full_reader_cases -ne 16) {
    throw 'The required complete Android16-r4 channel/blocking reader reference is missing'
  }
  $expectedNames = @('channel_sta20', 'channel_all20', 'channel_direct20', 'blocking_min_network',
    'blocking_min_bssid', 'blocking_max_network', 'blocking_max_bssid', 'blocking_snapshot',
    'channel_width40', 'channel_width80', 'channel_width160', 'channel_width80p80', 'channel_width320',
    'blocking_direct_network', 'blocking_direct_bssid', 'blocking_parcel_constructor')
  $channel = 'android.net.wifi.WifiAvailableChannel'
  $blocking = 'android.net.wifi.BlockingOption'
  $expectedReaders = @($channel, $channel, $channel, $blocking, $blocking, $blocking, $blocking, $blocking,
    $channel, $channel, $channel, $channel, $channel, $blocking, $blocking, $blocking)
  $text = $Output -join [Environment]::NewLine
  $options = [Text.RegularExpressions.RegexOptions]::Multiline
  $providers = [regex]::Matches($text,
    '^WIFI_CHANNEL_BLOCKING_PROVIDERS coherent production boot graph; complete original readers\r?$', $options)
  $profiles = [regex]::Matches($text,
    '^WIFI_CHANNEL_BLOCKING_PROFILE android16-r4; sdk36; declarations=27\r?$', $options)
  $complete = [regex]::Matches($text,
    '^WIFI_CHANNEL_BLOCKING_VALUES: PASS checks=([1-9][0-9]*) nativeParcel=true fullReaders=16\r?$', $options)
  $observed = [regex]::Matches($text,
    '^WIFI_CHANNEL_BLOCKING_PARCEL (\S+) bytes=([0-9]+) sha256=([0-9a-f]{64}) rewrite_sha256=([0-9a-f]{64}) roundtrip=true reader=(\S+)\r?$', $options)
  $markers = [regex]::Matches($text, '^WIFI_CHANNEL_BLOCKING_PARCEL(?:\s|$)', $options)
  $expected = @($referenceData.wire_cases)
  if ($expected.Count -ne 16 -or $observed.Count -ne 16 -or $markers.Count -ne 16) {
    throw 'Expected sixteen complete channel/blocking native reader/writer/rewrite observations'
  }
  if ($providers.Count -ne 1 -or $profiles.Count -ne 1 -or $complete.Count -ne 1 -or
      [regex]::Matches($text, '^WIFI_CHANNEL_BLOCKING_PROVIDERS(?:\s|$)', $options).Count -ne 1 -or
      [regex]::Matches($text, '^WIFI_CHANNEL_BLOCKING_PROFILE(?:\s|$)', $options).Count -ne 1 -or
      [regex]::Matches($text, '^WIFI_CHANNEL_BLOCKING_VALUES:', $options).Count -ne 1 -or
      $providers[0].Index -ge $profiles[0].Index -or $profiles[0].Index -ge $observed[0].Index -or
      $complete[0].Index -le $observed[15].Index -or
      [long]$complete[0].Groups[1].Value -ne [long]$profile.final_check_count -or
      $text.Contains('WIFI_CHANNEL_BLOCKING_READER_PENDING') -or $text.Contains('WIFI_CHANNEL_BLOCKING_WRITER_ONLY')) {
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
      throw "Channel/blocking native Parcel initial/rewrite bytes or original reader differ from pinned Android16-r4: $name"
    }
  }
}
