# Isolated splash-fixture receipts. Readers only open a completed immutable file:
# Legacy Add-Content's FileShare.Write conflicts with a polling Get-Content
# reader (PowerShell/PowerShell#5924). Never append to a polled file.
function Write-SplashReceipt {
  param(
    [Parameter(Mandatory = $true)][string] $Path,
    [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]] $Lines
  )
  $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
  try {
    [IO.File]::WriteAllLines($temporary, $Lines, [Text.Encoding]::ASCII)
    # Same-directory rename publishes closed, complete bytes. A duplicate writer
    # must fail rather than overwrite another case's or another process's result.
    [IO.File]::Move($temporary, $Path)
  } finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
  }
}

function Get-SplashChildProcess {
  param([Parameter(Mandatory = $true)][string[]] $ParentReceipt)
  $pidRecords = @($ParentReceipt | Where-Object { $_ -match '^child-pid=\d+$' })
  $birthRecords = @($ParentReceipt | Where-Object { $_ -match '^child-start=\d+$' })
  if ($pidRecords.Count -ne 1 -or $birthRecords.Count -ne 1) {
    throw 'Detached splash fixture did not record exactly one child process identity.'
  }
  $childId = [int]$pidRecords[0].Split('=')[1]
  $birth = [long]$birthRecords[0].Split('=')[1]
  if ($childId -le 0 -or $birth -le 0) { throw 'Invalid splash child process identity.' }
  $child = $null
  try {
    try { $child = [Diagnostics.Process]::GetProcessById($childId) }
    catch [ArgumentException] { return $null }  # Already retired.
    $null = $child.Handle  # Retain this kernel process object across PID reuse.
    if ($child.HasExited -or $child.StartTime.ToFileTimeUtc() -ne $birth) {
      $child.Dispose()
      return $null
    }
    return $child
  } catch [InvalidOperationException] {
    # The short-lived child can exit between GetProcessById and opening Handle.
    # Keep real identity/access errors visible; only an observed exit is benign.
    $retired = $false
    if ($child) {
      try { $retired = $child.HasExited } catch { }
      $child.Dispose()
    }
    if ($retired) { return $null }
    throw
  } catch {
    if ($child) { $child.Dispose() }
    throw
  }
}
