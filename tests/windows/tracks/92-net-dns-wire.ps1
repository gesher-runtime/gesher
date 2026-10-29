# Canonical full uses the exact current native2 ZIP receipt and production host/DLL.
try {
  $dnsNative = (Resolve-Path (Join-Path $Dir 'native2')).Path
  $dnsHost = Join-Path (Resolve-Path $Dir).Path 'gesher-exec.exe'
  & python (Join-Path $PSScriptRoot '../../../tools/net-dns/payload.py') verify $dnsNative
  if ($LASTEXITCODE -ne 0) { throw 'DNS typed source/attempt/compiler/member verification failed' }
  foreach ($abi in 'x86_64', 'arm64-v8a') {
    $model = if ($abi -eq 'x86_64') { 'x86_64' } else { 'arm64' }
    $arguments = @('--elf', (Join-Path $dnsNative "$abi/net_dns_wire"), 'net_dns_wire')
    foreach ($mode in 'main', 'extended', 'safety', 'negative') {
      $extra = if ($mode -eq 'main') { @() } else { @("--$mode") }
      $actual = Aot2Run $dnsHost ($arguments + $extra) @{ GESHER_INET='1' } 180
      Write-Host $actual.Out
      $rows = (@($actual.Out.Replace("`r`n", "`n").Split("`n") | Where-Object { $_.StartsWith('DNS ') }) -join "`n").Trim()
      if ($mode -eq 'negative') {
        if ($actual.Exit -ne 1 -or -not $actual.Out.Contains('FAIL DNS: negative corruption rejected') -or
            -not $actual.Out.Contains('DNS all done rows=99 failures=3')) { throw "DNS $abi corruption negative accepted" }
      } else {
        if ($actual.Exit -ne 0 -or $actual.Out.Contains('FAIL DNS:')) { throw "DNS $abi $mode failed exit=$($actual.Exit)" }
        if ($mode -eq 'safety') {
          if (-not $actual.Out.Contains('DNS safety skip cases=4 preserved=4') -or
              -not $actual.Out.Contains('DNS all done rows=100 failures=0')) { throw "DNS $abi malformed boundary/cursor controls missing" }
        } else {
          $reference = if ($mode -eq 'main') { 'linux-reference.log' } else { "aosp-extended-$model.log" }
          $expected = [IO.File]::ReadAllText((Join-Path $dnsNative "net-dns/$reference")).Replace("`r`n", "`n").Trim()
          if ($rows -cne $expected) { throw "DNS $abi $mode differs from executed independent reference" }
        }
      }
      Write-Host "PASS  DNS $abi ${mode}: actual canonical production host and matching DLL"
    }
  }
} catch {
  Write-Host "FAIL  DNS name wire contract: $_"
  $script:failed++
}
