# JNI/APK paths are required by 44-native2's new ifaddrs part in both ABIs
# and both app modes. This standalone ELF uses an independent Windows snapshot.
$oracleRows = [Collections.Generic.List[string]]::new()
foreach ($adapter in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
  if ($adapter.OperationalStatus -ne [Net.NetworkInformation.OperationalStatus]::Up) { continue }
  $properties = $adapter.GetIPProperties()
  $index = 0
  try { $index = $properties.GetIPv4Properties().Index } catch { }
  if ($index -eq 0) { try { $index = $properties.GetIPv6Properties().Index } catch { } }
  if ($index -le 0) { throw 'An active Windows interface has no index for the Net4 oracle' }
  $hardware = $adapter.GetPhysicalAddress().ToString().ToLowerInvariant()
  if (-not $hardware) { $hardware = '-' }
  $oracleRows.Add("P $index $hardware")
  foreach ($unicast in $properties.UnicastAddresses) {
    $address = $unicast.Address
    $scope = 0
    if ($address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork) { $family = 2 }
    elseif ($address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetworkV6) {
      $family = 10
      $scope = $address.ScopeId
    } else { continue }
    # Strip the textual %scope, while checking its real numeric value separately.
    $numeric = [Net.IPAddress]::new($address.GetAddressBytes()).ToString()
    $oracleRows.Add("A $index $family $($unicast.PrefixLength) $numeric $scope")
  }
}
$oracle = Join-Path $env:RUNNER_TEMP "net4-ifaddrs-$([guid]::NewGuid()).txt"
[IO.File]::WriteAllLines($oracle, $oracleRows, [Text.UTF8Encoding]::new($false))
try {
  foreach ($abi in 'x86_64', 'arm64-v8a') {
    $libraryPath = NoDrive "$n2/$abi"
    $out = N2Run "$n2Dir/gesher-exec.exe" @('--elf', "$n2/$abi/net4_ifaddrs_test", (NoDrive $oracle)) `
      (Get-Location).Path @{ LD_LIBRARY_PATH = $libraryPath } 90 | Out-String
    $code = $LASTEXITCODE
    Write-Host $out
    if ($code -eq 0 -and $out -notmatch '(?m)^FAIL' -and
        $out -match 'net4 ifaddrs: all passed; .*host oracle ([0-9]+)' -and [int]$Matches[1] -ge 3) {
      Write-Host "PASS  net4 ifaddrs ($abi): standalone ELF matches actual Windows addresses, prefixes, scopes, hardware/index and independent allocation lifetime"
    } else {
      Write-Host "FAIL  net4 ifaddrs ($abi): standalone ELF/oracle (exit $code)"
      $script:failed++
    }
  }
} finally {
  Remove-Item -LiteralPath $oracle -ErrorAction SilentlyContinue
}
