# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
# Required services-track gates. This script does not dispatch or restart CI.
$nsdrPython = (Get-Command python -ErrorAction Stop).Source
$nsdrPackets = Join-Path $Dir 'nsd-resolve-valid-packets.tsv'
& $nsdrPython (Join-Path $PSScriptRoot '../nsd/resolver_peer.py') --fixtures $nsdrPackets
if ($LASTEXITCODE -ne 0) { throw 'Independent resolver packet producer failed' }
CheckTimed 'services: DNS-SD resolve validation, binary TXT, TTL, scope and ownership seams' "$Dir/dalvikvm.exe" `
  ($dvmContracts + 'gesher.services.NsdResolveContractTest' + @('--windows', (NoDrive $nsdrPackets))) `
  @('NsdResolveContractTest all passed:', 'transport=explicit-fixtures') 45
$nsdrAdapter = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
  Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '127.0.0.1' -and
    (Get-NetIPInterface -InterfaceIndex $_.InterfaceIndex -AddressFamily IPv4).ConnectionState -eq 'Connected' } |
  Select-Object -First 1
if ($null -eq $nsdrAdapter) { throw 'Resolver production gate requires a real IPv4 adapter' }
$nsdrFamilies = @(@{ Family = 4; Address = $nsdrAdapter.IPAddress; Index = $nsdrAdapter.InterfaceIndex })
$nsdrV6 = Get-NetIPAddress -InterfaceAlias $nsdrAdapter.InterfaceAlias -AddressFamily IPv6 -ErrorAction SilentlyContinue |
  Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '::1' } | Select-Object -First 1
if ($null -ne $nsdrV6) { $nsdrFamilies += @{ Family = 6; Address = $nsdrV6.IPAddress; Index = $nsdrV6.InterfaceIndex } }
else { Write-Host 'NSD resolution IPv6 remains unqualified: selected real adapter has no preferred IPv6 address' }
foreach ($nsdrFamily in $nsdrFamilies) {
  $nsdrNonce = [Guid]::NewGuid().ToString('N').Substring(0, 12)
  $nsdrType = "_gr$nsdrNonce._tcp"
  $nsdrName = "Owned.שלום\😀$nsdrNonce"
  $nsdrControl = Join-Path $Dir "nsd-resolve-$nsdrNonce"
  New-Item -ItemType Directory -Force $nsdrControl | Out-Null
  $nsdrStart = [Diagnostics.ProcessStartInfo]::new($nsdrPython)
  $nsdrStart.UseShellExecute = $false
  foreach ($nsdrArg in @((Join-Path $PSScriptRoot '../nsd/resolver_peer.py'), '--control', $nsdrControl,
      '--type', $nsdrType, '--instance', $nsdrName, '--index', [string]$nsdrFamily.Index,
      '--address', [string]$nsdrFamily.Address, '--family', [string]$nsdrFamily.Family)) {
    $nsdrStart.ArgumentList.Add($nsdrArg)
  }
  $nsdrPeer = [Diagnostics.Process]::Start($nsdrStart)
  try {
    $nsdrDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path (Join-Path $nsdrControl 'ready'))) {
      if ($nsdrPeer.HasExited) { throw "Independent resolver peer exited $($nsdrPeer.ExitCode)" }
      if ([DateTime]::UtcNow -ge $nsdrDeadline) { throw 'Independent resolver peer readiness deadline' }
      Start-Sleep -Milliseconds 50
    }
    CheckTimed "services: actual Windows IPv$($nsdrFamily.Family) binary DNS-SD resolution and native retirement" "$Dir/dalvikvm.exe" `
      ($dvmContracts + 'gesher.services.NsdResolveWindowsTest' + @($nsdrType, $nsdrName,
        [string]$nsdrAdapter.InterfaceIndex, [string]$nsdrFamily.Index, (NoDrive $nsdrControl),
        [string]$nsdrFamily.Family, [string]$nsdrFamily.Address)) `
      @('NsdResolveWindowsTest all passed:', "family=$($nsdrFamily.Family)", 'production=Winsock-raw-mDNS', 'nativeActive=0') 90
  } finally {
    if (-not $nsdrPeer.HasExited) {
      # Let the owned peer close its UDP/TCP sockets and write its measurement
      # receipt even if the Java gate failed before sending its final command.
      $nsdrQuit = Join-Path $nsdrControl 'command-shutdown'
      [IO.File]::WriteAllText($nsdrQuit, 'shutdown quit', [Text.UTF8Encoding]::new($false))
      Move-Item -Force $nsdrQuit (Join-Path $nsdrControl 'command')
      if (-not $nsdrPeer.WaitForExit(5000)) { $nsdrPeer.Kill(); $nsdrPeer.WaitForExit() }
    }
    $nsdrPeer.Dispose()
  }
}
