$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'PID census exit observation is CI only' }
if (-not ('TsfJobSupervisor' -as [type])) { throw 'Load the actual controller before PID census observation' }
foreach ($source in @('TsfJobSupervisor.cs', 'PidCensusExitProbe.cs', 'pid-census-exit-probe.ps1')) {
  $path = if ($source -ceq 'TsfJobSupervisor.cs') { Join-Path $PSScriptRoot '../../../tools/windows4/TsfJobSupervisor.cs' } else { Join-Path $PSScriptRoot $source }
  Write-Host "PID_CENSUS_SOURCE source=$env:GITHUB_SHA name=$source sha256=$((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant())"
}
if (-not ('PidCensusExitProbe' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'PidCensusExitProbe.cs') }
$incomplete = 0
for ($iteration = 0; $iteration -lt 16; $iteration++) {
  $observation = [PidCensusExitProbe]::Run([TsfJobSupervisor], $env:ComSpec, $iteration)
  Write-Host ('PID_CENSUS_EXIT_OBSERVATION ' + ($observation | ConvertTo-Json -Depth 6 -Compress))
  if ($observation.Errors.Count -ne 0 -or -not $observation.Assigned -or -not $observation.Member -or
      -not $observation.RootSignaled -or -not $observation.ThreadClosed -or -not $observation.ProcessClosed -or
      -not $observation.JobClosed -or $observation.Total -ne 1 -or $observation.FinalActive -ne 0 -or
      $observation.ExitCode -ne 0 -or $observation.Milliseconds -ge 5000) {
    throw 'PID census observer failed its actual identity/retirement contract'
  }
  $incomplete += $observation.ControllerIncomplete
}
# Zero observations remain meaningful. They do not prove the archived failure
# external, repaired or impossible. Never retry to force a mismatch or turn the
# actual provider/controller's fail-closed result into success.
Write-Host "PID_CENSUS_EXIT_COMPLETE iterations=16 actual_controller_incomplete=$incomplete original_validation=unchanged"
