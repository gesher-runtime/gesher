# AppContainer isolation, milestone M1 (docs/decisions/0090-appcontainer-app-isolation.md,
# docs/sandbox.md). Dot-sourced by run-tests.ps1, whose Check, $Dir, $root and
# $failed these use. A binderd started with GESHER_SANDBOX=appcontainer and the
# production launch path (SpawnInAppContainer) with real AppContainer tokens:
# the kernel closes the user's files to a contained process, binderd's pipe admits
# it only with the binder-client capability, and binderd refuses its claims of a
# system UID or of another process's pid.
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
  Check 'sandbox: AppContainer launch path, the user''s files closed, binderd admission and refused identity claims' {
    & "$Dir/appcontainer_probe.exe" $sandboxDevice
  } @('all passed')
  Check 'sandbox: real AppContainer reads live broker properties with write access denied' {
    & "$Dir/appcontainer_property_probe.exe" (Join-Path $root 'sandbox-property')
  } @('property-area probe all passed')
} finally {
  if ($null -eq $sandboxSaved) { Remove-Item Env:GESHER_SANDBOX -ErrorAction SilentlyContinue }
  else { $env:GESHER_SANDBOX = $sandboxSaved }
  if ($null -ne $sandboxBinderd -and -not $sandboxBinderd.HasExited) {
    Stop-Process -Id $sandboxBinderd.Id -Force -ErrorAction SilentlyContinue
  }
  Get-Content (Join-Path $root 'sandbox-binderd.log') -ErrorAction SilentlyContinue | Write-Host
}
