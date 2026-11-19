# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
# Actual persistent device + installed APK + independent owned peer. No CI dispatch.
param([int] $NsdRegistrationInterfaceIndex = 0)
& {
$nsgSaved = @{}
foreach ($name in 'ANDROID_DATA','GESHER_BINDER','GESHER_LOGS','GESHER_APP_WINDOW','GESHER_IDLE_TIMEOUT') {
  $nsgSaved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$nsgRoot = Join-Path $root ("nsd-registration-" + [Guid]::NewGuid().ToString('N'))
$nsgData = Join-Path $nsgRoot 'data'
New-Item -ItemType Directory -Force $nsgData | Out-Null
$nsgDvm = @("-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$nsgPackage = 'com.gesher.nsdregistration'
$nsgServerPid = 0; $nsgPeer = $null; $nsgControl = $null; $nsgSuccess = $false; $nsgDeviceAttempted = $false
$nsgCounters = @{ Dump = 0; App = 0; Peer = 0 }
$nsgObservations = [Collections.Generic.List[object]]::new()

function NsdReg-Run([string[]] $Arguments, [string] $Label, [int] $Seconds = 90) {
  $text = RunTimed "$Dir/dalvikvm.exe" ($nsgDvm + $Arguments) $Seconds
  $code = $LASTEXITCODE
  [IO.File]::WriteAllText((Join-Path $nsgRoot "$Label.log"), $text, [Text.UTF8Encoding]::new($false))
  if ($code -ne 0) { throw "$Label failed (exit $code): $text" }
  return $text
}
function NsdReg-Json([string] $Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return $null }
  try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop } catch { return $null }
}
function NsdReg-Wait([scriptblock] $Condition, [string] $Reason, [int] $Seconds = 30) {
  $clock = [Diagnostics.Stopwatch]::StartNew()
  while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    if ($nsgControl) {
      foreach ($file in 'app-state.json','peer-receipt.json') {
        $value = NsdReg-Json (Join-Path $nsgControl $file)
        if ($value -and $value.error) { throw "$file failed: $($value.error)" }
      }
    }
    if (& $Condition) { return }
    Start-Sleep -Milliseconds 50
  }
  throw "Fixture deadline: $Reason"
}
function NsdReg-Command([string] $Recipient, [string] $Command) {
  if ($Recipient -eq 'app') { $nsgCounters.App++; $serial = $nsgCounters.App }
  else { $nsgCounters.Peer++; $serial = $nsgCounters.Peer }
  $file = Join-Path $nsgControl "$Recipient-command"
  [IO.File]::WriteAllText("$file.new", "$serial $Command", [Text.UTF8Encoding]::new($false))
  Move-Item -LiteralPath "$file.new" -Destination $file -Force
  NsdReg-Wait {
    $ack = NsdReg-Json (Join-Path $nsgControl "$Recipient-ack.json")
    return $ack -and $ack.serial -eq $serial -and $ack.command -eq $Command
  } "$Recipient $Command"
}
function NsdReg-Owner {
  $nsgCounters.Dump++
  $output = Join-Path $nsgRoot "owner-$($nsgCounters.Dump).txt"
  $text = NsdReg-Run @('-cp', (NoDrive "$Dir/services-tests.dex"), 'gesher.services.NsdRegistrationOwnerDump', (NoDrive $output)) "owner-$($nsgCounters.Dump)" 20
  $match = [regex]::Match($text, '(?m)^NSD_REGISTRATION_STATE pid=(\d+) clients=(\d+) requests=(\d+) native_requests=(\d+) sockets=(\d+) watches=(\d+) callbacks=(\d+) polled_requests=(\d+) sent_datagrams=(\d+) pending_withdrawals=(\d+)\r?$')
  if (-not $match.Success) { throw "Unavailable real owner counters: $text" }
  $value = @{}; $names = @('pid','clients','requests','native_requests','sockets','watches','callbacks','polled_requests','sent_datagrams','pending_withdrawals')
  for ($i = 0; $i -lt $names.Count; $i++) { $value[$names[$i]] = [long]$match.Groups[$i + 1].Value }
  if ($value.pid -ne $nsgServerPid) { throw 'Counters came from a different process' }
  $value.osUdp5353 = @(Get-NetUDPEndpoint -ErrorAction Stop |
      Where-Object { $_.OwningProcess -eq $nsgServerPid -and $_.LocalPort -eq 5353 }).Count
  $value.utc = [DateTime]::UtcNow.ToString('o'); $nsgObservations.Add($value)
  return $value
}
function NsdReg-Active([int] $Requests) {
  $v = NsdReg-Owner
  if ($v.clients -lt 1 -or $v.requests -ne $Requests -or $v.native_requests -ne 1 -or $v.sockets -lt 1 -or
      $v.watches -ne 2 -or $v.polled_requests -ne 1 -or $v.sent_datagrams -lt 1 -or $v.osUdp5353 -lt 1 -or
      $v.pending_withdrawals -ne 0) {
    throw "Owner sockets/watchers/poll/send readiness failed: $($v | ConvertTo-Json -Compress)"
  }
}
function NsdReg-Retired {
  $clock = [Diagnostics.Stopwatch]::StartNew()
  do {
    $v = NsdReg-Owner
    $left = @('clients','requests','native_requests','sockets','watches','callbacks','polled_requests','sent_datagrams','pending_withdrawals','osUdp5353') | Where-Object { $v[$_] -ne 0 }
    if (-not $left) { return }
    Start-Sleep -Milliseconds 200
  } while ($clock.Elapsed.TotalSeconds -lt 25)
  throw "Owner resources did not retire: $($v | ConvertTo-Json -Compress)"
}
function NsdReg-Process([int] $Id) {
  $p = Get-Process -Id $Id -ErrorAction SilentlyContinue
  if ($null -eq $p) { return $null }
  try { return @{ Id = $p.Id; StartTicks = $p.StartTime.ToUniversalTime().Ticks } } finally { $p.Dispose() }
}
function NsdReg-KillApp($Identity) {
  $p = Get-Process -Id $Identity.Id -ErrorAction Stop
  try {
    if ($p.StartTime.ToUniversalTime().Ticks -ne $Identity.StartTicks) { throw 'App PID was reused' }
    $command = (Get-CimInstance Win32_Process -Filter "ProcessId=$($Identity.Id)" -ErrorAction Stop).CommandLine
    if ($command -notmatch 'android\.app\.GesherAppProcess' -or $command -notmatch [regex]::Escape($nsgPackage)) { throw 'PID is not the owned app' }
    $p.Kill() # Abrupt death, no application unregister/finally.
    if (-not $p.WaitForExit(5000)) { throw 'Owned app did not exit' }
  } finally { $p.Dispose() }
}

try {
  $nsgPython = (Get-Command python -ErrorAction Stop).Source
  $nsgApk = Join-Path $Dir 'nsdregistration.apk'
  if (-not (Test-Path $nsgApk)) { throw 'Required nsdregistration.apk was not built/staged' }
  $env:ANDROID_DATA = NoDrive $nsgData
  $env:GESHER_LOGS = NoDrive (Join-Path $nsgRoot 'logs')
  $env:GESHER_APP_WINDOW = 'headless'; $env:GESHER_IDLE_TIMEOUT = '0'
  Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue
  $nsgDeviceAttempted = $true
  $null = NsdReg-Run @('com.gesher.app.Launcher', 'start') 'device-start' 400
  $stateFile = Join-Path $nsgData 'system/gesher-device'
  if (-not (Test-Path $stateFile)) { throw 'Persistent device state missing' }
  $state = @{}
  foreach ($line in Get-Content $stateFile) {
    if ($line -match '^\s*[#!]' -or $line -notmatch '=') { continue }
    $key, $value = $line -split '=', 2; $state[$key.Trim()] = ($value -replace '\\(.)', '$1')
  }
  $nsgServerPid = [int]$state.system_server; $nsgServerIdentity = NsdReg-Process $nsgServerPid
  if (-not $nsgServerIdentity) { throw 'Persistent server is not alive' }
  $env:GESHER_BINDER = $state.device
  NsdReg-Retired
  $v4 = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
    Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '127.0.0.1' -and
      ($NsdRegistrationInterfaceIndex -eq 0 -or $_.InterfaceIndex -eq $NsdRegistrationInterfaceIndex) -and
      (Get-NetIPInterface -InterfaceIndex $_.InterfaceIndex -AddressFamily IPv4).ConnectionState -eq 'Connected' } | Select-Object -First 1
  if ($null -eq $v4) { throw 'A real preferred IPv4 adapter is required' }
  $v6 = Get-NetIPAddress -InterfaceAlias $v4.InterfaceAlias -AddressFamily IPv6 -ErrorAction Stop |
    Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -ne '::1' } | Select-Object -First 1
  if ($null -eq $v6) { throw 'IPv6 remains unqualified: a real address on the selected adapter is required' }
  $families = @(@{ Family = 4; Address = $v4.IPAddress; Index = $v4.InterfaceIndex },
                @{ Family = 6; Address = $v6.IPAddress; Index = $v6.InterfaceIndex })
  foreach ($family in $families) {
    $familyName = if ($family.Family -eq 4) { 'IPv4' } else { 'IPv6' }
    $mtu = [int](Get-NetIPInterface -InterfaceIndex $family.Index -AddressFamily $familyName -ErrorAction Stop).NlMtu
    if ($mtu -le 0) { throw 'Actual family MTU missing' }
    foreach ($mode in 'public','reuse','death','uninstall','conflict') {
      $nonce = [Guid]::NewGuid().ToString('N').Substring(0, 12)
      $type = "_gr$nonce._tcp"; $instance = "Owned.שלום\😀$nonce"
      $nsgControl = Join-Path $nsgRoot "$familyName-$mode-$nonce"
      New-Item -ItemType Directory -Force $nsgControl | Out-Null
      $nsgCounters.App = 0; $nsgCounters.Peer = 0
      $null = NsdReg-Run @('com.gesher.app.Launcher', 'install', (NoDrive $nsgApk)) "install-$nonce" 300
      $psi = [Diagnostics.ProcessStartInfo]::new($nsgPython); $psi.UseShellExecute = $false
      foreach ($arg in @((Join-Path $PSScriptRoot '../nsd/registration_peer.py'), '--control', $nsgControl,
          '--type', $type, '--instance', $instance, '--mode', $mode, '--address', [string]$family.Address,
          '--family', [string]$family.Family, '--index', [string]$family.Index, '--mtu', [string]$mtu)) { $psi.ArgumentList.Add($arg) }
      $nsgPeer = [Diagnostics.Process]::Start($psi)
      try {
        NsdReg-Wait { (Test-Path (Join-Path $nsgControl 'peer-ready.json')) -and -not $nsgPeer.HasExited } 'peer readiness' 15
        $null = NsdReg-Run @('com.gesher.server.RuntimeCommands','am','start','-n',"$nsgPackage/.RegistrationActivity",
            '--es','control',(NoDrive $nsgControl),'--es','type',$type,'--es','instance',$instance,
            '--es','mode',$mode,'--es','address',[string]$family.Address,'--ei','interfaceIndex',[string]$family.Index) "launch-$nonce" 90
        NsdReg-Wait { $null -ne (NsdReg-Json (Join-Path $nsgControl 'ready.json')) } 'installed app registration' 45
        $app = NsdReg-Json (Join-Path $nsgControl 'ready.json'); $identity = NsdReg-Process ([int]$app.pid)
        if (-not $identity -or $app.uid -lt 10000 -or $app.pid -eq $nsgServerPid) { throw 'Separate assigned app identity missing' }
        NsdReg-Command 'peer' 'positive'
        NsdReg-Active $(if ($mode -eq 'reuse') { 2 } else { 1 })
        if ($mode -eq 'reuse') {
          NsdReg-Command 'peer' 'arm-reuse'; NsdReg-Command 'app' 'reuse'; NsdReg-Command 'peer' 'reuse'
          NsdReg-Active 2
          NsdReg-Command 'peer' 'arm-withdraw'; NsdReg-Command 'app' 'withdraw-main'; NsdReg-Command 'peer' 'withdraw'
          NsdReg-Active 1
          NsdReg-Command 'peer' 'arm-finish'; NsdReg-Command 'app' 'finish'
        } elseif ($mode -eq 'death') {
          NsdReg-Command 'peer' 'arm-finish'; NsdReg-KillApp $identity
        } elseif ($mode -eq 'uninstall') {
          NsdReg-Command 'peer' 'arm-finish'
          $removed = NsdReg-Run @('com.gesher.app.Launcher','uninstall',$nsgPackage) "uninstall-$nonce" 300
          if (-not $removed.Contains("uninstalled $nsgPackage")) { throw 'Actual uninstall not confirmed' }
          $packages = NsdReg-Run @('com.gesher.server.RuntimeCommands','pm','list','packages',$nsgPackage) "packages-$nonce"
          if ($packages.Contains("package:$nsgPackage")) { throw 'Removed package still installed' }
        } else { NsdReg-Command 'peer' 'arm-finish'; NsdReg-Command 'app' 'finish' }
        NsdReg-Command 'peer' 'retired'; NsdReg-Retired
        NsdReg-Wait { $null -eq (NsdReg-Process $identity.Id) } 'app retirement' 20
        $same = NsdReg-Process $nsgServerPid
        if (-not $same -or $same.StartTicks -ne $nsgServerIdentity.StartTicks) { throw 'System owner did not survive for goodbyes' }
        NsdReg-Command 'peer' 'quit'
        if (-not $nsgPeer.WaitForExit(5000) -or $nsgPeer.ExitCode -ne 0) { throw 'Peer did not close successfully' }
        $receipt = NsdReg-Json (Join-Path $nsgControl 'peer-receipt.json')
        if (-not $receipt.socketCleanup -or ($mode -ne 'public' -and -not $receipt.largeTxtReceived) -or $receipt.error) { throw 'Peer receipt incomplete' }
        Write-Host "PASS  services: actual Windows $familyName registration $mode; owned wire, same system owner, retired resources zero"
      } finally {
        if ($nsgPeer) {
          if (-not $nsgPeer.HasExited) {
            [IO.File]::WriteAllText((Join-Path $nsgControl 'peer-stop'), 'stop', [Text.UTF8Encoding]::new($false))
            if (-not $nsgPeer.WaitForExit(5000)) { $nsgPeer.Kill(); $null = $nsgPeer.WaitForExit(5000) }
          }
          $nsgPeer.Dispose(); $nsgPeer = $null
        }
      }
    }
  }
  $nsgSuccess = $true
} catch { Write-Host "FAIL  services: actual Windows registration gate: $_"; $script:failed++ }
finally {
  if ($nsgDeviceAttempted) {
    try { $null = NsdReg-Run @('com.gesher.app.Launcher','uninstall',$nsgPackage) 'cleanup-uninstall' 90 }
    catch { Write-Host "NSD fixture uninstall cleanup: $_" }
    try {
      $null = NsdReg-Run @('com.gesher.app.Launcher','shutdown') 'cleanup-shutdown' 90
      $clock = [Diagnostics.Stopwatch]::StartNew()
      do {
        $still = if ($nsgServerPid -gt 0) { NsdReg-Process $nsgServerPid } else { $null }
        if (-not $still -and -not (Test-Path (Join-Path $nsgData 'system/gesher-device'))) { break }
        Start-Sleep -Milliseconds 100
      } while ($clock.Elapsed.TotalSeconds -lt 10)
      if ($still -or (Test-Path (Join-Path $nsgData 'system/gesher-device'))) { throw 'Owned persistent device did not retire after shutdown' }
    }
    catch { Write-Host "NSD device cleanup failed: $_"; $script:failed++; $nsgSuccess = $false }
  }
  @{ selectedScenariosPassed = $nsgSuccess; fullWindowsMergeQualification = $false;
     windowsExecution = $true; ownerObservations = $nsgObservations;
     apkSha256 = $(if (Test-Path "$Dir/nsdregistration.apk") { (Get-FileHash "$Dir/nsdregistration.apk" -Algorithm SHA256).Hash });
     servicesDexSha256 = $(if (Test-Path "$Dir/services-tests.dex") { (Get-FileHash "$Dir/services-tests.dex" -Algorithm SHA256).Hash });
     limits = @('Actual interface loss/return and low-MTU fragmentation require dedicated owned-interface runs; this gate never changes host interfaces.') } |
    ConvertTo-Json -Depth 12 | Set-Content (Join-Path $nsgRoot 'gate-receipt.json') -Encoding utf8
  foreach ($name in $nsgSaved.Keys) { [Environment]::SetEnvironmentVariable($name, $nsgSaved[$name], 'Process') }
}
}
