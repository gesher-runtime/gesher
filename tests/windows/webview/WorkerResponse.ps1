# Real service-worker response qualification with an independent HTTP origin.
# A valid diagnostic can find that stream failures do NOT reject fetch; it does
# not enable or advertise Android service-worker support.
function WorkerResponseCheck {
  if (-not (Test-Path "$Dir/webview2_probe.exe")) {
    Write-Host 'FAIL  worker-response: probe binary is missing'; $script:failed++; return
  }
  $output = RunTimed "$Dir/webview2_probe.exe" @('--worker-response') 140
  $exit = $LASTEXITCODE
  ($output -split "`n" | Where-Object { $_ -match '^(PASS|FAIL|INFO) ' }) | Out-Host
  $unavailable = [regex]::Matches($output, '(?m)^INFO  worker-response: unavailable: SERVICE_WORKER filter interface absent; cases not run; no production worker capability enabled\r?$').Count
  $recorded = [regex]::Matches($output, '(?m)^PASS  worker-response: controls and independent network counters verified; error outcomes recorded; no production worker capability enabled\r?$').Count
  if ($exit -eq 2 -and $unavailable -eq 1 -and $recorded -eq 0 -and $output -notmatch '(?m)^FAIL ') {
    Write-Host 'INFO  worker-response: source-filter interface unavailable; worker qualification remains incomplete'
    return
  }
  $patterns = @(foreach ($name in 'pass', 'fulfill', 'http403', 'first-read', 'first-abort', 'mid-body') {
    "(?m)^INFO  worker-response: request case=$name source=SERVICE_WORKER\r?`$"
    $network = if ($name -eq 'pass') { 1 } else { 0 }
    "(?m)^INFO  worker-response: case=$name intercepted=1 origin-network=$network\r?`$"
  })
  foreach ($name in 'first-read', 'first-abort', 'mid-body') {
    $hr = if ($name -eq 'first-abort') { '80004004' } else { '8003001e' }
    $prefix = if ($name -eq 'mid-body') { '[1-9][0-9]*' } else { '0' }
    $patterns += "(?m)^INFO  worker-response: case=$name read-hr=0x$hr reads=[1-9][0-9]* faults=[1-9][0-9]* supplied-prefix=$prefix live-streams=[0-9]+ pre-response-rejection=(yes|no)\r?`$"
  }
  $missing = @($patterns | Where-Object { [regex]::Matches($output, $_).Count -ne 1 })
  if ($exit -ne 0 -or $recorded -ne 1 -or $unavailable -ne 0 -or $missing.Count -ne 0 -or $output -match '(?m)^FAIL ') {
    Write-Host "FAIL  worker-response: diagnostic failed (exit=$exit recorded=$recorded unavailable=$unavailable missing=$($missing.Count))"
    Write-Host $output
    $script:failed++
  } else {
    Write-Host 'INFO  worker-response: real error outcomes recorded; production worker support remains gated'
  }
}

WorkerResponseCheck
