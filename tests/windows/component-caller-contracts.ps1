# Production controller/AIDL contracts followed by real ActivityThread callbacks
# in two installed apps. This helper owns its device, data, and app logs.
# RunTimed and NoDrive come from the unchanged Windows runner prerequisites.
function Test-ComponentCallerContracts([string] $Dir, [string[]] $VmArguments, [string] $FixtureRoot) {
  $output = [Collections.Generic.List[string]]::new()
  $failed = $false
  $owned = $null
  $deviceAttempted = $false
  $deviceStopped = $true
  $deviceProcesses = [Collections.Generic.List[object]]::new()
  $names = @('ANDROID_DATA', 'GESHER_LOGS', 'GESHER_BINDER', 'GESHER_ROLE',
    'GESHER_BINDER_UID', 'GESHER_UID', 'GESHER_MICROG_SPOOF', 'GESHER_APP_WINDOW', 'GESHER_SHORTCUTS')
  $saved = @{}
  foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
  try {
    foreach ($name in @('components.apk', 'peer.apk')) {
      if (-not (Test-Path -LiteralPath (Join-Path $Dir $name) -PathType Leaf)) {
        throw "Missing real caller-contract app: $name"
      }
    }
    $owned = Join-Path $FixtureRoot ('component-callers-' + [Guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $owned 'data'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $owned 'logs'))
    $env:ANDROID_DATA = NoDrive (Join-Path $owned 'data')
    $env:GESHER_LOGS = NoDrive (Join-Path $owned 'logs')
    Remove-Item -LiteralPath Env:GESHER_BINDER -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath Env:GESHER_ROLE -ErrorAction SilentlyContinue
    $env:GESHER_BINDER_UID = '1000'
    $env:GESHER_UID = '1000'
    $env:GESHER_MICROG_SPOOF = '1'
    $env:GESHER_APP_WINDOW = 'headless'
    $env:GESHER_SHORTCUTS = '0' # This fixture owns Android state, not Start-menu/URL registrations.

    $result = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'gesher.services.ServiceFixtureRunner' +
      'ComponentCallerContractTest') 90
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0 -or -not $result.Contains('ComponentCallerContractTest all passed:')) {
      throw "Actual framework/controller Binder contract failed with exit $code"
    }

    $launcher = $VmArguments + 'com.gesher.app.Launcher'
    $deviceAttempted = $true
    $deviceStopped = $false
    $result = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'install' + (NoDrive "$Dir/peer.apk")) 90
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0) { throw "Real peer install failed with exit $code" }
    # Open and retain the actual process handles while the owned device is live.
    # Device's .start values are GetProcessTimes creation FILETIMEs. Matching
    # those prevents a reused PID from becoming either our evidence or a target.
    $state = @{}
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $owned 'data/system/gesher-device'))) {
      if ($line -match '^(binderd|servicemanager|system_server)(\.start)?=(\d+)$') {
        $state[$Matches[1] + $Matches[2]] = $Matches[3]
      }
    }
    foreach ($name in @('binderd', 'servicemanager', 'system_server')) {
      if (-not $state.ContainsKey($name) -or -not $state.ContainsKey($name + '.start')) {
        throw "Owned device is missing process identity: $name"
      }
      $process = [Diagnostics.Process]::GetProcessById([int]$state[$name])
      try {
        $null = $process.Handle
        if ($process.HasExited -or $process.StartTime.ToUniversalTime().ToFileTimeUtc() -ne
            [long]$state[$name + '.start']) {
          throw "Owned device process generation changed: $name"
        }
        $deviceProcesses.Add(@{ Name = $name; Process = $process })
        $process = $null
      } finally { if ($null -ne $process) { $process.Dispose() } }
    }
    $result = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'launch' + (NoDrive "$Dir/components.apk") +
      '--activity' + '.CallerContractActivity' + '--headless' + '--keep-task') 180
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0 -or -not $result.Contains('ComponentCallerLifecycleTest all passed:')) {
      throw "Actual cross-app caller lifecycle failed with exit $code"
    }
    $peerLog = Join-Path $owned 'logs/com.gesher.peer.log'
    if (-not (Test-Path -LiteralPath $peerLog -PathType Leaf) -or
        -not ([IO.File]::ReadAllText($peerLog)).Contains('ComponentCallerLifecyclePeer all passed:')) {
      throw 'Peer did not finish its actual callback, relaunch and delegated-caller checks'
    }
  } catch {
    $failed = $true
    $output.Add([string]$_)
  } finally {
    if ($deviceAttempted) {
      try {
        $result = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'com.gesher.app.Launcher' + 'shutdown') 60
        $code = $LASTEXITCODE
        $output.Add($result)
        if ($code -ne 0 -or (Test-Path -LiteralPath (Join-Path $owned 'data/system/gesher-device'))) {
          throw "Owned caller-contract device did not shut down (exit $code)"
        }
        foreach ($entry in $deviceProcesses) {
          if (-not $entry.Process.WaitForExit(10000) -or -not $entry.Process.HasExited) {
            throw "Owned device process did not exit: $($entry.Name)"
          }
        }
        $deviceStopped = $true
      } catch {
        $failed = $true
        $output.Add('Caller device cleanup failed: ' + [string]$_)
      }
    }
    if ($failed -and $owned) {
      foreach ($log in @(Get-ChildItem -LiteralPath (Join-Path $owned 'logs') -File -ErrorAction SilentlyContinue)) {
        try {
          $output.Add('Caller fixture log: ' + $log.Name)
          $output.Add([IO.File]::ReadAllText($log.FullName))
        } catch { $output.Add('Caller diagnostic read failed: ' + [string]$_) }
      }
    }
    foreach ($entry in $deviceProcesses) {
      try { $entry.Process.Dispose() }
      catch {
        $failed = $true
        $output.Add('Caller process handle cleanup failed: ' + [string]$_)
      }
    }
    foreach ($name in $names) {
      try {
        if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
        if (-not [object]::Equals([Environment]::GetEnvironmentVariable($name, 'Process'), $saved[$name])) {
          throw "Environment entry $name was not restored exactly"
        }
      } catch {
        $failed = $true
        $output.Add('Caller environment restoration failed: ' + [string]$_)
      }
    }
    try {
      if ($owned -and $deviceStopped -and (Test-Path -LiteralPath $owned)) {
        Remove-Item -LiteralPath $owned -Recurse -Force -ErrorAction Stop
        if (Test-Path -LiteralPath $owned) { throw 'Owned caller fixture data survived cleanup' }
      }
    } catch {
      $failed = $true
      $output.Add('Caller data cleanup failed: ' + [string]$_)
    }
  }
  if (-not $failed) { $output.Add('ComponentCallerBootstrapTest all passed: real Binder and cross-app callbacks; owned device stopped') }
  $global:LASTEXITCODE = if ($failed) { 1 } else { 0 }
  return $output -join "`n"
}
