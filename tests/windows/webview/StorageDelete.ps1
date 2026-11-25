# Exercise the actual production Host, using a fresh owned ordinary profile.
# There is no accepted UNAVAILABLE outcome for a production deletion gate.
function StorageDeleteCheck {
  if (-not (Test-Path "$Dir/webview2_probe.exe")) {
    Write-Host 'FAIL  storage-delete: probe binary is missing'; $script:failed++; return
  }
  $output = RunTimed "$Dir/webview2_probe.exe" @('--storage-delete') 690
  $exit = $LASTEXITCODE
  ($output -split "`n" | Where-Object { $_ -match '^(PASS|FAIL|INFO) ' }) | Out-Host
  $qualified = [regex]::Matches($output, '(?m)^PASS  storage-delete: QUALIFIED production=Host::ClearOriginData persisted_baseline=5 full_keys=14 deleted=9 preserved=5 localStorage=14 cookies=6 fileSystems=2 callbacks=5 browser_exit=true\r?$').Count
  $patterns = @(
    'PASS  storage-delete: network before positive-control requests=1 accepted-connections=[1-9][0-9]*',
    'PASS  storage-delete: persisted-baseline positive=5 removed=0',
    'PASS  storage-delete: five persisted keys discovered after browser exit before origin revisit',
    'PASS  storage-delete: expanded-origins positive=13 removed=0',
    'PASS  storage-delete: all-written positive=14 removed=0',
    'PASS  storage-delete: ordinary-network control started on unrelated origin with real no-store fetch',
    'PASS  storage-delete: production callbacks=2 ordered=true exactly-once=true owner-STA=true',
    'PASS  storage-delete: after-A positive=9 removed=5',
    'PASS  storage-delete: production discovered and deleted persisted partitions before ordinary origin revisit',
    'PASS  storage-delete: production callbacks=3 ordered=true exactly-once=true owner-STA=true',
    'PASS  storage-delete: sentinel-preserved positive=9 removed=5',
    'PASS  storage-delete: production callbacks=4 ordered=true exactly-once=true owner-STA=true',
    'PASS  storage-delete: after-HTTPS positive=7 removed=7',
    'PASS  storage-delete: shutdown drained accepted deletion pending-observed=true joined=true later-admission=rejected',
    'PASS  storage-delete: after-HTTP positive=5 removed=9',
    'PASS  storage-delete: resolver-origin-network-connections=0 independent-positive-control=true',
    'PASS  storage-delete: network after positive-control requests=2 accepted-connections=[1-9][0-9]*',
    'PASS  storage-delete: filesystem write A-first-party bytes=8192 writable-closed=true',
    'PASS  storage-delete: filesystem write B-first-party bytes=8192 writable-closed=true',
    'PASS  storage-delete: filesystem absent A-first-party bytes=0 writable-closed=true',
    'PASS  storage-delete: filesystem read B-first-party bytes=8192 writable-closed=true',
    'PASS  storage-delete: ordinary-network stopped failures=0 successful-fetches=[1-9][0-9]* origin-requests=[1-9][0-9]*')
  foreach ($phase in 'A', 'sentinel', 'HTTPS', 'HTTP') {
    $patterns += "PASS  storage-delete: ordinary-network phase=$phase failures=0 completed-during=[1-9][0-9]* independently-counted=true"
  }
  foreach ($label in 'A-first-party', 'HTTP-first-party', 'HTTPS-first-party') {
    $patterns += "PASS  storage-delete: absent $label bytes=0 localStorage=preserved cookies=preserved closed=true"
  }
  foreach ($label in 'C-under-A', 'D-under-subdomain', 'E-under-port', 'C-under-HTTP', 'D-under-HTTPS', 'A-through-B-under-A') {
    $patterns += "PASS  storage-delete: absent $label bytes=0 localStorage=preserved cookies=not-tested closed=true"
  }
  foreach ($label in 'B-first-party', 'subdomain-first-party', 'port-first-party') {
    $patterns += "PASS  storage-delete: read $label bytes=262144 localStorage=preserved cookies=preserved closed=true"
  }
  foreach ($label in 'C-under-B', 'A-under-B', 'C-under-HTTP') {
    $patterns += "PASS  storage-delete: read $label bytes=262144 localStorage=preserved cookies=not-tested closed=true"
  }
  $patterns += 'PASS  storage-delete: read HTTP-first-party bytes=262144 localStorage=preserved cookies=preserved closed=true'
  $missing = @($patterns | Where-Object { [regex]::Matches($output, "(?m)^$_\r?`$").Count -ne 1 })
  $finalCallbacks = [regex]::Matches($output, '(?m)^PASS  storage-delete: production callbacks=5 ordered=true exactly-once=true owner-STA=true\r?$').Count
  if ($exit -ne 0 -or $qualified -ne 1 -or $missing.Count -ne 0 -or $finalCallbacks -ne 2 -or
      $output -match '(?m)^FAIL ' -or $output -match 'UNAVAILABLE') {
    Write-Host "FAIL  storage-delete: production qualification failed (exit=$exit qualified=$qualified missing=$($missing.Count) final-callback-checks=$finalCallbacks)"
    Write-Host $output
    $script:failed++
  }
}

StorageDeleteCheck
