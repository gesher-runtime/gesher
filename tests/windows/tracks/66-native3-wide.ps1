# The NDK libraries run the same independent-reference contract assertions
# through actual shim entries in test44. Reuse their complete process logs;
# running these Activities a second time would add no ABI or mode coverage.
foreach ($wideAbi in 'x86_64', 'arm64') {
  foreach ($wideMode in 'dalvikvm', 'single', 'device') {
    $wideRecords = @($n2WideEvidence | Where-Object { $_.Abi -eq $wideAbi -and $_.Mode -eq $wideMode })
    if ($wideRecords.Count -eq 1 -and $wideRecords[0].Exit -eq 0 -and
        $wideRecords[0].Report.Contains('native2 wide: ok; pinned UTF8/16/32 state, errors, bounds and LP64 integers; wide locale aliases, floating parsing, C collation and UTF32 time')) {
      Write-Host "PASS  native3-wide ($wideAbi, $wideMode): pinned multibyte/C11 states, bounds, LP64 numbers, locale aliases, C collation and UTF32 time"
    } else {
      Write-Host "FAIL  native3-wide ($wideAbi, $wideMode): actual NDK process did not verify every wide contract"
      foreach ($wideRecord in $wideRecords) {
        Write-Host "exit $($wideRecord.Exit)"
        $wideRecord.Report -split "`r?`n" | Where-Object { $_ -match 'native2 wide:|Exception|Error|FAILED' } | Write-Host
      }
      $failed++
    }
  }
}
