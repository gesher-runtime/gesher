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
  if ($exit -eq 2 -and $unavailable -eq 1 -and $recorded -eq 0 -and $output -notmatch '(?m)^FAIL ' -and
      $output -notmatch '(?m)^(PASS|INFO)  worker-denial:') {
    Write-Host 'INFO  worker-response: required diagnostic interface unavailable; worker qualification remains incomplete'
    return
  }
  $patterns = @(foreach ($name in 'pass', 'fulfill', 'http403', 'first-read', 'first-abort', 'mid-body', 'uri-deferred', 'invoke-abort', 'followup') {
    "(?m)^INFO  worker-response: request case=$name source=SERVICE_WORKER\r?`$"
    $network = if ($name -in 'pass', 'followup') { '1' } elseif ($name -in 'uri-deferred', 'invoke-abort') { '[01]' } else { '0' }
    "(?m)^INFO  worker-response: case=$name intercepted=1 origin-network=$network\r?`$"
  })
  $patterns += '(?m)^INFO  worker-response: unknown-origin-requests=0\r?$'
  $patterns += '(?m)^PASS  worker-response: independent HTTP byte accounting controls verified\r?$'
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
  $scopeRecorded = [regex]::Matches($output, '(?m)^PASS  worker-denial: scope controls and independent request bodies verified; outcomes recorded; no production worker capability enabled\r?$').Count
  $patterns += '(?m)^INFO  worker-denial: unknown-origin-requests=0 origin-a-connections=[1-9][0-9]* origin-b-connections=[1-9][0-9]*\r?$'
  $patterns += '(?m)^INFO  worker-denial: received-http-bytes-a=[1-9][0-9]* received-http-bytes-b=[1-9][0-9]* unattributed-http-bytes=0\r?$'
  $patterns += '(?m)^INFO  worker-denial: parallel-reverse-completions=2 terminal-before-close=yes late-allow-rejections=1 original-restores=0 external-launches=0\r?$'
  $patterns += '(?m)^INFO  worker-denial: late-allow rejected case=teardown-deny terminal=denied original-restores=0\r?$'
  $patterns += '(?m)^INFO  worker-denial: clear upload before owner closure hr=0x00000000\r?$'
  $patterns += '(?m)^INFO  worker-denial: Complete after owner closure hr=0x(00000000|8007139f)\r?$'
  $redirectPattern = '(?m)^INFO  worker-denial: redirect-start-network=1 redirect-target-callback=(yes|no); observed WebView2 routing only; Android redirect parity unqualified\r?$'
  $patterns += $redirectPattern
  $redirectRows = [regex]::Matches($output, $redirectPattern)
  $redirectSeen = $redirectRows.Count -eq 1 -and $redirectRows[0].Groups[1].Value -eq 'yes'
  foreach ($name in 'post-pass', 'post-deny', 'cors-pass', 'cors-deny', 'redirect-pass', 'redirect-deny', 'parallel-allow', 'parallel-deny', 'sequential-pass', 'teardown-deny', 'teardown-followup') {
    $count = if ($name -eq 'redirect-pass') { '[12]' } elseif ($name -eq 'redirect-deny') { if ($redirectSeen) { '2' } else { '1' } } elseif ($name -eq 'teardown-followup') { '0' } else { '1' }
    $patterns += "(?m)^INFO  worker-denial: case=$name intercepted=$count\r?`$"
  }
  foreach ($name in 'parallel-allow', 'parallel-deny') {
    $patterns += "(?m)^INFO  worker-denial: parallel reverse-complete case=$name\r?`$"
  }
  foreach ($name in 'post-deny', 'cors-deny', 'parallel-deny', 'teardown-deny') {
    $route = if ($name -eq 'parallel-deny') { 'concurrent' } else { $name }
    $patterns += "(?m)^INFO  worker-denial: terminal-denial case=$name original=http://127\.0\.0\.1:[0-9]+/denial/network/$route rewritten=gesher-denial-[0-9a-f]{32}:blocked\r?`$"
  }
  if ($redirectSeen) {
    $patterns += '(?m)^INFO  worker-denial: terminal-denial case=redirect-deny original=http://127\.0\.0\.1:[0-9]+/denial/network/redirect-deny-target rewritten=gesher-denial-[0-9a-f]{32}:blocked\r?$'
  }
  # These are independently accepted HTTP requests, including incomplete
  # uploads. A preflight alone disqualifies cross-origin network blocking.
  $originPattern = '(?m)^INFO  worker-denial: origin=([AB]) method=([A-Z]+) path=([^ ]+) body-bytes=([0-9]+) complete=(yes|no)\r?$'
  $originRows = [regex]::Matches($output, $originPattern)
  if ($originRows.Count -ne [regex]::Matches($output, '(?m)^INFO  worker-denial: origin=').Count) { $inconsistent++ }
  foreach ($control in @(@('A', 'POST', 'post-pass', 32768), @('B', 'POST', 'cors-pass', 32768), @('B', 'OPTIONS', 'cors-pass', 0),
                         @('A', 'GET', 'redirect-pass', 0), @('B', 'GET', 'redirect-pass-target', 0), @('A', 'GET', 'redirect-deny', 0), @('A', 'GET', 'teardown-followup', 0))) {
    $patterns += "(?m)^INFO  worker-denial: origin=$($control[0]) method=$($control[1]) path=/denial/network/$($control[2]) body-bytes=$($control[3]) complete=yes\r?`$"
  }
  $concurrentControls = [regex]::Matches($output, '(?m)^INFO  worker-denial: origin=A method=POST path=/denial/network/concurrent body-bytes=32768 complete=yes\r?$').Count
  if ($concurrentControls -lt 2) { $inconsistent++ }
  foreach ($name in 'post-deny', 'cors-deny', 'redirect-deny', 'parallel-deny', 'teardown-deny') {
    $pattern = "(?m)^INFO  worker-denial: result case=$name initial-typeerror=(yes|no) request-identity=(yes|no) timing-identity=(yes|no) target-network=([0-9]+) preflight-network=([0-9]+) outcome=(network-rejection-observed|not-qualified)\r?`$"
    $patterns += $pattern
    $rows = [regex]::Matches($output, $pattern)
    if ($rows.Count -ne 1) { continue }
    $row = $rows[0]
    $side = if ($name -in 'cors-deny', 'redirect-deny') { 'B' } else { 'A' }
    $route = if ($name -eq 'redirect-deny') { 'redirect-deny-target' } elseif ($name -eq 'parallel-deny') { 'concurrent' } else { $name }
    $requests = @($originRows | Where-Object { $_.Groups[1].Value -eq $side -and $_.Groups[3].Value -eq "/denial/network/$route" })
    $preflight = if ($name -eq 'cors-deny') { @($requests | Where-Object { $_.Groups[2].Value -eq 'OPTIONS' }).Count } else { 0 }
    $network = $requests.Count - $preflight - $(if ($name -eq 'parallel-deny') { 2 } else { 0 })
    if ([int]$row.Groups[4].Value -ne $network -or [int]$row.Groups[5].Value -ne $preflight) { $inconsistent++ }
    $observed = $row.Groups[1].Value -eq 'yes' -and $row.Groups[2].Value -eq 'yes' -and $row.Groups[3].Value -eq 'yes' -and
                $network -eq 0 -and $preflight -eq 0 -and ($name -ne 'redirect-deny' -or $redirectSeen)
    $expected = if ($observed) { 'network-rejection-observed' } else { 'not-qualified' }
    if ($row.Groups[6].Value -ne $expected) { $inconsistent++ }
  }
  $missing = @($patterns | Where-Object { [regex]::Matches($output, $_).Count -ne 1 })
  if ($exit -ne 0 -or $recorded -ne 1 -or $scopeRecorded -ne 1 -or $unavailable -ne 0 -or $missing.Count -ne 0 -or $inconsistent -ne 0 -or $output -match '(?m)^FAIL ') {
    Write-Host "FAIL  worker-response: diagnostic failed (exit=$exit recorded=$recorded scope=$scopeRecorded unavailable=$unavailable missing=$($missing.Count) inconsistent=$inconsistent)"
    Write-Host $output
    $script:failed++
  } else {
    Write-Host 'INFO  worker-response: real error outcomes recorded; production worker support remains gated'
  }
}

WorkerResponseCheck
