# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Isolated log fixtures test the exact PowerShell gate, never native behavior.
$ErrorActionPreference = 'Stop'
$gate = Join-Path $PSScriptRoot '../windows/webview/WorkerResponse.ps1'
$Dir = Join-Path ([IO.Path]::GetTempPath()) ('worker-response-gate-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $Dir > $null
New-Item -ItemType File -Path (Join-Path $Dir 'webview2_probe.exe') > $null
# Out-Host writes directly to the host, bypassing stream redirection. Keep
# invented fixture rows out of CI logs so they cannot be mistaken for evidence.
function Out-Host {
  [CmdletBinding()] param([Parameter(ValueFromPipeline)] $InputObject)
  process {}
}
function RunTimed {
  param($Executable, $Arguments, $Timeout)
  if ($Arguments[0] -ne '--worker-response' -or $Timeout -ne 140) { throw 'gate invoked a different diagnostic' }
  $global:LASTEXITCODE = $script:probeExit
  return $script:probeOutput
}
function BaseFixture([bool]$Rejection) {
  $rows = [Collections.Generic.List[string]]::new()
  foreach ($name in 'pass', 'fulfill', 'http403', 'first-read', 'first-abort', 'mid-body', 'uri-deferred', 'invoke-abort', 'followup') {
    $rows.Add("INFO  worker-response: request case=$name source=SERVICE_WORKER")
    $network = if ($name -in 'pass', 'followup') { 1 } elseif ($name -in 'uri-deferred', 'invoke-abort' -and -not $Rejection) { 1 } else { 0 }
    $rows.Add("INFO  worker-response: case=$name intercepted=1 origin-network=$network")
  }
  foreach ($name in 'first-read', 'first-abort', 'mid-body') {
    $hr = if ($name -eq 'first-abort') { '80004004' } else { '8003001e' }
    $prefix = if ($name -eq 'mid-body') { 65536 } else { 0 }
    $rows.Add("INFO  worker-response: case=$name read-hr=0x$hr reads=1 faults=1 supplied-prefix=$prefix live-streams=0 pre-response-rejection=no")
  }
  foreach ($name in 'uri-deferred', 'invoke-abort') {
    $initial = if ($Rejection) { 'yes' } else { 'no' }
    $network = if ($Rejection) { 0 } else { 1 }
    $outcome = if ($Rejection) { 'network-rejection-observed' } else { 'not-qualified' }
    $rows.Add("INFO  worker-response: denial case=$name initial-typeerror=$initial request-identity=yes timing-identity=yes origin-network=$network outcome=$outcome")
  }
  $rows.Add('INFO  worker-response: unknown-origin-requests=0')
  $rows.Add('PASS  worker-response: independent HTTP byte accounting controls verified')
  $rows.Add('INFO  worker-response: deferred-uri-completions=1 invoke-abort-returns=1 external-launches=0 followup-same-worker=yes')
  $rows.Add('INFO  worker-response: uri-deferred original=http://127.0.0.1:32123/network/uri-deferred rewritten=gesher-denial-00000000000000000000000000000000:blocked response=unset after-invoke=yes')
  $rows.Add('INFO  worker-response: invoke-abort synchronous return; response unset; no deferral hr=0x80004004')
  $rows.Add('PASS  worker-response: controls and independent network counters verified; error outcomes recorded; no production worker capability enabled')
  return ($rows -join "`n") + "`n"
}
function ScopeFixture {
  $rows = [Collections.Generic.List[string]]::new()
  $rows.Add('INFO  worker-denial: fresh-profile=C:\fixture\scope origin=http://127.0.0.1:32123 other=http://127.0.0.1:32124')
  foreach ($name in 'post-pass', 'post-deny', 'cors-pass', 'cors-deny', 'redirect-pass', 'redirect-deny', 'parallel-allow', 'parallel-deny', 'sequential-pass', 'cors-preflight-deny', 'teardown-deny', 'teardown-followup') {
    $count = if ($name -in 'redirect-pass', 'redirect-deny') { 2 } elseif ($name -in 'teardown-followup', 'cors-preflight-deny') { 0 } else { 1 }
    $rows.Add("INFO  worker-denial: case=$name intercepted=$count")
  }
  foreach ($name in 'parallel-allow', 'parallel-deny') { $rows.Add("INFO  worker-denial: parallel reverse-complete case=$name") }
  foreach ($name in 'post-deny', 'cors-deny', 'redirect-deny', 'parallel-deny', 'cors-preflight-deny', 'teardown-deny') {
    $route = if ($name -eq 'redirect-deny') { 'redirect-deny-target' } elseif ($name -eq 'parallel-deny') { 'concurrent' } else { $name }
    $port = if ($name -in 'cors-deny', 'cors-preflight-deny', 'redirect-deny') { 32124 } else { 32123 }
    $rows.Add("INFO  worker-denial: terminal-denial case=$name original=http://127.0.0.1:$port/denial/network/$route rewritten=gesher-denial-00000000000000000000000000000000:blocked")
    $rows.Add("INFO  worker-denial: result case=$name initial-typeerror=yes request-identity=yes timing-identity=yes target-network=0 preflight-network=0 outcome=network-rejection-observed")
  }
  foreach ($control in @(@('A', 'POST', 'post-pass', 32768), @('B', 'POST', 'cors-pass', 32768), @('B', 'OPTIONS', 'cors-pass', 0),
                         @('A', 'GET', 'redirect-pass', 0), @('B', 'GET', 'redirect-pass-target', 0), @('A', 'GET', 'redirect-deny', 0),
                         @('A', 'GET', 'teardown-followup', 0), @('A', 'POST', 'concurrent', 32768), @('A', 'POST', 'concurrent', 32768))) {
    $rows.Add("INFO  worker-denial: origin=$($control[0]) method=$($control[1]) path=/denial/network/$($control[2]) body-bytes=$($control[3]) complete=yes")
  }
  $rows.Add('INFO  worker-denial: unknown-origin-requests=0 origin-a-connections=7 origin-b-connections=4')
  $rows.Add('INFO  worker-denial: received-http-bytes-a=101000 received-http-bytes-b=34000 unattributed-http-bytes=0')
  $rows.Add('INFO  worker-denial: parallel-reverse-completions=2 terminal-before-close=yes late-allow-rejections=1 original-restores=0 external-launches=0')
  $rows.Add('INFO  worker-denial: late-allow rejected case=teardown-deny terminal=denied original-restores=0')
  $rows.Add('INFO  worker-denial: clear upload before owner closure hr=0x00000000')
  $rows.Add('INFO  worker-denial: Complete after owner closure hr=0x8007139f')
  $rows.Add('INFO  worker-denial: preflight-policy case=cors-preflight-deny options-intercepted=1 post-intercepted=0')
  $rows.Add('INFO  worker-denial: preflight case=cors-preflight-deny requested-method=POST requesting-origin=http://127.0.0.1:32123 source=SERVICE_WORKER')
  $rows.Add('INFO  worker-denial: request uri=http://127.0.0.1:32124/denial/network/cors-preflight-deny method=OPTIONS source=SERVICE_WORKER')
  $rows.Add('INFO  worker-denial: redirect-start-network=1 redirect-target-callback=yes; observed WebView2 routing only; Android redirect parity unqualified')
  $rows.Add('PASS  worker-denial: scope controls and independent request bodies verified; outcomes recorded; no production worker capability enabled')
  return ($rows -join "`n") + "`n"
}
function OrderingFixture {
  $rows = [Collections.Generic.List[string]]::new()
  $rows.Add('INFO  worker-ordering: fresh-profile=C:\fixture\ordering origin=http://127.0.0.1:32125 other=http://127.0.0.1:32126')
  $rows.Add('INFO  worker-ordering: terminal put_Uri hr=0x00000000')
  $rows.Add('INFO  worker-ordering: terminal-denial case=teardown-complete-deny original=http://127.0.0.1:32125/denial/network/teardown-complete-deny rewritten=gesher-denial-11111111111111111111111111111111:blocked')
  $rows.Add('INFO  worker-ordering: clear upload before owner closure hr=0x00000000')
  $rows.Add('INFO  worker-ordering: Complete before owner closure hr=0x00000000')
  $rows.Add('INFO  worker-ordering: completion-order case=teardown-complete-deny complete-before-filters-and-close=yes owner-live=yes filters=2')
  $rows.Add('INFO  worker-ordering: remove source filter hr=0x00000000')
  $rows.Add('INFO  worker-ordering: remove source filter hr=0x00000000')
  $rows.Add('INFO  worker-ordering: controller.Close hr=0x00000000')
  $rows.Add('INFO  worker-ordering: late-allow rejected case=teardown-complete-deny terminal=denied original-restores=0')
  $rows.Add('INFO  worker-ordering: controller.Close hr=0x00000000')
  $rows.Add('INFO  worker-ordering: case=post-pass intercepted=1')
  $rows.Add('INFO  worker-ordering: case=teardown-complete-deny intercepted=1')
  $rows.Add('INFO  worker-ordering: case=teardown-complete-followup intercepted=0')
  $rows.Add('INFO  worker-ordering: origin=A method=POST path=/denial/network/post-pass body-bytes=32768 complete=yes')
  $rows.Add('INFO  worker-ordering: origin=A method=GET path=/denial/network/teardown-complete-followup body-bytes=0 complete=yes')
  $rows.Add('INFO  worker-ordering: unknown-origin-requests=0 origin-a-connections=5 origin-b-connections=0')
  $rows.Add('INFO  worker-ordering: received-http-bytes-a=34000 received-http-bytes-b=0 unattributed-http-bytes=0')
  $rows.Add('INFO  worker-ordering: parallel-reverse-completions=0 terminal-before-close=yes late-allow-rejections=1 original-restores=0 external-launches=0')
  $rows.Add('INFO  worker-ordering: result case=teardown-complete-deny initial-typeerror=yes request-identity=yes timing-identity=yes target-network=0 preflight-network=0 outcome=network-rejection-observed')
  $rows.Add('PASS  worker-ordering: scope controls and independent request bodies verified; outcomes recorded; no production worker capability enabled')
  return ($rows -join "`n") + "`n"
}
$script:checks = 0
function Check([string]$Name, [string]$Output, [int]$ExitCode, [int]$Failures) {
  $script:failed = 0; $script:probeOutput = $Output; $script:probeExit = $ExitCode
  & { . $gate } *> $null
  if ($script:failed -ne $Failures) { throw "$Name failures=$script:failed expected=$Failures" }
  $script:checks++
}
try {
  $scope = ScopeFixture
  $ordering = OrderingFixture
  $positive = (BaseFixture $true) + $scope + $ordering
  $negative = (BaseFixture $false) + $scope + $ordering
  Check 'observed rejection' $positive 0 0
  Check 'valid primitive negative' $negative 0 0
  Check 'base timing identity lost' ($positive -replace 'timing-identity=yes origin-network=0 outcome=network-rejection-observed', 'timing-identity=no origin-network=0 outcome=not-qualified') 0 0
  Check 'forged primitive rejection' ($negative -replace 'outcome=not-qualified', 'outcome=network-rejection-observed') 0 1
  Check 'contradictory primitive counter' ($positive -replace 'case=uri-deferred intercepted=1 origin-network=0', 'case=uri-deferred intercepted=1 origin-network=1') 0 1
  Check 'external launch attempt' ($positive -replace 'external-launches=0', 'external-launches=1') 0 1
  Check 'missing original followup' ($positive -replace '(?m)^INFO  worker-response: request case=followup source=SERVICE_WORKER\n', '') 0 1
  Check 'duplicate primitive observation' ($positive + "INFO  worker-response: denial case=uri-deferred initial-typeerror=yes request-identity=yes timing-identity=yes origin-network=0 outcome=network-rejection-observed`n") 0 1
  $unavailable = "INFO  worker-response: unavailable: external-scheme guard interface absent; cases not run; no production worker capability enabled`n"
  Check 'guard unavailable' $unavailable 2 0
  Check 'unavailable cannot claim matrix observations' ($unavailable + $scope) 2 1

  $preflight = $positive + "INFO  worker-denial: origin=B method=OPTIONS path=/denial/network/cors-deny body-bytes=0 complete=yes`n"
  Check 'escaped preflight must be counted' $preflight 0 1
  $preflight = $preflight -replace '(result case=cors-deny .*?)preflight-network=0 outcome=network-rejection-observed', '${1}preflight-network=1 outcome=not-qualified'
  Check 'escaped preflight is valid negative' $preflight 0 0
  Check 'escaped preflight cannot qualify blocking' ($preflight -replace 'preflight-network=1 outcome=not-qualified', 'preflight-network=1 outcome=network-rejection-observed') 0 1
  $partial = $positive + "INFO  worker-denial: origin=A method=POST path=/denial/network/post-deny body-bytes=7 complete=no`n"
  Check 'partial denied upload must be counted' $partial 0 1
  $partial = $partial -replace '(result case=post-deny .*?)target-network=0 preflight-network=0 outcome=network-rejection-observed', '${1}target-network=1 preflight-network=0 outcome=not-qualified'
  Check 'partial denied upload is valid negative' $partial 0 0
  $headers = $positive + "INFO  worker-denial: origin=A method=POST path=/denial/network/post-deny body-bytes=0 complete=no`n"
  Check 'partial headers must be counted' $headers 0 1
  $headers = $headers -replace '(result case=post-deny .*?)target-network=0 preflight-network=0 outcome=network-rejection-observed', '${1}target-network=1 preflight-network=0 outcome=not-qualified'
  Check 'attributable partial headers are valid negative' $headers 0 0
  Check 'unclassifiable HTTP prefix invalidates evidence' ($positive -replace 'unattributed-http-bytes=0', 'unattributed-http-bytes=16') 0 1
  Check 'partial control upload fails harness' ($positive -replace 'path=/denial/network/post-pass body-bytes=32768 complete=yes', 'path=/denial/network/post-pass body-bytes=7 complete=no') 0 1
  $redirect = $positive -replace '(?m)^INFO  worker-denial: terminal-denial case=redirect-deny[^\n]+\n', ''
  $redirect = $redirect -replace 'case=redirect-deny intercepted=2', 'case=redirect-deny intercepted=1' -replace 'redirect-start-network=1 redirect-target-callback=yes', 'redirect-start-network=1 redirect-target-callback=no'
  Check 'unobserved redirect target cannot qualify' $redirect 0 1
  $redirect = $redirect -replace '(result case=redirect-deny .*?)target-network=0 preflight-network=0 outcome=network-rejection-observed', '${1}target-network=1 preflight-network=0 outcome=not-qualified'
  $redirect += "INFO  worker-denial: origin=B method=GET path=/denial/network/redirect-deny-target body-bytes=0 complete=yes`n"
  Check 'unobserved redirect target is valid negative' $redirect 0 0
  Check 'late allow restores original' ($positive -replace 'original-restores=0', 'original-restores=1') 0 1
  Check 'missing teardown followup' ($positive -replace '(?m)^INFO  worker-denial: origin=A method=GET path=/denial/network/teardown-followup[^\n]+\n', '') 0 1
  Check 'missed required interception' ($positive -replace 'case=post-deny intercepted=1', 'case=post-deny intercepted=0') 0 1
  Check 'missing matrix completion' ($positive -replace '(?m)^PASS  worker-denial:[^\n]+\n', '') 0 1
  Check 'unexpected origin request' ($positive -replace 'worker-denial: unknown-origin-requests=0', 'worker-denial: unknown-origin-requests=1') 0 1
  $lostTiming = $positive -replace '(result case=post-deny .*?)timing-identity=yes', '${1}timing-identity=no'
  Check 'changed identity cannot qualify' $lostTiming 0 1
  Check 'changed identity is valid negative' ($lostTiming -replace '(result case=post-deny .*?)outcome=network-rejection-observed', '${1}outcome=not-qualified') 0 0

  $preflightDenied = $positive + "INFO  worker-denial: origin=B method=OPTIONS path=/denial/network/cors-preflight-deny body-bytes=0 complete=yes`n"
  Check 'denied OPTIONS escape must be counted' $preflightDenied 0 1
  $preflightDenied = $preflightDenied -replace '(result case=cors-preflight-deny .*?)preflight-network=0 outcome=network-rejection-observed', '${1}preflight-network=1 outcome=not-qualified'
  Check 'denied OPTIONS escape is valid negative' $preflightDenied 0 0
  Check 'preflight requested another method' ($positive -replace 'requested-method=POST', 'requested-method=PUT') 0 1
  $noOptions = $positive -replace 'options-intercepted=1', 'options-intercepted=0'
  $noOptions = $noOptions -replace '(?m)^INFO  worker-denial: (preflight case=cors-preflight-deny|request uri=[^ ]+/cors-preflight-deny method=OPTIONS|terminal-denial case=cors-preflight-deny)[^\n]*\n', ''
  Check 'missing OPTIONS cannot qualify' $noOptions 0 1
  Check 'missing OPTIONS is valid negative' ($noOptions -replace '(result case=cors-preflight-deny .*?)outcome=network-rejection-observed', '${1}outcome=not-qualified') 0 0
  $postAfterOptions = $positive -replace 'case=cors-preflight-deny intercepted=0', 'case=cors-preflight-deny intercepted=1' -replace 'post-intercepted=0', 'post-intercepted=1'
  Check 'POST after OPTIONS denial cannot qualify' $postAfterOptions 0 1
  Check 'POST after OPTIONS denial is valid negative' ($postAfterOptions -replace '(result case=cors-preflight-deny .*?)outcome=network-rejection-observed', '${1}outcome=not-qualified') 0 0

  $oldClose = $positive + "INFO  worker-denial: origin=A method=POST path=/denial/network/teardown-deny body-bytes=32768 complete=yes`n"
  $oldClose = $oldClose -replace 'result case=teardown-deny initial-typeerror=yes', 'result case=teardown-deny initial-typeerror=no'
  $oldClose = $oldClose -replace '(result case=teardown-deny .*?)target-network=0 preflight-network=0 outcome=network-rejection-observed', '${1}target-network=1 preflight-network=0 outcome=not-qualified'
  Check 'original Close-before-Complete negative retained' $oldClose 0 0
  $newClose = $positive + "INFO  worker-ordering: origin=A method=POST path=/denial/network/teardown-complete-deny body-bytes=32768 complete=yes`n"
  Check 'new ordering escape must be counted' $newClose 0 1
  $newClose = $newClose -replace 'result case=teardown-complete-deny initial-typeerror=yes', 'result case=teardown-complete-deny initial-typeerror=no'
  $newClose = $newClose -replace '(result case=teardown-complete-deny .*?)target-network=0 preflight-network=0 outcome=network-rejection-observed', '${1}target-network=1 preflight-network=0 outcome=not-qualified'
  Check 'new ordering full upload escape is valid negative' $newClose 0 0
  Check 'new ordering cannot fabricate success' ($newClose -replace '(result case=teardown-complete-deny .*?)outcome=not-qualified', '${1}outcome=network-rejection-observed') 0 1
  $lateComplete = $positive -replace '(?m)^INFO  worker-ordering: Complete before owner closure[^\n]+\n', ''
  $lateComplete += "INFO  worker-ordering: Complete before owner closure hr=0x00000000`n"
  Check 'native order contradicts completion claim' $lateComplete 0 1
  Check 'duplicate completion forbidden' ($positive + "INFO  worker-ordering: Complete before owner closure hr=0x00000000`n") 0 1
  Check 'ordering must have a distinct profile' ($positive -replace 'fresh-profile=C:\\fixture\\ordering', 'fresh-profile=C:\fixture\scope') 0 1
  Check 'ordering followup is required' ($positive -replace '(?m)^INFO  worker-ordering: origin=A method=GET path=/denial/network/teardown-complete-followup[^\n]+\n', '') 0 1
  Check 'ordering cannot run after unsupported first phase' ($unavailable + $ordering) 2 1
  Write-Host "Worker response PowerShell gate: $script:checks fixture scenarios passed; no native runtime exercised"
} finally {
  Remove-Item -LiteralPath $Dir -Recurse -Force
}
