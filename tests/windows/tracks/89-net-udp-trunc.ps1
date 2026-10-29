# native2 is obtained by ci.yml's current-run/attempt/ZIP-digest receipt.
# This additionally ties exact ELF members and the independently executed Linux
# rows to this source, compiler and ABI. No PowerShell socket semantics oracle.
try {
  & python (Join-Path $PSScriptRoot '../../../tools/net-udp/payload.py') verify $n2
  if ($LASTEXITCODE -ne 0) { throw 'UDP source/run/attempt/roles/digest verification failed' }
  $reference = [IO.File]::ReadAllText((Join-Path $n2 'net-udp/linux-reference.txt')).Replace("`r`n", "`n").Trim()
  foreach ($abi in 'x86_64', 'arm64-v8a') {
    $environment = @{ GESHER_INET='1'; LD_LIBRARY_PATH=(NoDrive "$n2/$abi") }
    $arguments = @('--elf', "$n2/$abi/net_udp_trunc", 'net_udp_trunc')
    $actual = Aot2Run "$n2Dir/gesher-exec.exe" $arguments $environment 90
    Write-Host $actual.Out
    $rows = (@($actual.Out.Replace("`r`n", "`n").Split("`n") | Where-Object { $_.StartsWith('UDP ') }) -join "`n").Trim()
    if ($actual.Exit -ne 0 -or $actual.Out -match 'FAIL UDP:' -or $rows -cne $reference) {
      throw "UDP $abi actual bionic API results differ from executed Linux reference (exit $($actual.Exit))"
    }
    Write-Host "PASS  UDP ${abi}: 192 Linux-identical actual rows; source addresses/scatter/peek/zero; 128 alias packets; IP maxima/iov bounds; stream control"
    $negative = Aot2Run "$n2Dir/gesher-exec.exe" ($arguments + '--negative') $environment 30
    Write-Host $negative.Out
    if ($negative.Exit -eq 0 -or -not $negative.Out.Contains('FAIL UDP: negative missing MSG_TRUNC rejected')) {
      throw "UDP $abi missing-truncation negative fixture was not rejected"
    }
    Write-Host "PASS  UDP ${abi}: actual missing-MSG_TRUNC negative rejected"
    $faultReference = [IO.File]::ReadAllText((Join-Path $n2 'net-udp/linux-fault-reference.txt')).Replace("`r`n", "`n").Trim()
    $fault = Aot2Run "$n2Dir/gesher-exec.exe" @('--elf', "$n2/$abi/net_udp_fault", 'net_udp_fault') $environment 90
    Write-Host $fault.Out
    $faultRows = (@($fault.Out.Replace("`r`n", "`n").Split("`n") | Where-Object { $_.StartsWith('UDPFAULT ') }) -join "`n").Trim()
    if ($fault.Exit -ne 0 -or $faultRows -cne $faultReference) {
      throw "UDP $abi fault/metadata output differs from actual Linux (exit $($fault.Exit))"
    }
    Write-Host "PASS  UDP ${abi}: 120 actual Linux-identical EFAULT/partial-scatter/queue/metadata/cross-page rows"
  }
} catch {
  Write-Host "FAIL  UDP truncation contract: $_"
  $script:failed++
}
