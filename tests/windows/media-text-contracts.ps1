# Separate ART processes, ordinary app UID, real media/text services and Binder-death cleanup.
# Caller supplies RunTimed and the ordinary framework/app VM classpath.
function Test-MediaTextTransport([string] $Dir, [string[]] $VmArguments, [string] $FixtureRoot) {
  $output = [Collections.Generic.List[string]]::new()
  $owned = [Collections.Generic.List[object]]::new()
  $failed = $false
  $fixture = $null
  $names = @('ANDROID_DATA', 'GESHER_BINDER', 'GESHER_ROLE', 'GESHER_BINDER_UID', 'GESHER_UID', 'GESHER_MICROG_SPOOF')
  $saved = @{}
  foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
  # Keep the temporary directory's drive when the VM's classpath is on another drive.
  function MediaTextGuestPath([string] $WindowsPath) {
    $full = [IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^([A-Za-z]):[\\/]') { throw "Media/text fixture path is not a local drive path: $full" }
    return '/mnt/' + $Matches[1].ToLowerInvariant() + ($full.Substring(2) -replace '\\', '/')
  }
  function Start-MediaTextProcess([string] $Name, [string] $Exe, [string[]] $Arguments, [string] $ErrorLog) {
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
    $entry = $null
    $errorFile = $null
    try {
      if ($ErrorLog) {
        # Do not buffer the readiness line until process exit. Preserve the full
        # stderr stream while allowing this fixture to read its live diagnostic.
        $errorFile = [IO.FileStream]::new($ErrorLog, [IO.FileMode]::CreateNew,
          [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite, 1, [IO.FileOptions]::Asynchronous)
      }
      if (-not $process.Start()) { throw "Could not start owned media/text process $Name" }
      $entry = @{ Name = $Name; Process = $process; Out = $null; Err = $null;
        ErrorLog = $ErrorLog; ErrorFile = $errorFile }
      $owned.Add($entry)
      $null = $process.Handle
      $entry.Out = $process.StandardOutput.ReadToEndAsync()
      $entry.Err = if ($errorFile) { $process.StandardError.BaseStream.CopyToAsync($errorFile) }
        else { $process.StandardError.ReadToEndAsync() }
      return $process
    } catch {
      if ($null -eq $entry) {
        if ($errorFile) { $errorFile.Dispose() }
        $process.Dispose()
      }
      throw
    }
  }
  try {
    $fixture = Join-Path $FixtureRoot ('media-text-transport-' + [Guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $fixture 'data'))
    $ready = Join-Path $fixture 'ready'
    $env:ANDROID_DATA = MediaTextGuestPath (Join-Path $fixture 'data')
    $env:GESHER_BINDER = 'media-text-' + [Guid]::NewGuid().ToString('N')
    $env:GESHER_BINDER_UID = '1000'
    $env:GESHER_UID = '1000'
    $env:GESHER_MICROG_SPOOF = '0'
    Remove-Item -LiteralPath Env:GESHER_ROLE -ErrorAction SilentlyContinue
    # Share the existing startup budget across broker, context manager and server.
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    $broker = Start-MediaTextProcess 'binderd' "$Dir/binderd.exe" @($env:GESHER_BINDER)
    if ($broker.HasExited) { throw "Media/text binderd exited before readiness: $($broker.ExitCode)" }
    $handshake = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'com.gesher.server.RuntimeCommands' +
      'wait-binder' + $env:GESHER_BINDER + [string]$broker.Id + '15000') 15
    $handshakeCode = $LASTEXITCODE
    $output.Add($handshake)
    if ($handshakeCode -ne 0 -or -not $handshake.Contains("binder endpoint accepted a protocol connection: $env:GESHER_BINDER")) {
      throw "Media/text binderd handshake failed with exit $handshakeCode"
    }
    if ($broker.HasExited) { throw "Media/text binderd exited after readiness: $($broker.ExitCode)" }
    $managerLog = Join-Path $fixture 'servicemanager.log'
    $manager = Start-MediaTextProcess 'servicemanager' "$Dir/servicemanager.exe" @() $managerLog
    $managerCapture = $owned[$owned.Count - 1]
    $managerDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while ($true) {
      if ($broker.HasExited) { throw "Media/text binderd exited while waiting for servicemanager: $($broker.ExitCode)" }
      if ($manager.HasExited) { throw "Media/text servicemanager exited before readiness: $($manager.ExitCode)" }
      if ($managerCapture.Err.IsFaulted) { $managerCapture.Err.GetAwaiter().GetResult() }
      if ([DateTime]::UtcNow -ge $managerDeadline -or [DateTime]::UtcNow -ge $deadline) {
        throw 'Media/text servicemanager readiness timed out'
      }
      if (((Get-Content -LiteralPath $managerLog -Raw -ErrorAction Stop) + '').Contains('servicemanager: ready on /dev/binder')) { break }
      Start-Sleep -Milliseconds 100
    }
    $server = Start-MediaTextProcess 'media-text-server' "$Dir/dalvikvm.exe" `
      ($VmArguments + 'gesher.services.MediaTextRemoteTest' + 'server' + (MediaTextGuestPath $ready) + (MediaTextGuestPath (Join-Path $Dir 'components.apk')))
    while (-not (Test-Path -LiteralPath $ready -PathType Leaf)) {
      if ($broker.HasExited) { throw "Media/text binderd exited before server readiness: $($broker.ExitCode)" }
      if ($manager.HasExited) { throw "Media/text servicemanager exited before server readiness: $($manager.ExitCode)" }
      if ($server.HasExited) { throw "Media/text transport server exited before readiness: $($server.ExitCode)" }
      if ([DateTime]::UtcNow -ge $deadline) { throw 'Media/text transport server readiness timed out' }
      Start-Sleep -Milliseconds 100
    }
    $admission = [IO.File]::ReadAllLines($ready)
    if ($admission.Count -ne 2) { throw 'Invalid media/text fixture admission record' }
    $package = $admission[0]
    if ($package -notmatch '^[A-Za-z0-9_.]+$') { throw 'Invalid media/text fixture package' }
    $clientUid = 0
    if (-not [int]::TryParse($admission[1], [ref]$clientUid) -or $clientUid -lt 10000 -or $clientUid -gt 19999) {
      throw 'Media/text fixture did not receive an ordinary canonical application UID'
    }
    $env:GESHER_BINDER_UID = [string]$clientUid
    $env:GESHER_UID = [string]$clientUid
    $result = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'gesher.services.MediaTextRemoteTest' + 'client' + $package + [string]$clientUid) 120
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0 -or -not $result.Contains('MediaTextRemoteTest all passed:')) {
      throw "Media/text ordinary-UID transport failed with exit $code"
    }
    $env:GESHER_BINDER_UID = '1000'
    $env:GESHER_UID = '1000'
    $cleanup = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'gesher.services.MediaTextRemoteTest' + 'verify-cleanup') 45
    $cleanupCode = $LASTEXITCODE
    $output.Add($cleanup)
    if ($cleanupCode -ne 0 -or -not $cleanup.Contains('MediaTextRemoteTest death cleanup passed')) {
      throw "Media/text Binder-death cleanup failed with exit $cleanupCode"
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
          if ($null -eq $entry[$stream]) { continue }
          if (-not $entry[$stream].Wait(2000)) { throw "Owned $($entry.Name) output did not drain" }
          $text = $entry[$stream].GetAwaiter().GetResult()
          if ($stream -eq 'Err' -and $entry.ErrorFile) {
            $entry.ErrorFile.Flush()
            $text = Get-Content -LiteralPath $entry.ErrorLog -Raw -ErrorAction Stop
          }
          if ($text) { $output.Add("$($entry.Name) ${stream}: $text") }
        }
      } catch { $failed = $true; $output.Add('Media/text cleanup: ' + [string]$_) }
      finally {
        try { if ($entry.ErrorFile) { $entry.ErrorFile.Dispose() } }
        catch { $failed = $true; $output.Add('Media/text capture disposal: ' + [string]$_) }
        finally { $entry.Process.Dispose() }
      }
    }
    foreach ($name in $names) {
      try {
        if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
      } catch { $failed = $true; $output.Add('Media/text environment restore: ' + [string]$_) }
    }
    try { if ($fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction Stop } }
    catch { $failed = $true; $output.Add('Media/text fixture cleanup: ' + [string]$_) }
  }
  $global:LASTEXITCODE = if ($failed) { 1 } else { 0 }
  return $output -join "`n"
}
