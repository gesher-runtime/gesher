# AppContainer isolation, milestone M1 (docs/decisions/0090-appcontainer-app-isolation.md,
# docs/sandbox.md). Dot-sourced by run-tests.ps1, whose Check, $Dir, $root and
# $failed these use. A binderd started with GESHER_SANDBOX=appcontainer and the
# production launch path (SpawnInAppContainer) with real AppContainer tokens:
# the kernel closes the user's files to a contained process, binderd's pipe admits
# it only with the binder-client capability, and binderd refuses its claims of a
# system UID or of another process's pid. The same probe checks shared-storage
# inheritance, live downgrade/revoke, protected private roots and replacement
# policy-file readability through real Windows access checks.
$sandboxSaved = $env:GESHER_SANDBOX
$sandboxDevice = "sandbox-$PID"
$sandboxBinderd = $null
try {
  $env:GESHER_SANDBOX = 'appcontainer'
  $sandboxBinderd = Start-Process "$Dir/binderd.exe" -ArgumentList $sandboxDevice -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $root 'sandbox-binderd.out') `
    -RedirectStandardError (Join-Path $root 'sandbox-binderd.log')
  $pipe = "gesher-binder-$sandboxDevice"
  for ($i = 0; $i -lt 600 -and -not (Get-ChildItem '\\.\pipe\' | Where-Object Name -eq $pipe); $i++) {
    if ($sandboxBinderd.HasExited) { break }
    Start-Sleep -Milliseconds 100
  }
  Check 'sandbox: AppContainer isolation, Binder identity and live storage ACL lifecycle' {
    & "$Dir/appcontainer_probe.exe" $sandboxDevice
  } @('all passed')
  Check 'sandbox: real AppContainer reads live broker properties with write access denied' {
    & "$Dir/appcontainer_property_probe.exe" (Join-Path $root 'sandbox-property')
  } @('property-area probe all passed')
  Check 'sandbox: real AppContainer preserves Binder admission, record locks and directory descriptors' {
    & "$Dir/appcontainer_record_lock_probe.exe" $Dir (Join-Path $root 'sandbox-record-locks')
  } @('protected server drives fixture: ok')
  Check 'sandbox: actual SQLite WAL and metadata operations use the exact rooted AppContainer scope' {
    & "$Dir/rooted_storage_host_probe.exe" (Join-Path $root 'sandbox-rooted-sqlite')
  } @('production rooted host serves only exact scope and selected area: ok')
  Check 'sandbox: installed AppContainer mediates scoped and shared storage operations' {
    & "$Dir/appcontainer_installed_media_probe.exe" $Dir (Join-Path $root 'sandbox-installed-media')
  } @('installed media probe all passed')
} finally {
  if ($null -eq $sandboxSaved) { Remove-Item Env:GESHER_SANDBOX -ErrorAction SilentlyContinue }
  else { $env:GESHER_SANDBOX = $sandboxSaved }
  if ($null -ne $sandboxBinderd -and -not $sandboxBinderd.HasExited) {
    Stop-Process -Id $sandboxBinderd.Id -Force -ErrorAction SilentlyContinue
  }
  Get-Content (Join-Path $root 'sandbox-binderd.log') -ErrorAction SilentlyContinue | Write-Host
}
