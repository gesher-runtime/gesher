# Read-only CI diagnosis: retain every interface and both protocol observations.
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'The Net4 executable probe runs only in CI' }
$missing = 0
$active = 0
foreach ($adapter in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
  $properties = $adapter.GetIPProperties()
  $v4 = $null
  $v6 = $null
  $v4Error = ''
  $v6Error = ''
  try { $v4 = $properties.GetIPv4Properties().Index } catch { $v4Error = $_.Exception.ToString() }
  try { $v6 = $properties.GetIPv6Properties().Index } catch { $v6Error = $_.Exception.ToString() }
  Write-Host "INFO net4 adapter: name=$($adapter.Name); id=$($adapter.Id); status=$($adapter.OperationalStatus); type=$($adapter.NetworkInterfaceType); IPv4=[$v4]; IPv6=[$v6]; supports4=$($adapter.Supports([Net.NetworkInformation.NetworkInterfaceComponent]::IPv4)); supports6=$($adapter.Supports([Net.NetworkInformation.NetworkInterfaceComponent]::IPv6)); MAC=$($adapter.GetPhysicalAddress())"
  if ($v4Error) { Write-Host "INFO net4 IPv4 query error: $v4Error" }
  if ($v6Error) { Write-Host "INFO net4 IPv6 query error: $v6Error" }
  foreach ($address in $properties.UnicastAddresses) {
    Write-Host "INFO net4 address: $($address.Address)/$($address.PrefixLength)"
  }
  if ($adapter.OperationalStatus -eq [Net.NetworkInformation.OperationalStatus]::Up) {
    $active++
    # This is exactly the old test72 selection, kept for diagnosis.
    $old = 0
    try { $old = $properties.GetIPv4Properties().Index } catch { }
    if ($old -eq 0) { try { $old = $properties.GetIPv6Properties().Index } catch { } }
    Write-Host "INFO net4 original oracle selection: [$old]; is-null=$($null -eq $old)"
    if ($old -le 0) { $missing++ }
  }
}
if ($active -eq 0) { throw 'No active Windows network interface observed' }
if ($missing -ne 0) { throw "The original oracle failed to select an index for $missing active interface(s)" }
Write-Host "PASS net4 original oracle selects actual indexes for all $active active Windows interfaces"
