$ErrorActionPreference = 'Stop'
# QA218-OBSERVATION-BEGIN fixture-entry

if (-not $env:GESHER_QA218_PHASE_DIR) { throw 'Owned diagnostic phase directory absent' }
[IO.File]::WriteAllText([IO.Path]::Combine($env:GESHER_QA218_PHASE_DIR, ($PID.ToString() + '-fixture-entry.txt')), ($PID.ToString() + '|' + [Diagnostics.Stopwatch]::GetTimestamp().ToString() + '|' + [Diagnostics.Stopwatch]::Frequency.ToString() + '|' + [DateTime]::UtcNow.ToString('o') + '|fixture-entry|'))
# QA218-OBSERVATION-END fixture-entry
. (Join-Path $PSScriptRoot '../tracks/geocoder-operation.ps1')
# QA218-OBSERVATION-BEGIN fixture-dot-source-end
Write-GeocoderDiagnosticPhase 'fixture-dot-source-end'
Write-GeocoderDiagnosticPhase 'fixture-add-type-begin'
# QA218-OBSERVATION-END fixture-dot-source-end
Add-Type -AssemblyName System.Runtime.WindowsRuntime
# QA218-OBSERVATION-BEGIN fixture-add-type-end
Write-GeocoderDiagnosticPhase 'fixture-add-type-end'
# QA218-OBSERVATION-END fixture-add-type-end
$source = Join-Path $PSScriptRoot '../tracks/geocoder-winrt-supplier.cs'
# QA218-OBSERVATION-BEGIN fixture-import-begin
Write-GeocoderDiagnosticPhase 'fixture-import-begin'
# QA218-OBSERVATION-END fixture-import-begin
Import-GeocoderProbeType $source
# QA218-OBSERVATION-BEGIN fixture-import-end
Write-GeocoderDiagnosticPhase 'fixture-import-end'
# QA218-OBSERVATION-END fixture-import-end
$run = @([System.Runtime.InteropServices.WindowsRuntime.AsyncInfo].GetMethods() | Where-Object {
  $_.Name -eq 'Run' -and $_.IsGenericMethodDefinition -and $_.GetGenericArguments().Count -eq 1 -and
  $_.GetParameters().Count -eq 1 -and $_.ReturnType.Name -eq 'IAsyncOperation`1'
})
if ($run.Count -ne 1) { throw 'Expected one AsyncInfo.Run TResult factory' }
foreach ($case in @('success', 'error', 'timeout', 'late')) {
  $supplier = [ControlledWinRTSupplier]::new()
  $operation = $run[0].MakeGenericMethod([int]).Invoke($null, @($supplier.Factory))
  $adapter = New-WinRTGeocoderAdapter $operation ([int])
  $originalWait = $adapter.Wait
  if ($case -in @('success', 'error')) { $supplier.Complete(($case -eq 'error')) }
  else {
    # Inject only the deadline decision; task conversion and cancellation are real.
    $deadline = @{ First = $true }
    $adapter.Wait = { param($ms) if ($deadline.First) { $deadline.First = $false; return $false }; & $originalWait $ms }
    $originalCancel = $adapter.Cancel
    $adapter.Cancel = {
      if ($case -eq 'late') {
        $supplier.Complete($false)
        if (-not (& $originalWait 5000) -or $adapter.Task.Status -ne 'RanToCompletion') { throw 'Late bridge completion barrier failed' }
        Write-Host 'WINRT_ORDER case=late bridge_completed_before_cancel=True'
      }
      & $originalCancel
    }
  }
  $failed = $false
  try { $answer = Invoke-GeocoderOperation $adapter 100 5000 }
  catch { $failed = $true; $failure = $_.Exception.Message }
  if ($case -eq 'success') {
    if ($failed -or $answer -ne 42 -or $adapter.Task.Status -ne 'RanToCompletion' -or $adapter.Lifetime.ResultReads -ne 1) { throw "WinRT success: $failure" }
  } else {
    if (-not $failed -or $adapter.Lifetime.ResultReads -ne 0) { throw "WinRT $case accepted or read result" }
    if ($case -eq 'error' -and ($adapter.Task.Status -ne 'Faulted' -or $failure -notmatch 'controlled WinRT error')) { throw "WinRT error: $failure" }
    if ($case -in @('timeout', 'late') -and $failure -notmatch 'did not complete') { throw "WinRT deadline changed: $failure" }
    if ($case -eq 'timeout' -and ($supplier.CancelObserved -ne 1 -or $adapter.Task.Status -ne 'Canceled')) { throw 'AsTask token did not cancel underlying AsyncInfo supplier' }
    if ($case -eq 'late' -and ($adapter.Task.Status -ne 'RanToCompletion' -or $supplier.CancelObserved -ne 0 -or $supplier.CompletionCalls -ne 1)) { throw 'Late completion lost' }
  }
  if (-not $adapter.Task.IsCompleted) { throw 'WinRT bridge did not reach terminal state' }
  & $adapter.Close; & $adapter.Close
  $supplier.Dispose()
  if (-not $adapter.Lifetime.Disposed -or -not $supplier.Disposed) { throw 'WinRT cleanup missing' }
  Write-Host "WINRT_ADAPTER case=$case operation_type=$($operation.GetType().FullName) task_status=$($adapter.Task.Status) cancel_observed=$($supplier.CancelObserved) result_reads=$($adapter.Lifetime.ResultReads) disposed=$($adapter.Lifetime.Disposed) supplier_disposed=$($supplier.Disposed)"
}
