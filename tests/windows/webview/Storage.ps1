# The public WebStorage API on ART and the real WebView2 quota manager/IndexedDB.
# The test app supplies only its HTML through WebViewClient.shouldInterceptRequest;
# distinct fresh HTTPS origins remain real browser origins. No network response is claimed.
# Dot-sourced by track37 with RunTimed, NoDrive, $Dir, $dvmWebView and $failed.
function WebStorageCheck {
  $name = 'public WebStorage usage/quota for two fresh HTTPS origins and IndexedDB deletion isolation'
  if (-not (Test-Path "$Dir/webapp.apk")) {
    Write-Host "FAIL  gesher-app: $name (webapp.apk is missing)"; $script:failed++; return
  }
  $profile = Join-Path $env:RUNNER_TEMP "webstorage-profile-$([Guid]::NewGuid().ToString('N'))"
  New-Item -ItemType Directory -Path $profile | Out-Null
  $saved = @{}
  foreach ($key in @('ANDROID_DATA', 'GESHER_LOGS', 'GESHER_UID', 'GESHER_BINDER_UID', 'GESHER_BINDER', 'GESHER_STANDALONE_RESTARTED')) {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key)
  }
  $output = ''
  $problems = @()
  try {
    foreach ($key in @('GESHER_UID', 'GESHER_BINDER_UID', 'GESHER_BINDER', 'GESHER_STANDALONE_RESTARTED')) {
      [Environment]::SetEnvironmentVariable($key, $null)
    }
    $env:ANDROID_DATA = NoDrive $profile
    $env:GESHER_LOGS = NoDrive (Join-Path $profile 'logs')
    $output = RunTimed "$Dir/dalvikvm.exe" (@($dvmWebView) + @('com.gesher.app.Main', '--window',
      '--activity', 'com.gesher.webapp.WebStorageActivity', (NoDrive "$Dir/webapp.apk"))) 240
    if ($LASTEXITCODE -ne 0) { $problems += "process exit $LASTEXITCODE" }
    if ($output.Contains('webstorage: FAIL ')) { $problems += 'activity reported a failure' }
    $patterns = @(
      'webstorage: PASS written A bytes=262144 connection=closed origin=https://gesher-storage-a-[0-9a-f]{32}\.invalid',
      'webstorage: PASS written B bytes=262144 connection=closed origin=https://gesher-storage-b-[0-9a-f]{32}\.invalid',
      'webstorage: PASS baseline A usage=[1-9][0-9]* quota=[1-9][0-9]*',
      'webstorage: PASS baseline B usage=[1-9][0-9]* quota=[1-9][0-9]*',
      'webstorage: PASS deleted A usage=0 quota=[1-9][0-9]*',
      'webstorage: PASS absent A bytes=0 connection=closed origin=https://gesher-storage-a-[0-9a-f]{32}\.invalid',
      'webstorage: PASS present B bytes=262144 connection=closed origin=https://gesher-storage-b-[0-9a-f]{32}\.invalid',
      'webstorage: PASS preserved B usage=[1-9][0-9]* quota=[1-9][0-9]*',
      'webstorage: ALL PASSED origins=2 public_queries=([1-9][0-9]*) callbacks=\1 callbacks_on_main=true')
    foreach ($pattern in $patterns) {
      if ([regex]::Matches($output, $pattern).Count -ne 1) { $problems += "missing or duplicate /$pattern/" }
    }
  } catch {
    $problems += "harness failure: $_"
  } finally {
    foreach ($entry in $saved.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value) }
  }
  if ($problems.Count -eq 0) {
    Write-Host "PASS  gesher-app: $name"
    ($output -split "`n" | Where-Object { $_ -match 'webstorage: ' }) | Out-Host
  } else {
    Write-Host "FAIL  gesher-app: $name ($($problems -join '; '))"
    Write-Host $output
    $script:failed++
  }
}

WebStorageCheck
