# Real ART server and separate ordinary/system clients using production provider transport.
# Caller supplies RunTimed and the ordinary framework/app VM classpath.
function Test-SettingsProviderTransport([string] $Dir, [string[]] $VmArguments, [string] $FixtureRoot) {
  $output = [Collections.Generic.List[string]]::new()
  $owned = [Collections.Generic.List[object]]::new()
  $failed = $false
  $fixture = $null
  $names = @('ANDROID_DATA', 'GESHER_BINDER', 'GESHER_ROLE', 'GESHER_BINDER_UID', 'GESHER_UID', 'GESHER_MICROG_SPOOF')
  $saved = @{}
  foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
  # Keep the temporary directory's drive when the VM's classpath is on another drive.
  function SettingsGuestPath([string] $WindowsPath) {
    $full = [IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^([A-Za-z]):[\\/]') { throw "Settings fixture path is not a local drive path: $full" }
    return '/mnt/' + $Matches[1].ToLowerInvariant() + ($full.Substring(2) -replace '\\', '/')
  }
  function Start-SettingsProcess([string] $Name, [string] $Exe, [string[]] $Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Exe
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    # Retain the suite's working directory: its VM classpath may contain dist/...
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
      if (-not $process.Start()) { throw "Could not start owned settings process $Name" }
      $null = $process.Handle
      $entry = @{ Name = $Name; Process = $process;
        Out = $process.StandardOutput.ReadToEndAsync(); Err = $process.StandardError.ReadToEndAsync() }
      $owned.Add($entry)
      return $process
    } catch { $process.Dispose(); throw }
  }
  function Start-ReadySettingsManager([Diagnostics.Process] $Broker, [string] $Device) {
    if ($Broker.HasExited) { throw "Settings binderd exited before readiness: $($Broker.ExitCode)" }
    $probe = @($VmArguments) + @('com.gesher.server.RuntimeCommands', 'wait-binder',
      $Device, [string]$Broker.Id, '15000')
    $readyOut = RunTimed "$Dir/dalvikvm.exe" $probe 15
    $readyCode = $LASTEXITCODE
    $output.Add($readyOut)
    if ($readyCode -ne 0 -or -not $readyOut.Contains("binder endpoint accepted a protocol connection: $Device")) {
      throw "Settings binderd did not become ready (exit $readyCode)"
    }
    if ($Broker.HasExited) { throw "Settings binderd exited after readiness: $($Broker.ExitCode)" }

    # Keep a readable log while the manager runs: ReadToEndAsync completes only
    # on exit, too late to observe ownership of the Binder context.
    $managerOut = Join-Path $fixture 'servicemanager.out'
    $managerErr = Join-Path $fixture 'servicemanager.err'
    $manager = Start-Process "$Dir/servicemanager.exe" -PassThru -NoNewWindow -ErrorAction Stop `
      -RedirectStandardOutput $managerOut -RedirectStandardError $managerErr
    $owned.Add(@{ Name = 'servicemanager'; Process = $manager; OutFile = $managerOut; ErrFile = $managerErr })
    $null = $manager.Handle
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 15000) {
      if ($Broker.HasExited) { throw "Settings binderd exited while waiting for servicemanager: $($Broker.ExitCode)" }
      if ($manager.HasExited) { throw "Settings servicemanager exited before readiness: $($manager.ExitCode)" }
      $managerLog = if (Test-Path -LiteralPath $managerErr -PathType Leaf) {
        (Get-Content -LiteralPath $managerErr -Raw -ErrorAction Stop) + ''
      } else { '' }
      if ($managerLog.Contains('servicemanager: ready on /dev/binder') -and
          $timer.ElapsedMilliseconds -lt 15000 -and -not $Broker.HasExited -and -not $manager.HasExited) {
        return $manager
      }
      Start-Sleep -Milliseconds 100
    }
    throw 'Settings servicemanager did not become ready within 15 s'
  }
  try {
    $fixture = Join-Path $FixtureRoot ('settings-transport-' + [Guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $fixture 'data'))
    $ready = Join-Path $fixture 'ready'
    $env:ANDROID_DATA = SettingsGuestPath (Join-Path $fixture 'data')
    $env:GESHER_BINDER = 'settings-' + [Guid]::NewGuid().ToString('N')
    $env:GESHER_BINDER_UID = '1000'
    $env:GESHER_UID = '1000'
    $env:GESHER_MICROG_SPOOF = '0'
    Remove-Item -LiteralPath Env:GESHER_ROLE -ErrorAction SilentlyContinue
    $broker = Start-SettingsProcess 'binderd' "$Dir/binderd.exe" @($env:GESHER_BINDER)
    $null = Start-ReadySettingsManager $broker $env:GESHER_BINDER
    $server = Start-SettingsProcess 'settings-server' "$Dir/dalvikvm.exe" `
      ($VmArguments + 'gesher.services.SettingsProviderRemoteTest' + 'server' + (SettingsGuestPath $ready))
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (-not (Test-Path -LiteralPath $ready -PathType Leaf)) {
      if ($server.HasExited) { throw "Settings transport server exited before readiness: $($server.ExitCode)" }
      if ([DateTime]::UtcNow -ge $deadline) { throw 'Settings transport server readiness timed out' }
      Start-Sleep -Milliseconds 100
    }
    $env:GESHER_BINDER_UID = '10042'
    $env:GESHER_UID = '10042'
    $result = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'gesher.services.SettingsProviderRemoteTest' + 'client') 90
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0 -or -not $result.Contains('SettingsProviderRemoteTest ordinary passed:')) {
      throw "Settings ordinary-UID transport failed with exit $code"
    }
    $env:GESHER_BINDER_UID = '1000'
    $env:GESHER_UID = '1000'
    $result = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'gesher.services.SettingsProviderRemoteTest' + 'client') 90
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0 -or -not $result.Contains('SettingsProviderRemoteTest all passed:')) {
      throw "Settings authorized system-UID transport failed with exit $code"
    }
  } catch {
    $failed = $true
    $output.Add([string]$_)
  } finally {
    for ($i = $owned.Count - 1; $i -ge 0; $i--) {
      $entry = $owned[$i]
      try {
        if (-not $entry.Process.HasExited) { $entry.Process.Kill($true) }
        if (-not $entry.Process.WaitForExit(10000)) { throw "Owned $($entry.Name) did not stop" }
        foreach ($stream in @('Out', 'Err')) {
          if ($entry.ContainsKey($stream + 'File')) {
            $text = Get-Content -LiteralPath $entry[$stream + 'File'] -Raw -ErrorAction Stop
          } else {
            if (-not $entry[$stream].Wait(2000)) { throw "Owned $($entry.Name) output did not drain" }
            $text = $entry[$stream].GetAwaiter().GetResult()
          }
          if ($text) { $output.Add("$($entry.Name) ${stream}: $text") }
        }
      } catch { $failed = $true; $output.Add('Settings cleanup: ' + [string]$_) }
      finally { $entry.Process.Dispose() }
    }
    foreach ($name in $names) {
      try {
        if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
      } catch { $failed = $true; $output.Add('Settings environment restore: ' + [string]$_) }
    }
    try { if ($fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction Stop } }
    catch { $failed = $true; $output.Add('Settings fixture cleanup: ' + [string]$_) }
  }
  $global:LASTEXITCODE = if ($failed) { 1 } else { 0 }
  return $output -join "`n"
}
