# Dot-source and call Test-TemporaryDirectoryContracts with track94's ordinary
# VM arguments. Each scenario starts a fresh VM; no fixture rewrites java.io
# temp caches or changes the default directory after its first use.
. "$PSScriptRoot/helpers/CloseProcessReaders.ps1"

function Test-TemporaryDirectoryContracts([string] $Dir, [string[]] $VmArguments) {
  $output = [Collections.Generic.List[string]]::new()
  $failed = $false
  $owned = $null
  $denied = $null
  $originalAcl = $null
  $scenarios = 0
  function GuestTemporaryPath([string] $WindowsPath) {
    $full = [IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^([A-Za-z]):[\\/]') { throw "Fixture root is not a local drive path: $full" }
    return '/mnt/' + $Matches[1].ToLowerInvariant() + ($full.Substring(2) -replace '\\', '/').TrimEnd('/')
  }
  function TempProcess($StartInfo) {
    $process = $null
    $nativeHandle = $null
    $cancellation = [Threading.CancellationTokenSource]::new()
    $pipes = @(@{ Name = 'stdout'; Reader = $null; Handle = $null; Raw = [IntPtr]::Zero; Task = $null; Copied = $false },
               @{ Name = 'stderr'; Reader = $null; Handle = $null; Raw = [IntPtr]::Zero; Task = $null; Copied = $false })
    $text = ''
    $incomplete = $true
    $nativeExit = $null
    try {
      $process = [Diagnostics.Process]::Start($StartInfo)
      $nativeHandle = $process.SafeHandle
      foreach ($pipe in $pipes) {
        $pipe.Reader = if ($pipe.Name -eq 'stdout') { $process.StandardOutput } else { $process.StandardError }
        $pipe.Handle = $pipe.Reader.BaseStream.SafeFileHandle
        $pipe.Raw = $pipe.Handle.DangerousGetHandle()
        $pipe.Task = $pipe.Reader.ReadToEndAsync($cancellation.Token)
      }
      $incomplete = -not $process.WaitForExit(60000)
      if ($incomplete) {
        $text += "`n(temporary-directory child timed out after 60 seconds)"
        try { $process.Kill($true) } catch { $text += "`n(process kill failed: $_)" }
        if (-not $process.WaitForExit(5000)) { $text += "`n(process alive five seconds after kill)" }
      }
      foreach ($pipe in $pipes) {
        try {
          if ($pipe.Task.Wait(5000)) { $text += $pipe.Task.Result; $pipe.Copied = $true }
          else { $incomplete = $true; $text += "`n($($pipe.Name) EOF exceeded five seconds)" }
        } catch { $incomplete = $true; $text += "`n($($pipe.Name) read failed: $_)" }
      }
      if ($process.HasExited) { $nativeExit = $process.ExitCode }
    } finally {
      $cleanup = @{ LateText = ''; TasksComplete = $false; PipesClosed = $false; Problem = 'reader cleanup did not complete' }
      try { $cleanup = Close-ProcessReaders $cancellation $pipes }
      catch { $incomplete = $true; $text += "`n(reader cleanup failed: $_)" }
      $text += $cleanup.LateText
      if (-not $cleanup.TasksComplete -or -not $cleanup.PipesClosed -or $cleanup.Problem) {
        $incomplete = $true
        $text += "`n(reader cleanup incomplete: $($cleanup.Problem))"
      }
      if ($null -ne $process) {
        try {
          if (-not $process.HasExited) {
            $process.Kill($true)
            if (-not $process.WaitForExit(5000)) { $incomplete = $true; $text += "`n(final process cleanup exceeded five seconds)" }
          }
        } catch { $incomplete = $true; $text += "`n(final process cleanup failed: $_)" }
        try { $process.Dispose() } catch { $incomplete = $true; $text += "`n(process disposal failed: $_)" }
        if ($null -ne $nativeHandle -and -not $nativeHandle.IsClosed) {
          $incomplete = $true
          $text += "`n(native process handle did not close)"
        }
      }
    }
    return [pscustomobject]@{ Output = $text; ExitCode = $(if ($incomplete) { -1 } else { $nativeExit }) }
  }
  function TempChild([string] $HostTemp, [string] $Mode, [string] $Expected,
      [hashtable] $Environment = @{}, [string[]] $Options = @(), [string[]] $Extra = @()) {
    $psi = [Diagnostics.ProcessStartInfo]::new([IO.Path]::GetFullPath((Join-Path $Dir 'dalvikvm.exe')))
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $null = $psi.Environment.Remove('TMPDIR')
    $psi.Environment['TMP'] = $HostTemp
    $psi.Environment['TEMP'] = $HostTemp
    foreach ($key in $Environment.Keys) { $psi.Environment[$key] = [string]$Environment[$key] }
    foreach ($argument in @($VmArguments) + @($Options) + @('gesher.services.TempDirectoryContractTest', $Mode, $Expected) + @($Extra)) {
      $psi.ArgumentList.Add([string]$argument)
    }
    $result = TempProcess $psi
    if ($result.ExitCode -ne 0 -or $result.Output -notmatch 'TempDirectoryContractTest all passed:') {
      throw "Temporary-directory $Mode scenario exited $($result.ExitCode):`n$($result.Output)"
    }
    foreach ($line in ($result.Output -split "`r?`n" | Where-Object { $_ -like 'TempDirectoryContractTest*' })) {
      $output.Add($line)
    }
  }
  try {
    $local = [Environment]::GetFolderPath('LocalApplicationData')
    if (-not $local) { throw 'No real user-local directory for temporary-directory contracts' }
    $owned = Join-Path $local ('gesher-temp-contract-' + [Guid]::NewGuid().ToString('N'))
    $hostTemp = Join-Path $owned 'Temp קבצים 😀'
    $override = Join-Path $owned 'Override קבצים 😀'
    $cache = Join-Path $owned 'app-cache'
    $denied = Join-Path $owned 'denied'
    foreach ($path in @($owned, $hostTemp, $override, $cache, $denied)) {
      $null = New-Item -ItemType Directory -Path $path
    }
    $missing = Join-Path $owned 'does-not-exist'
    $notDirectory = Join-Path $owned 'ordinary-file'
    [IO.File]::WriteAllText($notDirectory, 'preserve this file')
    $guestTemp = GuestTemporaryPath $hostTemp
    $guestOverride = GuestTemporaryPath $override
    $guestMissing = GuestTemporaryPath $missing
    $guestFile = GuestTemporaryPath $notDirectory
    $output.Add('TempDirectoryContractTest host drive=' + [IO.Path]::GetPathRoot($hostTemp) +
        ' VM drive=' + [IO.Path]::GetPathRoot([IO.Path]::GetFullPath((Join-Path $Dir 'dalvikvm.exe'))))

    TempChild $hostTemp 'default' $guestTemp -Extra @('-Djava.io.tmpdir=/application-argument')
    $scenarios++
    TempChild $hostTemp 'default' $guestOverride -Environment @{ TMPDIR = $guestOverride }
    $scenarios++
    TempChild $missing 'explicit' $guestOverride -Options @("-Djava.io.tmpdir=$guestOverride")
    $scenarios++
    TempChild $missing 'cache' $guestMissing -Extra @(GuestTemporaryPath $cache)
    $scenarios++
    TempChild $missing 'missing' $guestMissing
    $scenarios++
    TempChild $hostTemp 'missing' $guestMissing -Environment @{ TMPDIR = $guestMissing }
    $scenarios++
    TempChild $hostTemp 'missing' $guestMissing -Options @("-Djava.io.tmpdir=$guestMissing")
    $scenarios++
    TempChild $notDirectory 'not-directory' $guestFile
    $scenarios++
    TempChild $hostTemp 'not-directory' $guestFile -Options @("-Djava.io.tmpdir=$guestFile")
    $scenarios++

    # Deny only creation in a disposable directory owned by this test user.
    # Restore the exact original ACL before removing the owned fixture tree.
    $originalAcl = Get-Acl -LiteralPath $denied
    $restricted = Get-Acl -LiteralPath $denied
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $rights = [Security.AccessControl.FileSystemRights]::CreateFiles -bor [Security.AccessControl.FileSystemRights]::CreateDirectories
    $rule = [Security.AccessControl.FileSystemAccessRule]::new($identity, $rights, 'None', 'None', 'Deny')
    $restricted.AddAccessRule($rule)
    Set-Acl -LiteralPath $denied -AclObject $restricted
    TempChild $denied 'denied' (GuestTemporaryPath $denied)
    $scenarios++
    if (Test-Path -LiteralPath $missing) { throw 'A failing temporary-directory case created its missing parent' }
    if ([IO.File]::ReadAllText($notDirectory) -ne 'preserve this file') { throw 'Temporary-directory failure modified an existing file' }
  } catch {
    $failed = $true
    $output.Add([string]$_)
  } finally {
    try {
      if ($originalAcl) { Set-Acl -LiteralPath $denied -AclObject $originalAcl }
      if ($owned -and (Test-Path -LiteralPath $owned)) { Remove-Item -LiteralPath $owned -Recurse -Force }
    } catch {
      $failed = $true
      $output.Add('Temporary-directory fixture cleanup failed: ' + [string]$_)
    }
  }
  if (-not $failed) { $output.Add("TempDirectoryBootstrapTest all passed: $scenarios fresh VM scenarios") }
  $global:LASTEXITCODE = if ($failed) { 1 } else { 0 }
  return $output -join "`n"
}
