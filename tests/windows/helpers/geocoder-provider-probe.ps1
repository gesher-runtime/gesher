param([string] $HelperPath = (Join-Path $PSScriptRoot '../tracks/geocoder-operation.ps1'))
$ErrorActionPreference = 'Stop'
. $HelperPath
# Environment.Exit still runs CLR/WinRT shutdown and can deadlock after the
# completed answer. This isolated probe owns only its current process.
Import-GeocoderProbeType (Join-Path $PSScriptRoot '../tracks/geocoder-probe-exit.cs')
Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Services.Maps.MapLocationFinder, Windows.Services.Maps, ContentType = WindowsRuntime] | Out-Null
try {
  $operation = [Windows.Services.Maps.MapLocationFinder]::FindLocationsAsync('Jerusalem', $null)
  $adapter = New-WinRTGeocoderAdapter $operation ([Windows.Services.Maps.MapLocationFinderResult])
  $result = Invoke-GeocoderOperation $adapter 30000 5000
  [Console]::Out.WriteLine('WINRT_PROVIDER task_status=' + $adapter.Task.Status + ' result_type=' + $result.GetType().FullName + ' result_reads=' + $adapter.Lifetime.ResultReads + ' resources_disposed=' + $adapter.Lifetime.Disposed)
  [Console]::Out.WriteLine('status: ' + $result.Status)
  [Console]::Out.Flush()
  # WinRT/COM teardown can keep PowerShell alive after the genuine answer.
  [GesherGeocoderProbeExit]::Exit(0)
} catch {
  [Console]::Out.WriteLine('error: ' + $_.Exception.Message)
  [Console]::Out.Flush()
  [GesherGeocoderProbeExit]::Exit(1)
}
