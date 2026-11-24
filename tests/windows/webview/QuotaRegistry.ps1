# Explicitly isolated capability qualification. The binary uses a fresh owned
# profile, real HTTPS/IndexedDB fixtures and a genuine browser-process restart.
# An unavailable built-in route is reported as unavailable, never as support.
function QuotaRegistryCheck {
  if (-not (Test-Path "$Dir/webview2_probe.exe")) {
    Write-Host 'FAIL  quota-registry: probe binary is missing'; $script:failed++; return
  }
  $output = RunTimed "$Dir/webview2_probe.exe" @('--quota-registry') 180
  $exit = $LASTEXITCODE
  ($output -split "`n" | Where-Object { $_ -match '^(PASS|FAIL|INFO) ' }) | Out-Host
  $unavailable = [regex]::Matches($output, '(?m)^INFO  quota-registry: UNAVAILABLE .+; production support not established\r?$').Count
  $qualified = [regex]::Matches($output, '(?m)^PASS  quota-registry: QUALIFIED persisted=2 live=5 deleted=2 preserved=3 payload_bytes=262144; production API still gated\r?$').Count
  if ($exit -eq 3 -and $unavailable -eq 1 -and $qualified -eq 0 -and $output -notmatch '(?m)^FAIL ') {
    Write-Host 'INFO  quota-registry: live registry route unavailable; full WebStorage remains incomplete'
    return
  }
  $patterns = @(
    'PASS  quota-registry: persisted-before-revisit complete keys=2 removed=0',
    'PASS  quota-registry: persisted discovery after browser exit without revisiting fixture origins',
    'PASS  quota-registry: live-update complete keys=5 removed=0',
    'PASS  quota-registry: post-delete complete keys=3 removed=2',
    'PASS  quota-registry: absent A-first-party origin=https://quota-a-[0-9a-f]{32}\.invalid bytes=0 closed=true secure=true',
    'PASS  quota-registry: absent C-under-A origin=https://quota-c-[0-9a-f]{32}\.invalid bytes=0 closed=true secure=true',
    'PASS  quota-registry: read B-first-party origin=https://quota-b-[0-9a-f]{32}\.invalid bytes=262144 closed=true secure=true',
    'PASS  quota-registry: read C-under-B origin=https://quota-c-[0-9a-f]{32}\.invalid bytes=262144 closed=true secure=true',
    'PASS  quota-registry: read A-under-B origin=https://quota-a-[0-9a-f]{32}\.invalid bytes=262144 closed=true secure=true')
  $missing = @($patterns | Where-Object { [regex]::Matches($output, $_).Count -ne 1 })
  if ($exit -ne 0 -or $qualified -ne 1 -or $unavailable -ne 0 -or $missing.Count -ne 0 -or $output -match '(?m)^FAIL ') {
    Write-Host "FAIL  quota-registry: qualification failed (exit=$exit qualified=$qualified unavailable=$unavailable missing=$($missing.Count))"
    Write-Host $output
    $script:failed++
  }
}

QuotaRegistryCheck
