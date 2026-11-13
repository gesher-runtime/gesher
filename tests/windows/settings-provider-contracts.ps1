# Two actual ART processes, separate Binder UIDs, and the production provider transport.
# Caller supplies RunTimed, NoDrive and the ordinary framework/app VM classpath.
function Test-SettingsProviderTransport([string] $Dir, [string[]] $VmArguments, [string] $FixtureRoot) {
  $output = [Collections.Generic.List[string]]::new()
  $owned = [Collections.Generic.List[object]]::new()
  $failed = $false
  $fixture = $null
  $names = @('ANDROID_DATA', 'GESHER_BINDER', 'GESHER_ROLE', 'GESHER_BINDER_UID', 'GESHER_UID', 'GESHER_MICROG_SPOOF')
  $saved = @{}
  foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
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
  try {
    $fixture = Join-Path $FixtureRoot ('settings-transport-' + [Guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $fixture 'data'))
    $ready = Join-Path $fixture 'ready'
    $env:ANDROID_DATA = NoDrive (Join-Path $fixture 'data')
    $env:GESHER_BINDER = 'settings-' + [Guid]::NewGuid().ToString('N')
    $env:GESHER_BINDER_UID = '1000'
    $env:GESHER_UID = '1000'
    $env:GESHER_MICROG_SPOOF = '0'
    Remove-Item -LiteralPath Env:GESHER_ROLE -ErrorAction SilentlyContinue
    $null = Start-SettingsProcess 'binderd' "$Dir/binderd.exe" @($env:GESHER_BINDER)
    $null = Start-SettingsProcess 'servicemanager' "$Dir/servicemanager.exe" @()
    $server = Start-SettingsProcess 'settings-server' "$Dir/dalvikvm.exe" `
      ($VmArguments + 'gesher.services.SettingsProviderRemoteTest' + 'server' + (NoDrive $ready))
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
    if ($code -ne 0 -or -not $result.Contains('SettingsProviderRemoteTest all passed:')) {
      throw "Settings ordinary-UID transport failed with exit $code"
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
          if (-not $entry[$stream].Wait(2000)) { throw "Owned $($entry.Name) output did not drain" }
          $text = $entry[$stream].GetAwaiter().GetResult()
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
