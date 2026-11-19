# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
# Dot-sourced from the required services track. Missing APIs/peers/callbacks fail.
$nsdPython = (Get-Command python -ErrorAction Stop).Source
& $nsdPython (Join-Path $PSScriptRoot '../../tools/test_nsd_peer_scope.py')
if ($LASTEXITCODE -ne 0) { throw 'NSD peer receive-interface and permission-fixture checks failed' }
$nsdAdapter = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
  Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '127.0.0.1' -and
    (Get-NetIPInterface -InterfaceIndex $_.InterfaceIndex -AddressFamily IPv4).ConnectionState -eq 'Connected' } |
  Select-Object -First 1
if ($null -eq $nsdAdapter) { throw 'NSD production gate requires a real IPv4 interface' }
$nsdFamilies = @(@{ Family = 4; Address = $nsdAdapter.IPAddress; Index = $nsdAdapter.InterfaceIndex })
$nsdV6 = Get-NetIPAddress -InterfaceAlias $nsdAdapter.InterfaceAlias -AddressFamily IPv6 -ErrorAction SilentlyContinue |
  Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '::1' } | Select-Object -First 1
if ($null -ne $nsdV6) { $nsdFamilies += @{ Family = 6; Address = $nsdV6.IPAddress; Index = $nsdV6.InterfaceIndex } }
else { Write-Host 'NSD IPv6 not exercised: selected interface has no preferred IPv6 address' }
foreach ($nsdFamily in $nsdFamilies) {
  $nsdNonce = [Guid]::NewGuid().ToString('N').Substring(0, 12)
  $nsdType = "_gs$nsdNonce._tcp"
  $nsdInstance = "Owned.שלום\$nsdNonce"
  $nsdControl = Join-Path $Dir "nsd-$nsdNonce"
  New-Item -ItemType Directory -Force $nsdControl | Out-Null
  $nsdInfo = [Diagnostics.ProcessStartInfo]::new($nsdPython)
  $nsdInfo.UseShellExecute = $false
  $nsdArguments = @((Join-Path $PSScriptRoot '../nsd/windows_peer.py'), '--control', $nsdControl,
    '--type', $nsdType, '--instance', $nsdInstance, '--index', [string]$nsdFamily.Index,
    '--address', [string]$nsdFamily.Address, '--family', [string]$nsdFamily.Family)
  foreach ($nsdArgument in $nsdArguments) { $nsdInfo.ArgumentList.Add($nsdArgument) }
  $nsdPeer = [Diagnostics.Process]::Start($nsdInfo)
  try {
    $nsdDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path (Join-Path $nsdControl 'ready'))) {
      if ($nsdPeer.HasExited) { throw "Independent NSD peer failed with exit $($nsdPeer.ExitCode)" }
      if ([DateTime]::UtcNow -gt $nsdDeadline) { throw 'Independent NSD peer did not become ready' }
      Start-Sleep -Milliseconds 50
    }
    CheckTimed "services: Windows DNS-SD IPv$($nsdFamily.Family), wire peer, lost, binding and cancel" "$Dir/dalvikvm.exe" `
      ($dvmContracts + 'gesher.services.NsdBrowseWindowsTest' + @($nsdType, $nsdInstance,
        [string]$nsdAdapter.InterfaceIndex, (NoDrive $nsdControl), [string]$nsdFamily.Family)) `
      @('NsdBrowseWindowsTest all passed:', "family=$($nsdFamily.Family)", 'production=DnsStartMulticastQuery', 'nativeActive=0') 120
  } finally {
    if (-not $nsdPeer.HasExited) { $nsdPeer.Kill(); $nsdPeer.WaitForExit() }
    $nsdPeer.Dispose()
  }
}
