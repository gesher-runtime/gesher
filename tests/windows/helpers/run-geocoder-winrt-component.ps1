$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'WinRT qualification is CI only' }
. (Join-Path $PSScriptRoot 'geocoder-compile.ps1')
try {
New-GeocoderProbeAssembly
$exe = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
foreach ($case in @('fixture', 'provider')) {
  $file = if ($case -eq 'fixture') { 'geocoder-winrt-fixture.ps1' } else { 'geocoder-provider-probe.ps1' }
  $path = Join-Path $PSScriptRoot $file
  $r = [TsfJobSupervisor]::Run($exe, ('-NoProfile -ExecutionPolicy Bypass -File "' + $path + '"'), 45000, '')
  Assert-GeocoderOwnedJob $r $exe $case
  Write-Host $r.Output
  if ($case -eq 'fixture') {
    foreach ($name in @('success','error','timeout','late')) {
      if ($r.Output -notmatch "WINRT_ADAPTER case=$name .*disposed=True supplier_disposed=True") { throw "WinRT $name trace missing" }
    }
    if ($r.Output -notmatch 'WINRT_ORDER case=late bridge_completed_before_cancel=True') { throw 'WinRT completion ordering missing' }
  } else {
    if ($r.Output -notmatch '(?m)^status: [A-Za-z]+\r?$' -or $r.Output -notmatch 'WINRT_PROVIDER task_status=RanToCompletion result_type=Windows.Services.Maps.MapLocationFinderResult result_reads=1 resources_disposed=True') { throw 'Actual Maps provider result missing' }
  }
  Write-Host "PASS WinRT $case with actual bridge and owned child retirement"
}
} finally { Remove-GeocoderProbeAssembly }
