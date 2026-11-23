# Own a private Binder endpoint and two real ordinary-UID peers for the SAF
# fixture. The test operator owns discovery; app UIDs do not register services.
# Caller supplies RunTimed and the framework/app VM classpath.
function Start-SafBinderFixture([string] $Dir, [string[]] $VmArguments, [string] $FixtureRoot) {
  $session = @{ Owned = [Collections.Generic.List[object]]::new(); Saved = @{};
    Names = @('GESHER_BINDER', 'GESHER_ROLE', 'GESHER_BINDER_UID', 'GESHER_UID');
    Directory = $null; Diagnostics = [Collections.Generic.List[string]]::new() }
  foreach ($name in $session.Names) {
    $session.Saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
  }
  function SafGuestPath([string] $WindowsPath) {
    $full = [IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^([A-Za-z]):[\\/]') { throw "SAF path is not on a local drive: $full" }
    return '/mnt/' + $Matches[1].ToLowerInvariant() + ($full.Substring(2) -replace '\\', '/')
  }
  function Start-SafOwned([string] $Name, [string] $Exe, [string[]] $Arguments,
      [int] $Uid = 1000, [string] $ErrorLog = '') {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Exe
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $info.Environment['GESHER_BINDER_UID'] = [string]$Uid
    $info.Environment['GESHER_UID'] = [string]$Uid
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $entry = $null
    $errorFile = $null
    try {
      if ($ErrorLog) {
        $errorFile = [IO.FileStream]::new($ErrorLog, [IO.FileMode]::CreateNew,
          [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite, 1, [IO.FileOptions]::Asynchronous)
      }
      if (-not $process.Start()) { throw "Could not start owned SAF process $Name" }
      $entry = @{ Name = $Name; Process = $process; Out = $null; Err = $null;
        ErrorLog = $ErrorLog; ErrorFile = $errorFile }
      $session.Owned.Add($entry)
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
  function Assert-SafAlive {
    foreach ($entry in $session.Owned) {
      if ($entry.Process.HasExited) { throw "Owned SAF $($entry.Name) exited before readiness: $($entry.Process.ExitCode)" }
      if ($entry.Err.IsFaulted) { $entry.Err.GetAwaiter().GetResult() }
    }
  }
  function Wait-SafReady([Diagnostics.Process] $Process, [int] $Uid, [string] $Ready) {
    while (-not (Test-Path -LiteralPath $Ready -PathType Leaf)) {
      Assert-SafAlive
      if ([DateTime]::UtcNow -ge $deadline) { throw "SAF process $($Process.Id) readiness timed out" }
      Start-Sleep -Milliseconds 100
    }
    Assert-SafAlive
    $credentials = [IO.File]::ReadAllText($Ready).Trim()
    if ($credentials -notmatch '^([0-9]+) ([0-9]+)$' -or
        [int]$Matches[1] -ne $Uid -or [int]$Matches[2] -ne $Process.Id) {
      throw "SAF native credentials do not match owned process $($Process.Id) and UID $Uid`: $credentials"
    }
  }
  try {
    $session.Directory = Join-Path $FixtureRoot ('saf-binder-' + [Guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory($session.Directory)
    $env:GESHER_BINDER = 'saf-' + [Guid]::NewGuid().ToString('N')
    $env:GESHER_BINDER_UID = '1000'
    $env:GESHER_UID = '1000'
    Remove-Item -LiteralPath Env:GESHER_ROLE -ErrorAction SilentlyContinue
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    $broker = Start-SafOwned 'binderd' "$Dir/binderd.exe" @($env:GESHER_BINDER)
    Assert-SafAlive
    $handshake = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'com.gesher.server.RuntimeCommands' +
      'wait-binder' + $env:GESHER_BINDER + [string]$broker.Id + '15000') 15
    $handshakeCode = $LASTEXITCODE
    $session.Diagnostics.Add($handshake)
    if ($handshakeCode -ne 0 -or -not $handshake.Contains("binder endpoint accepted a protocol connection: $env:GESHER_BINDER")) {
      throw "SAF binderd handshake failed with exit $handshakeCode"
    }
    Assert-SafAlive
    $managerLog = Join-Path $session.Directory 'servicemanager.log'
    $manager = Start-SafOwned 'servicemanager' "$Dir/servicemanager.exe" @() 1000 $managerLog
    $managerDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while ($true) {
      Assert-SafAlive
      if ([DateTime]::UtcNow -ge $managerDeadline -or [DateTime]::UtcNow -ge $deadline) {
        throw 'SAF servicemanager readiness timed out'
      }
      if (((Get-Content -LiteralPath $managerLog -Raw -ErrorAction Stop) + '').Contains('servicemanager: ready on /dev/binder')) { break }
      Start-Sleep -Milliseconds 100
    }
    $registryReady = Join-Path $session.Directory 'registry.ready'
    $registry = Start-SafOwned 'registry' "$Dir/dalvikvm.exe" `
      ($VmArguments + 'gesher.services.SafDocumentsContractTest' + 'registry' + (SafGuestPath $registryReady))
    Wait-SafReady $registry 1000 $registryReady
    $peers = @()
    foreach ($uid in @(10601, 10602)) {
      $ready = Join-Path $session.Directory "$uid.ready"
      $peer = Start-SafOwned "peer-$uid" "$Dir/dalvikvm.exe" `
        ($VmArguments + 'gesher.services.SafDocumentsContractTest' + 'relay' + [string]$uid + (SafGuestPath $ready)) $uid
      $peers += @{ Process = $peer; Uid = $uid; Ready = $ready }
    }
    foreach ($peer in $peers) { Wait-SafReady $peer.Process $peer.Uid $peer.Ready }
    return $session
  } catch {
    $startupFailure = $_
    try { Stop-SafBinderFixture $session }
    catch { throw "SAF startup failed: $startupFailure; cleanup failed: $_" }
    throw $startupFailure
  }
}

function Stop-SafBinderFixture($Session) {
  if ($null -eq $Session) { return }
  $failures = [Collections.Generic.List[string]]::new()
  for ($i = $Session.Owned.Count - 1; $i -ge 0; $i--) {
    $entry = $Session.Owned[$i]
    try {
      if ($entry.Process.HasExited) { $failures.Add("Owned SAF $($entry.Name) exited unexpectedly: $($entry.Process.ExitCode)") }
      else { $entry.Process.Kill($true) }
      if (-not $entry.Process.WaitForExit(10000)) { throw "Owned SAF $($entry.Name) did not stop" }
      foreach ($stream in @('Out', 'Err')) {
        if ($null -eq $entry[$stream]) { continue }
        if (-not $entry[$stream].Wait(2000)) { throw "Owned SAF $($entry.Name) output did not drain" }
        $text = $entry[$stream].GetAwaiter().GetResult()
        if ($stream -eq 'Err' -and $entry.ErrorFile) {
          $entry.ErrorFile.Flush()
          $text = Get-Content -LiteralPath $entry.ErrorLog -Raw -ErrorAction Stop
        }
        if ($text) { $Session.Diagnostics.Add("$($entry.Name) ${stream}: $text") }
      }
    } catch { $failures.Add([string]$_) }
    finally {
      try { if ($entry.ErrorFile) { $entry.ErrorFile.Dispose() } }
      catch { $failures.Add('SAF capture disposal: ' + [string]$_) }
      finally { $entry.Process.Dispose() }
    }
  }
  foreach ($name in $Session.Names) {
    try {
      if ($null -eq $Session.Saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
      else { [Environment]::SetEnvironmentVariable($name, $Session.Saved[$name], 'Process') }
    } catch { $failures.Add('SAF environment restore: ' + [string]$_) }
  }
  try { if ($Session.Directory) { Remove-Item -LiteralPath $Session.Directory -Recurse -Force -ErrorAction Stop } }
  catch { $failures.Add('SAF fixture cleanup: ' + [string]$_) }
  foreach ($diagnostic in $Session.Diagnostics) { Write-Host $diagnostic }
  if ($failures.Count -gt 0) { throw ($failures -join "`n") }
}
