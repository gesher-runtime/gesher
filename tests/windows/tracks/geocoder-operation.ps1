# All adapter calls execute on the probe's owning PowerShell runspace. The outer
# process deadline contains a blocked adapter call; it cannot stop a provider.
function Invoke-GeocoderOperation($Adapter, [int] $WaitMs, [int] $GraceMs) {
  $terminal = @('Completed', 'Canceled', 'Error')
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
    if ($status -ne 'Completed') {
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
