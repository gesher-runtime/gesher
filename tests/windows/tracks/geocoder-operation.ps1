# QA218-OBSERVATION-BEGIN phase-writer
function Write-GeocoderDiagnosticPhase([string] $Phase, [string] $Detail = '') {
  if (-not $env:GESHER_QA218_PHASE_DIR) { throw 'Owned diagnostic phase directory absent' }
  if ($Phase -notmatch '^[a-zA-Z0-9_.-]{1,100}$' -or $Detail.Length -gt 128) { throw 'Invalid diagnostic phase' }
  $line = $PID.ToString() + '|' + [Diagnostics.Stopwatch]::GetTimestamp().ToString() + '|' + [Diagnostics.Stopwatch]::Frequency.ToString() + '|' + [DateTime]::UtcNow.ToString('o') + '|' + $Phase + '|' + $Detail
  # One bounded, closed .NET write per phase. No stdout/stderr or retained handle.
  [IO.File]::WriteAllText([IO.Path]::Combine($env:GESHER_QA218_PHASE_DIR, ($PID.ToString() + '-' + $Phase + '.txt')), $line)
}
# QA218-OBSERVATION-END phase-writer
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
# QA218-OBSERVATION-BEGIN import-entry
  Write-GeocoderDiagnosticPhase 'import-entry'
# QA218-OBSERVATION-END import-entry
  $manifest = $env:GESHER_GEOCODER_RECEIPT
  if (-not $manifest) { throw 'Compiled helper receipt absent' }
# QA218-OBSERVATION-BEGIN receipt-read-begin
  Write-GeocoderDiagnosticPhase 'receipt-read-begin'
# QA218-OBSERVATION-END receipt-read-begin
  $manifestBytes = [IO.File]::ReadAllBytes($manifest)
# QA218-OBSERVATION-BEGIN receipt-read-end
  Write-GeocoderDiagnosticPhase 'receipt-read-end'
# QA218-OBSERVATION-END receipt-read-end
# QA218-OBSERVATION-BEGIN receipt-hash-begin
  Write-GeocoderDiagnosticPhase 'receipt-hash-begin'
# QA218-OBSERVATION-END receipt-hash-begin
  $manifestHasher = [Security.Cryptography.SHA256]::Create()
  try { $manifestHash = [BitConverter]::ToString($manifestHasher.ComputeHash($manifestBytes)).Replace('-', '').ToLowerInvariant() }
  finally { $manifestHasher.Dispose() }
# QA218-OBSERVATION-BEGIN receipt-hash-end
  Write-GeocoderDiagnosticPhase 'receipt-hash-end'
# QA218-OBSERVATION-END receipt-hash-end
  if ($manifestHash -cne $env:GESHER_GEOCODER_RECEIPT_SHA256) { throw 'Compiled helper receipt mismatch' }
# QA218-OBSERVATION-BEGIN utility-import-begin
  Write-GeocoderDiagnosticPhase 'utility-import-begin'
# QA218-OBSERVATION-END utility-import-begin
  Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
# QA218-OBSERVATION-BEGIN utility-import-end
  Write-GeocoderDiagnosticPhase 'utility-import-end'
# QA218-OBSERVATION-END utility-import-end
# QA218-OBSERVATION-BEGIN receipt-json-begin
  Write-GeocoderDiagnosticPhase 'receipt-json-begin'
# QA218-OBSERVATION-END receipt-json-begin
  $receipt = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
# QA218-OBSERVATION-BEGIN receipt-json-end
  Write-GeocoderDiagnosticPhase 'receipt-json-end'
# QA218-OBSERVATION-END receipt-json-end
  if ($receipt.Repository -cne $env:GITHUB_REPOSITORY -or $receipt.RunId -cne $env:GITHUB_RUN_ID -or $receipt.RunAttempt -cne $env:GITHUB_RUN_ATTEMPT -or $receipt.Role -cne 'geocoder-helper-compiler') { throw 'Compiled helper producer context mismatch' }
# QA218-OBSERVATION-BEGIN producer-context-begin
  Write-GeocoderDiagnosticPhase 'producer-context-begin'
# QA218-OBSERVATION-END producer-context-begin
  if ($receipt.SourceRevision -cne $env:GITHUB_SHA) { throw 'Compiled helper revision mismatch' }
# QA218-OBSERVATION-BEGIN producer-context-end
  Write-GeocoderDiagnosticPhase 'producer-context-end'
# QA218-OBSERVATION-END producer-context-end
# QA218-OBSERVATION-BEGIN source-validation-begin
  Write-GeocoderDiagnosticPhase 'source-validation-begin'
# QA218-OBSERVATION-END source-validation-begin
  $sourcePath = [IO.Path]::GetFullPath($Source)
  $entry = @($receipt.Sources | Where-Object { $_.Path -ceq $sourcePath })
  if ($entry.Count -ne 1 -or $entry[0].Sha256 -cne (Get-GeocoderSourceSha256 $sourcePath)) { throw 'Compiled helper source mismatch' }
# QA218-OBSERVATION-BEGIN source-validation-end
  Write-GeocoderDiagnosticPhase 'source-validation-end'
# QA218-OBSERVATION-END source-validation-end
  foreach ($dependency in @($receipt.Sources) + @($receipt.References) + @($receipt.Compilers)) {
# QA218-OBSERVATION-BEGIN dependency-begin
    Write-GeocoderDiagnosticPhase ('input-' + [IO.Path]::GetFileName($dependency.Path) + '-begin')
# QA218-OBSERVATION-END dependency-begin
    if ((Get-GeocoderSourceSha256 $dependency.Path) -cne $dependency.Sha256) { throw 'Compiled helper input mismatch' }
# QA218-OBSERVATION-BEGIN dependency-end
    Write-GeocoderDiagnosticPhase ('input-' + [IO.Path]::GetFileName($dependency.Path) + '-end')
# QA218-OBSERVATION-END dependency-end
  }
# QA218-OBSERVATION-BEGIN compiler-identity-begin
  Write-GeocoderDiagnosticPhase 'compiler-identity-begin'
# QA218-OBSERVATION-END compiler-identity-begin
  if ((Get-GeocoderSourceSha256 $receipt.Compiler) -cne $receipt.CompilerSha256 -or [Diagnostics.FileVersionInfo]::GetVersionInfo($receipt.Compiler).FileVersion -cne $receipt.CompilerVersion) { throw 'Compiled helper compiler mismatch' }
# QA218-OBSERVATION-BEGIN compiler-identity-end
  Write-GeocoderDiagnosticPhase 'compiler-identity-end'
# QA218-OBSERVATION-END compiler-identity-end
# QA218-OBSERVATION-BEGIN roslyn-worker-begin
  Write-GeocoderDiagnosticPhase 'roslyn-worker-begin'
# QA218-OBSERVATION-END roslyn-worker-begin
  if ((Get-GeocoderSourceSha256 $receipt.Worker) -cne $receipt.WorkerSha256 -or $receipt.Compilers.Count -ne 2) { throw 'Compiled helper Roslyn worker mismatch' }
# QA218-OBSERVATION-BEGIN roslyn-worker-end
  Write-GeocoderDiagnosticPhase 'roslyn-worker-end'
# QA218-OBSERVATION-END roslyn-worker-end
  # Hash and load the SAME byte array, avoiding a check/load file race.
# QA218-OBSERVATION-BEGIN dll-read-begin
  Write-GeocoderDiagnosticPhase 'dll-read-begin'
# QA218-OBSERVATION-END dll-read-begin
  $bytes = [IO.File]::ReadAllBytes($receipt.Output)
# QA218-OBSERVATION-BEGIN dll-read-end
  Write-GeocoderDiagnosticPhase 'dll-read-end'
# QA218-OBSERVATION-END dll-read-end
# QA218-OBSERVATION-BEGIN dll-hash-begin
  Write-GeocoderDiagnosticPhase 'dll-hash-begin'
# QA218-OBSERVATION-END dll-hash-begin
  $hasher = [Security.Cryptography.SHA256]::Create()
  try { $actual = [BitConverter]::ToString($hasher.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() }
  finally { $hasher.Dispose() }
# QA218-OBSERVATION-BEGIN dll-hash-end
  Write-GeocoderDiagnosticPhase 'dll-hash-end'
# QA218-OBSERVATION-END dll-hash-end
  if ($actual -cne $receipt.OutputSha256) { throw 'Compiled helper DLL mismatch' }
# QA218-OBSERVATION-BEGIN assembly-load-begin
  Write-GeocoderDiagnosticPhase 'assembly-load-begin' $actual
# QA218-OBSERVATION-END assembly-load-begin
  $assembly = [Reflection.Assembly]::Load($bytes)
# QA218-OBSERVATION-BEGIN assembly-load-end
  Write-GeocoderDiagnosticPhase 'assembly-load-end'
# QA218-OBSERVATION-END assembly-load-end
# QA218-OBSERVATION-BEGIN runtime-output-begin
  Write-GeocoderDiagnosticPhase 'runtime-output-begin'
# QA218-OBSERVATION-END runtime-output-begin
  Write-Host "WINRT_RUNTIME_LOAD source_sha=$env:GITHUB_SHA dll_sha256=$actual assembly=$($assembly.FullName) receipt_sha256=$env:GESHER_GEOCODER_RECEIPT_SHA256 compilation=False ps=$($PSVersionTable.PSVersion) clr=$([Environment]::Version)"
# QA218-OBSERVATION-BEGIN runtime-output-end
  Write-GeocoderDiagnosticPhase 'runtime-output-end'
# QA218-OBSERVATION-END runtime-output-end
}
