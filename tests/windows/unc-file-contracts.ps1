# Actual SMB-backed Java/libcore I/O. The unique share exposes only this owned
# disposable fixture tree to the invoking account, and is removed in finally.
# RunTimed is the bounded process/reader helper supplied by track50.
function Test-UncFileContracts([string] $Dir, [string[]] $VmArguments, [string] $FixtureRoot) {
  $output = [Collections.Generic.List[string]]::new()
  $failed = $false
  $owned = $null
  $shareCreated = $false
  $startedServer = $false
  $shareName = 'gesher-unc-' + [Guid]::NewGuid().ToString('N') + '$'
  $savedEnvironment = @{}
  foreach ($name in @('TMPDIR', 'TMP', 'TEMP')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
  }
  try {
    $server = Get-Service -Name LanmanServer -ErrorAction Stop
    if ($server.Status -eq 'Stopped') {
      Start-Service -Name LanmanServer -ErrorAction Stop
      $startedServer = $true
    } elseif ($server.Status -ne 'Running') {
      throw "SMB fixture needs an available server service: $($server.Status)"
    }
    $owned = Join-Path $FixtureRoot ('unc-files-' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $owned -ErrorAction Stop
    $account = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $null = New-SmbShare -Name $shareName -Path $owned -FullAccess $account `
      -Temporary -EncryptData $true -CachingMode None -ErrorAction Stop
    $shareCreated = $true
    $access = @(Get-SmbShareAccess -Name $shareName -ErrorAction Stop)
    if ($access.Count -ne 1 -or $access[0].AccountName -ne $account -or
        $access[0].AccessControlType -ne 'Allow' -or $access[0].AccessRight -ne 'Full') {
      throw 'Owned SMB fixture has an unexpected share access policy'
    }
    $unc = "\\localhost\$shareName"
    if (-not [IO.Directory]::Exists($unc)) { throw 'Owned localhost SMB share is not reachable' }
    $result = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'gesher.services.UncFileContractTest' + $unc) 90
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0 -or -not $result.Contains('UncFileContractTest all passed:')) {
      throw "Actual UNC file contract failed with exit $code"
    }

    # The default temp path must survive System/File/Path initialization on the
    # same real network share. There is no alternate local path for this VM.
    $tempName = 'Temp קבצים 😀'
    $null = [IO.Directory]::CreateDirectory((Join-Path $owned $tempName))
    [Environment]::SetEnvironmentVariable('TMPDIR', $null, 'Process')
    [Environment]::SetEnvironmentVariable('TMP', "$unc\$tempName", 'Process')
    [Environment]::SetEnvironmentVariable('TEMP', "$unc\$tempName", 'Process')
    $guestTemp = '/mnt/unc/localhost/' + $shareName + '/' + $tempName
    $result = RunTimed "$Dir/dalvikvm.exe" ($VmArguments + 'gesher.services.TempDirectoryContractTest' + 'default' + $guestTemp) 60
    $code = $LASTEXITCODE
    $output.Add($result)
    if ($code -ne 0 -or -not $result.Contains('TempDirectoryContractTest all passed:')) {
      throw "Actual UNC temporary-directory contract failed with exit $code"
    }
    if (@(Get-ChildItem -LiteralPath (Join-Path $owned $tempName) -Force).Count -ne 0) {
      throw 'UNC temporary files survived their real API cleanup'
    }
  } catch {
    $failed = $true
    $output.Add([string]$_)
  } finally {
    foreach ($name in @('TMPDIR', 'TMP', 'TEMP')) {
      [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    try {
      if ($shareCreated) {
        Remove-SmbShare -Name $shareName -Force -Confirm:$false -ErrorAction Stop
        if (Get-SmbShare -Name $shareName -ErrorAction SilentlyContinue) {
          throw 'Owned SMB fixture share survived removal'
        }
      }
    } catch {
      $failed = $true
      $output.Add('UNC share cleanup failed: ' + [string]$_)
    }
    try {
      if ($owned -and (Test-Path -LiteralPath $owned)) {
        Remove-Item -LiteralPath $owned -Recurse -Force -ErrorAction Stop
        if (Test-Path -LiteralPath $owned) { throw 'Owned UNC fixture directory survived cleanup' }
      }
    } catch {
      $failed = $true
      $output.Add('UNC file cleanup failed: ' + [string]$_)
    }
    try {
      if ($startedServer) { Stop-Service -Name LanmanServer -ErrorAction Stop }
    } catch {
      $failed = $true
      $output.Add('SMB fixture service restoration failed: ' + [string]$_)
    }
  }
  if (-not $failed) { $output.Add('UncFileBootstrapTest all passed: real file and temporary-directory I/O; share removed') }
  $global:LASTEXITCODE = if ($failed) { 1 } else { 0 }
  return $output -join "`n"
}
