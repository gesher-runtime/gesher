# Real service-worker response qualification with an independent HTTP origin.
# A valid diagnostic can find that stream failures or denial candidates do NOT
# reject fetch. An observed rejection is one bounded primitive observation, not
# Android service-worker support or proof of caching/streaming/lifecycle parity.
function WorkerResponseCheck {
  if (-not (Test-Path "$Dir/webview2_probe.exe")) {
    Write-Host 'FAIL  worker-response: probe binary is missing'; $script:failed++; return
  }
  $output = RunTimed "$Dir/webview2_probe.exe" @('--worker-response') 140
  $exit = $LASTEXITCODE
  ($output -split "`n" | Where-Object { $_ -match '^(PASS|FAIL|INFO) ' }) | Out-Host
  $unavailable = [regex]::Matches($output, '(?m)^INFO  worker-response: unavailable: (SERVICE_WORKER filter|external-scheme guard) interface absent; cases not run; no production worker capability enabled\r?$').Count
  $recorded = [regex]::Matches($output, '(?m)^PASS  worker-response: controls and independent network counters verified; error outcomes recorded; no production worker capability enabled\r?$').Count
  if ($exit -eq 2 -and $unavailable -eq 1 -and $recorded -eq 0 -and $output -notmatch '(?m)^FAIL ') {
    Write-Host 'INFO  worker-response: required diagnostic interface unavailable; worker qualification remains incomplete'
    return
  }
  $patterns = @(foreach ($name in 'pass', 'fulfill', 'http403', 'first-read', 'first-abort', 'mid-body', 'uri-deferred', 'invoke-abort', 'followup') {
    "(?m)^INFO  worker-response: request case=$name source=SERVICE_WORKER\r?`$"
    $network = if ($name -in 'pass', 'followup') { '1' } elseif ($name -in 'uri-deferred', 'invoke-abort') { '[01]' } else { '0' }
    "(?m)^INFO  worker-response: case=$name intercepted=1 origin-network=$network\r?`$"
  })
  $patterns += '(?m)^INFO  worker-response: unknown-origin-requests=0\r?$'
  $patterns += '(?m)^INFO  worker-response: deferred-uri-completions=1 invoke-abort-returns=1 external-launches=0 followup-same-worker=yes\r?$'
  $patterns += '(?m)^INFO  worker-response: uri-deferred original=http://127\.0\.0\.1:[0-9]+/network/uri-deferred rewritten=gesher-denial-[0-9a-f]{32}:blocked response=unset after-invoke=yes\r?$'
  $patterns += '(?m)^INFO  worker-response: invoke-abort synchronous return; response unset; no deferral hr=0x80004004\r?$'
  foreach ($name in 'first-read', 'first-abort', 'mid-body') {
    $hr = if ($name -eq 'first-abort') { '80004004' } else { '8003001e' }
    $prefix = if ($name -eq 'mid-body') { '[1-9][0-9]*' } else { '0' }
    $patterns += "(?m)^INFO  worker-response: case=$name read-hr=0x$hr reads=[1-9][0-9]* faults=[1-9][0-9]* supplied-prefix=$prefix live-streams=[0-9]+ pre-response-rejection=(yes|no)\r?`$"
  }
  $inconsistent = 0
  foreach ($name in 'uri-deferred', 'invoke-abort') {
    $pattern = "(?m)^INFO  worker-response: denial case=$name initial-typeerror=(yes|no) request-identity=(yes|no) timing-identity=(yes|no) origin-network=([01]) outcome=(network-rejection-observed|not-qualified)\r?`$"
    $patterns += $pattern
    $rows = [regex]::Matches($output, $pattern)
    if ($rows.Count -eq 1) {
      $row = $rows[0]
      $observed = $row.Groups[1].Value -eq 'yes' -and $row.Groups[2].Value -eq 'yes' -and $row.Groups[3].Value -eq 'yes' -and $row.Groups[4].Value -eq '0'
      $expected = if ($observed) { 'network-rejection-observed' } else { 'not-qualified' }
      if ($row.Groups[5].Value -ne $expected) { $inconsistent++ }
      $counter = [regex]::Matches($output, "(?m)^INFO  worker-response: case=$name intercepted=1 origin-network=([01])\r?`$")
      if ($counter.Count -ne 1 -or $counter[0].Groups[1].Value -ne $row.Groups[4].Value) { $inconsistent++ }
    }
  }
  $missing = @($patterns | Where-Object { [regex]::Matches($output, $_).Count -ne 1 })
  if ($exit -ne 0 -or $recorded -ne 1 -or $unavailable -ne 0 -or $missing.Count -ne 0 -or $inconsistent -ne 0 -or $output -match '(?m)^FAIL ') {
    Write-Host "FAIL  worker-response: diagnostic failed (exit=$exit recorded=$recorded unavailable=$unavailable missing=$($missing.Count) inconsistent=$inconsistent)"
    Write-Host $output
    $script:failed++
  } else {
    Write-Host 'INFO  worker-response: real error outcomes recorded; production worker support remains gated'
  }
}

WorkerResponseCheck
