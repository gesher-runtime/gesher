# All adapter calls execute on the probe's owning PowerShell runspace. The outer
# process deadline contains a blocked adapter call; it cannot stop a provider.
function Invoke-GeocoderOperation($Adapter, [int] $WaitMs, [int] $GraceMs) {
  $terminal = @('Completed', 'Canceled', 'Error')
  try {
    # Waiting on the task handle avoids Wait() throwing on a fault or cancellation.
    $completedInTime = $Adapter.Wait.Invoke($WaitMs)
    if (-not $completedInTime) {
      $Adapter.Cancel.Invoke()
      $null = $Adapter.Wait.Invoke($GraceMs)
      $status = [string]$Adapter.Status.Invoke()
      throw "MapLocationFinder did not complete within 30 seconds (status after cancel: $status)"
    }
    $status = [string]$Adapter.Status.Invoke()
    if ($status -ne 'Completed') { throw "MapLocationFinder ended with status $status" }
    return $Adapter.Result.Invoke()
  } finally {
    # Never close an operation still reported Started. If Cancel/Status/Close
    # blocks, the outer process deadline remains the containment boundary.
    $statusForClose = [string]$Adapter.Status.Invoke()
    if ($terminal -contains $statusForClose) { $Adapter.Close.Invoke() }
  }
}
