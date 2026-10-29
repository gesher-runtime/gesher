# All adapter calls execute on the probe's owning PowerShell runspace. The outer
# process deadline contains a blocked adapter call; it cannot stop a provider.
function Invoke-GeocoderOperation($Adapter, [int] $WaitMs, [int] $GraceMs) {
  $terminal = @('Completed', 'Canceled', 'Error', 'RanToCompletion', 'Faulted')
  $completedStatus = if ($Adapter.CompletedStatus) { $Adapter.CompletedStatus } else { 'Completed' }
  $taskFinished = $false
  try {
    # Waiting on the task handle avoids Wait() throwing on a fault or cancellation.
    $completedInTime = & $Adapter.Wait $WaitMs
    $taskFinished = $completedInTime
    if (-not $completedInTime) {
      $null = & $Adapter.Cancel
      $taskFinished = & $Adapter.Wait $GraceMs
      $status = [string](& $Adapter.Status)
      throw "MapLocationFinder did not complete within 30 seconds (status after cancel: $status)"
    }
    $status = [string](& $Adapter.Status)
    if ($status -ne $completedStatus) {
      $detail = if ($Adapter.Error) { & $Adapter.Error } else { '' }
      throw "MapLocationFinder ended with status ${status}: $detail"
    }
    return (& $Adapter.Result)
  } finally {
    # The AsTask completion machinery may still use the operation even after
    # WinRT reports a terminal status. Fail closed until both are terminal.
    # A blocked call remains contained by the outer process deadline.
    if ($taskFinished) {
      $statusForClose = [string](& $Adapter.Status)
      if ($terminal -contains $statusForClose) { $null = & $Adapter.Close }
    }
  }
}

# The observed Windows PowerShell operation.Status was empty. Use the
# documented AsTask cancellation overload: its token reaches IAsyncInfo.Cancel.
# TaskStatus and MapLocationFinderResult.Status are separate contracts.
function New-WinRTGeocoderAdapter($Operation, [Type] $ResultType) {
  Add-Type -AssemblyName System.Runtime.WindowsRuntime
  $method = @([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and
    $_.GetGenericArguments().Count -eq 1 -and $_.GetParameters().Count -eq 2 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' -and
    $_.GetParameters()[1].ParameterType -eq [Threading.CancellationToken]
  })
  if ($method.Count -ne 1) { throw 'Expected exactly one cancellable IAsyncOperation AsTask overload' }
  $cancel = [Threading.CancellationTokenSource]::new()
  $task = $method[0].MakeGenericMethod($ResultType).Invoke($null, @($Operation, $cancel.Token))
  $lifetime = @{ Handle = ([System.IAsyncResult]$task).AsyncWaitHandle; Disposed = $false; ResultReads = 0 }
  return @{
    CompletedStatus = 'RanToCompletion'
    Wait = { param($ms) $lifetime.Handle.WaitOne($ms) }.GetNewClosure()
    Cancel = { $cancel.Cancel() }.GetNewClosure()
    Status = { $task.Status.ToString() }.GetNewClosure()
    Error = { if ($task.Exception) { $task.Exception.ToString() } }.GetNewClosure()
    Result = { $lifetime.ResultReads++; $task.GetAwaiter().GetResult() }.GetNewClosure()
    Close = {
      if (-not $lifetime.Disposed) {
        if (-not $task.IsCompleted) { throw 'Cannot dispose a pending WinRT task' }
        $lifetime.Handle.Dispose(); $cancel.Dispose(); $lifetime.Disposed = $true
      }
      # Do not call operation.Close while its AsTask callback may still run.
      # The isolated probe process owns the COM projection until retirement.
    }.GetNewClosure()
    Task = $task
    Lifetime = $lifetime
  }
}

# Hash directly through .NET: an owned Windows PowerShell child cannot assume
# that the parent's PSModulePath provides the Get-FileHash script module.
function Get-GeocoderSourceSha256([string] $Path) {
  $stream = $null
  $hasher = $null
  try {
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
  } finally {
    if ($hasher) { $hasher.Dispose() }
    if ($stream) { $stream.Dispose() }
  }
}

# Runtime loads the exact producer bytes. Never invoke CodeDOM in an owned
# runtime child; the unchanged supervisor must reject compiler descendants.
function Import-GeocoderProbeType([string] $Source) {
  $manifest = $env:GESHER_GEOCODER_RECEIPT
  if (-not $manifest) { throw 'Compiled helper receipt absent' }
  $manifestBytes = [IO.File]::ReadAllBytes($manifest)
  $manifestHasher = [Security.Cryptography.SHA256]::Create()
  try { $manifestHash = [BitConverter]::ToString($manifestHasher.ComputeHash($manifestBytes)).Replace('-', '').ToLowerInvariant() }
  finally { $manifestHasher.Dispose() }
  if ($manifestHash -cne $env:GESHER_GEOCODER_RECEIPT_SHA256) { throw 'Compiled helper receipt mismatch' }
  Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
  $receipt = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
  if ($receipt.Repository -cne $env:GITHUB_REPOSITORY -or $receipt.RunId -cne $env:GITHUB_RUN_ID -or $receipt.RunAttempt -cne $env:GITHUB_RUN_ATTEMPT -or $receipt.Role -cne 'geocoder-helper-compiler') { throw 'Compiled helper producer context mismatch' }
  if ($receipt.SourceRevision -cne $env:GITHUB_SHA) { throw 'Compiled helper revision mismatch' }
  $sourcePath = [IO.Path]::GetFullPath($Source)
  $entry = @($receipt.Sources | Where-Object { $_.Path -ceq $sourcePath })
  if ($entry.Count -ne 1 -or $entry[0].Sha256 -cne (Get-GeocoderSourceSha256 $sourcePath)) { throw 'Compiled helper source mismatch' }
  foreach ($dependency in @($receipt.Sources) + @($receipt.References)) {
    if ((Get-GeocoderSourceSha256 $dependency.Path) -cne $dependency.Sha256) { throw 'Compiled helper input mismatch' }
  }
  if ((Get-GeocoderSourceSha256 $receipt.Compiler) -cne $receipt.CompilerSha256 -or [Diagnostics.FileVersionInfo]::GetVersionInfo($receipt.Compiler).FileVersion -cne $receipt.CompilerVersion) { throw 'Compiled helper compiler mismatch' }
  # Hash and load the SAME byte array, avoiding a check/load file race.
  $bytes = [IO.File]::ReadAllBytes($receipt.Output)
  $hasher = [Security.Cryptography.SHA256]::Create()
  try { $actual = [BitConverter]::ToString($hasher.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() }
  finally { $hasher.Dispose() }
  if ($actual -cne $receipt.OutputSha256) { throw 'Compiled helper DLL mismatch' }
  $assembly = [Reflection.Assembly]::Load($bytes)
  Write-Host "WINRT_RUNTIME_LOAD source_sha=$env:GITHUB_SHA dll_sha256=$actual assembly=$($assembly.FullName) receipt_sha256=$env:GESHER_GEOCODER_RECEIPT_SHA256 compilation=False ps=$($PSVersionTable.PSVersion) clr=$([Environment]::Version)"
}
