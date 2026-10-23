# Binder across processes and the framework's system services
# (docs/services.md, docs/decisions/0015-services-binder-ipc.md). Dot-sourced
# by run-tests.ps1, whose Check, NoDrive, $Dir, $root, $bcp and $failed these
# use.

# Runs a program with a time limit (binder calls that go wrong tend to wait
# forever), killing it and whatever it started when the time is up. Returns
# its output; $LASTEXITCODE is its exit code (-1 on a time-out).
function RunTimed([string] $Exe, [string[]] $Arguments = @(), [int] $Seconds = 120) {
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) {
    $p.Kill($true)
    $p.WaitForExit()
  }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $global:LASTEXITCODE = if ($timedOut) { -1 } else { $p.ExitCode }
  $out = $stdout.Result + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  return $out
}

function CheckTimed([string] $Name, [string] $Exe, [string[]] $Arguments, [string[]] $Expect, [int] $Seconds = 120) {
  $out = RunTimed $Exe $Arguments $Seconds
  $code = $LASTEXITCODE
  $missing = $Expect | Where-Object { -not $out.Contains($_) }
  if ($code -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
  } else {
    Write-Host "FAIL  $Name (exit $code; missing: $($missing -join ', '))"
    Write-Host $out
    $script:failed++
  }
}

# The driver's ABI across processes: binderd, a context manager and a client
# with nested calls, objects passed both ways, and a death notification.
CheckTimed 'binder: /dev/binder across processes (binderd, raw ioctls)' "$Dir/binder_probe.exe" @() @(
  'binder probe: all passed')

# libbinder over it, with frameworks/native's servicemanager.
CheckTimed 'binder: servicemanager and libbinder across processes' "$Dir/libbinder_probe.exe" @() @(
  'libbinder probe: all passed')

# The framework's Binder between two dalvikvm processes: binderd and
# servicemanager for a device of this run's own, a service process that
# registers itself with ServiceManager, and a client (tests/dex-services).
# BinderTest reads $ANDROID_ROOT/build.prop, as gesher-app does.
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$device = "tests-$PID"
$logs = Join-Path $root 'services-logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$env:GESHER_BINDER = $device
$env:GESHER_BINDER_TRACE = '1'
$binderd = Start-Process "$Dir/binderd.exe" -ArgumentList $device -PassThru -NoNewWindow `
  -RedirectStandardOutput "$logs/binderd.out" -RedirectStandardError "$logs/binderd.log"
Remove-Item Env:GESHER_BINDER_TRACE
$servicemanager = Start-Process "$Dir/servicemanager.exe" -PassThru -NoNewWindow `
  -RedirectStandardOutput "$logs/servicemanager.out" -RedirectStandardError "$logs/servicemanager.log"
$bcpServices = (('core-oj', 'core-libart', 'core-icu4j', 'framework') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvm = @("-Xbootclasspath:$bcpServices", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
  '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/services-tests.dex"))
$service = Start-Process "$Dir/dalvikvm.exe" -ArgumentList ($dvm + 'gesher.services.BinderTest', 'service') `
  -PassThru -NoNewWindow -RedirectStandardOutput "$logs/service.out" -RedirectStandardError "$logs/service.err"
$failedBefore = $failed
CheckTimed 'binder: two dalvikvm processes, ServiceManager, a Binder passed back, death' "$Dir/dalvikvm.exe" `
  ($dvm + 'gesher.services.BinderTest', 'client') @('BinderTest all passed') 240
foreach ($p in $service, $servicemanager, $binderd) {
  if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
if ($failed -ne $failedBefore) {
  foreach ($f in 'service.out', 'service.err', 'servicemanager.log', 'binderd.log') {
    Write-Host "--- $f"
    Get-Content "$logs/$f" -ErrorAction SilentlyContinue | Select-Object -Last 80 | Out-Host
  }
}
Remove-Item Env:GESHER_BINDER

# gesher-app parses the APK with the framework's own parser (PackageParser2,
# com.gesher.server.ApkPackage) in its single-process mode, the installer's.
CheckTimed 'services: gesher-app parses lifecycle.apk with PackageParser2 (single process)' "$Dir/dalvikvm.exe" @(
  "-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
  '-Xint', '-Xusejit:false', 'com.gesher.app.Main', (NoDrive "$Dir/lifecycle.apk")) @(
  'gesher-app: com.gesher.lifecycle parsed by PackageParser2: 1 activities',
  'gesher-app: stand-in services in this process: activity, activity_task, display, package, user, window',
  'gesher-app: com.gesher.lifecycle.LifecycleActivity onResume',
  'gesher-app: done') 300

# system_server-lite: com.gesher.server.SystemServer parses lifecycle.apk and
# registers the system services with servicemanager over binderd; gesher-app
# --system-server, in another process, gets the package and every service
# through ServiceManager and takes the app to onResume.
$device = "apps-$PID"
$env:GESHER_BINDER = $device
$binderd = Start-Process "$Dir/binderd.exe" -ArgumentList $device -PassThru -NoNewWindow `
  -RedirectStandardOutput "$logs/apps-binderd.out" -RedirectStandardError "$logs/apps-binderd.log"
$servicemanager = Start-Process "$Dir/servicemanager.exe" -PassThru -NoNewWindow `
  -RedirectStandardOutput "$logs/apps-servicemanager.out" -RedirectStandardError "$logs/apps-servicemanager.log"
$dvmApp = @("-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
  '-Xint', '-Xusejit:false')
$server = Start-Process "$Dir/dalvikvm.exe" -PassThru -NoNewWindow `
  -ArgumentList ($dvmApp + 'com.gesher.server.SystemServer', (NoDrive "$Dir/lifecycle.apk")) `
  -RedirectStandardOutput "$logs/system_server.out" -RedirectStandardError "$logs/system_server.err"
$failedBefore = $failed
CheckTimed 'services: gesher-app --system-server, lifecycle.apk to onResume with the services in system_server' "$Dir/dalvikvm.exe" `
  ($dvmApp + 'com.gesher.app.Main', '--system-server', (NoDrive "$Dir/lifecycle.apk")) @(
  'gesher-app: system services from servicemanager: ',
  ' (in another process)',
  'gesher-app: package com.gesher.lifecycle from the system server''s package manager',
  'LifecycleApplication onCreate, package=com.gesher.lifecycle',
  'label=Lifecycle Activity, greeting=Hello from a resource, theme attr=from-theme, windowActionBar=false',
  'gesher-app: com.gesher.lifecycle.LifecycleActivity onResume',
  'gesher-app: done') 400
foreach ($p in $server, $servicemanager, $binderd) {
  if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
Start-Sleep -Milliseconds 500
$serverOut = (Get-Content "$logs/system_server.out" -Raw -ErrorAction SilentlyContinue) + ''
$missing = @(
  'system_server: com.gesher.lifecycle parsed by PackageParser2: 1 activities',
  'system_server: registered package, activity, activity_task, window, display, user with servicemanager'
) | Where-Object { -not $serverOut.Contains($_) }
if (-not $missing) {
  Write-Host 'PASS  services: system_server parses lifecycle.apk and registers package, activity, activity_task, window, display, user'
} else {
  Write-Host "FAIL  services: system_server parses lifecycle.apk and registers its services (missing: $($missing -join ', '))"
  $failed++
}
if ($failed -ne $failedBefore) {
  foreach ($f in 'system_server.out', 'system_server.err', 'apps-servicemanager.log', 'apps-binderd.log') {
    Write-Host "--- $f"
    Get-Content "$logs/$f" -ErrorAction SilentlyContinue | Select-Object -Last 80 | Out-Host
  }
}
Remove-Item Env:GESHER_BINDER
