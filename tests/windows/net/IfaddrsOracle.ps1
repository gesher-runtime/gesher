# Match GetAdaptersAddresses(AF_UNSPEC) without GAA_FLAG_INCLUDE_ALL_INTERFACES:
# only interfaces bound to IPv4 or IPv6 are in the backend's enumeration.
# https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses
function Get-Net4IfaddrsOracleRows {
  $rows = [Collections.Generic.List[string]]::new()
  foreach ($adapter in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
    if ($adapter.OperationalStatus -ne [Net.NetworkInformation.OperationalStatus]::Up) { continue }
    $supports4 = $adapter.Supports([Net.NetworkInformation.NetworkInterfaceComponent]::IPv4)
    $supports6 = $adapter.Supports([Net.NetworkInformation.NetworkInterfaceComponent]::IPv6)
    if (-not $supports4 -and -not $supports6) {
      Write-Host "INFO net4 oracle: unbound NDIS interface outside AF_UNSPEC domain: $($adapter.Name) [$($adapter.Id)]"
      continue
    }
    $properties = $adapter.GetIPProperties()
    $index = if ($supports4) { $properties.GetIPv4Properties().Index } else { $properties.GetIPv6Properties().Index }
    if ($index -le 0) { throw "An IP-bound active Windows interface has no index: $($adapter.Name) [$($adapter.Id)]" }
    $hardware = $adapter.GetPhysicalAddress().ToString().ToLowerInvariant()
    if (-not $hardware) { $hardware = '-' }
    $rows.Add("P $index $hardware")
    foreach ($unicast in $properties.UnicastAddresses) {
      $address = $unicast.Address
      $scope = 0
      if ($address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork) { $family = 2 }
      elseif ($address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetworkV6) {
        $family = 10
        $scope = $address.ScopeId
      } else { throw "An IP-bound interface returned an unexpected address family: $($address.AddressFamily)" }
      $numeric = [Net.IPAddress]::new($address.GetAddressBytes()).ToString()
      $rows.Add("A $index $family $($unicast.PrefixLength) $numeric $scope")
    }
    Write-Host "INFO net4 oracle: IP-bound interface $($adapter.Name); index=$index; MAC=$hardware; addresses=$($properties.UnicastAddresses.Count)"
  }
  $rows.ToArray()
}
