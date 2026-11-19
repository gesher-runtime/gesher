# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
# Real Windows transport gate. Installed-app identity and remote Binder are
# explicitly separate; this script does not dispatch or restart CI.
$nsdsPython = (Get-Command python -ErrorAction Stop).Source
$nsdsAdapter = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
  Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '127.0.0.1' -and
    (Get-NetIPInterface -InterfaceIndex $_.InterfaceIndex -AddressFamily IPv4).ConnectionState -eq 'Connected' } |
  Select-Object -First 1
if ($null -eq $nsdsAdapter) { throw 'Subscription production gate requires a real IPv4 adapter' }
$nsdsV6 = Get-NetIPAddress -InterfaceAlias $nsdsAdapter.InterfaceAlias -AddressFamily IPv6 -ErrorAction Stop |
  Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '::1' } | Select-Object -First 1
if ($null -eq $nsdsV6) { throw 'Subscription IPv6 remains unqualified: selected real adapter has no preferred IPv6 address' }
$nsdsFamilies = @(
  @{ Family = 4; Address = $nsdsAdapter.IPAddress; Index = $nsdsAdapter.InterfaceIndex },
  @{ Family = 6; Address = $nsdsV6.IPAddress; Index = $nsdsV6.InterfaceIndex }
)
foreach ($nsdsFamily in $nsdsFamilies) {
  $nsdsNonce = [Guid]::NewGuid().ToString('N').Substring(0, 12)
  $nsdsType = "_gr$nsdsNonce._tcp"
  $nsdsName = "Owned.שלום\😀$nsdsNonce"
  $nsdsControl = Join-Path $Dir "nsd-subscription-$nsdsNonce"
  # The owned peer creates this fresh directory itself; an existing path is an error.
  $nsdsStart = [Diagnostics.ProcessStartInfo]::new($nsdsPython)
  $nsdsStart.UseShellExecute = $false
  foreach ($nsdsArg in @((Join-Path $PSScriptRoot '../nsd/subscription_peer.py'), '--control', $nsdsControl,
      '--type', $nsdsType, '--instance', $nsdsName, '--index', [string]$nsdsFamily.Index,
      '--address', [string]$nsdsFamily.Address, '--family', [string]$nsdsFamily.Family)) {
    $nsdsStart.ArgumentList.Add($nsdsArg)
  }
  $nsdsPeer = [Diagnostics.Process]::Start($nsdsStart)
  try {
    $nsdsDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path (Join-Path $nsdsControl 'ready'))) {
      if ($nsdsPeer.HasExited) { throw "Independent subscription peer exited $($nsdsPeer.ExitCode)" }
      if ([DateTime]::UtcNow -ge $nsdsDeadline) { throw 'Independent subscription peer readiness deadline' }
      Start-Sleep -Milliseconds 50
    }
    CheckTimed "services: actual Windows IPv$($nsdsFamily.Family) service-info updates and native retirement" "$Dir/dalvikvm.exe" `
      ($dvmContracts + 'gesher.services.NsdSubscriptionWindowsTest' + @($nsdsType, $nsdsName,
        [string]$nsdsAdapter.InterfaceIndex, [string]$nsdsFamily.Index, (NoDrive $nsdsControl),
        [string]$nsdsFamily.Family, [string]$nsdsFamily.Address)) `
      @('NsdSubscriptionWindowsTest all passed:', "family=$($nsdsFamily.Family)",
        'production=Winsock-raw-mDNS', 'nativeActive=0', 'ordinaryApkIdentity=false', 'remoteBinder=false') 90
    if (-not $nsdsPeer.WaitForExit(5000)) { throw 'Owned subscription peer did not finish after terminal command' }
    if ($nsdsPeer.ExitCode -ne 0) { throw "Owned subscription peer exited $($nsdsPeer.ExitCode)" }
    $nsdsReceipt = Get-Content -Raw (Join-Path $nsdsControl 'result.json') | ConvertFrom-Json
    if ($nsdsReceipt.transport -ne 'real-multicast-peer' -or $nsdsReceipt.ownedQueries -lt 1 -or
        $nsdsReceipt.sentPackets -lt 4 -or $nsdsReceipt.tcpConnections -lt 1) {
      throw 'Owned subscription peer did not observe actual query, update and TCP progress'
    }
  } finally {
    if (-not $nsdsPeer.HasExited) {
      if (Test-Path $nsdsControl) {
        $nsdsQuit = Join-Path $nsdsControl 'command-shutdown'
        [IO.File]::WriteAllText($nsdsQuit, 'shutdown quit', [Text.UTF8Encoding]::new($false))
        Move-Item -Force $nsdsQuit (Join-Path $nsdsControl 'command')
      }
      if (-not $nsdsPeer.WaitForExit(5000)) { $nsdsPeer.Kill(); $nsdsPeer.WaitForExit() }
    }
    $nsdsPeer.Dispose()
  }
}
