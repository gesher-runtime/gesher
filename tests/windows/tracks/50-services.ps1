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
  'gesher-app: stand-in services in this process: activity, activity_task, display, input_method, notification, package, user, window',
  'gesher-app: com.gesher.lifecycle.LifecycleActivity onResume',
  'gesher-app: done') 300

# Passes when every line of $Expect is in $Out, in that order.
function CheckOrdered([string] $Name, [string] $Out, [string[]] $Expect, [int] $Code = 0) {
  $at = 0
  $missing = @()
  foreach ($e in $Expect) {
    $i = $Out.IndexOf($e, $at)
    if ($i -lt 0) { $missing += $e } else { $at = $i + $e.Length }
  }
  if ($Code -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
    return $true
  }
  Write-Host "FAIL  $Name (exit $Code; missing or out of order: $($missing -join ' | '))"
  $script:failed++
  return $false
}

# system_server-lite: com.gesher.server.SystemServer parses lifecycle.apk and
# registers the system services with servicemanager over binderd. gesher-app
# --system-server, in another process, is an app process the framework's way
# (android.app.GesherAppProcess): ActivityThread.attach → attachApplication;
# its launcher thread asks for the launch (startActivityAndWait). The server
# binds the process (bindApplication, with the ApplicationSharedMemory region
# passed as a file descriptor over binder) and drives the activity with
# client transactions (LaunchActivityItem + ResumeActivityItem); once it is
# idle the launcher removes the task, and the server pauses, stops and
# destroys the activity (PauseActivityItem, StopActivityItem,
# DestroyActivityItem) and ends the process (scheduleExit). A second run asks
# the activity to finish() itself.
$device = "apps-$PID"
$env:GESHER_BINDER = $device
$binderd = Start-Process "$Dir/binderd.exe" -ArgumentList $device -PassThru -NoNewWindow `
  -RedirectStandardOutput "$logs/apps-binderd.out" -RedirectStandardError "$logs/apps-binderd.log"
$servicemanager = Start-Process "$Dir/servicemanager.exe" -PassThru -NoNewWindow `
  -RedirectStandardOutput "$logs/apps-servicemanager.out" -RedirectStandardError "$logs/apps-servicemanager.log"
$dvmApp = @("-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
  '-Xint', '-Xusejit:false')
$server = Start-Process "$Dir/dalvikvm.exe" -PassThru -NoNewWindow `
  -ArgumentList ($dvmApp + 'com.gesher.server.SystemServer', (NoDrive "$Dir/lifecycle.apk"), (NoDrive "$Dir/components.apk")) `
  -RedirectStandardOutput "$logs/system_server.out" -RedirectStandardError "$logs/system_server.err"
$failedBefore = $failed
$appOut = RunTimed "$Dir/dalvikvm.exe" ($dvmApp + 'com.gesher.app.Main', '--system-server',
  (NoDrive "$Dir/lifecycle.apk")) 400
$appCode = $LASTEXITCODE
$ok = CheckOrdered 'services: gesher-app --system-server, lifecycle.apk to onResume with the services in system_server' $appOut @(
  'gesher-app: system services from servicemanager: ',
  ' (in another process)',
  'gesher-app: package com.gesher.lifecycle from the system server''s package manager',
  'LifecycleApplication onCreate, package=com.gesher.lifecycle',
  'LifecycleActivity onCreate',
  'LifecycleActivity onStart',
  'label=Lifecycle Activity, greeting=Hello from a resource, theme attr=from-theme, windowActionBar=false') $appCode
$ok = (CheckOrdered 'services: the launcher removes the task: onPause, onStop, onDestroy, then the server ends the process' $appOut @(
  'gesher-app: startActivityAndWait: result 0, com.gesher.lifecycle/.LifecycleActivity',
  'gesher-app: removing task ',
  'LifecycleActivity onPause',
  'LifecycleActivity onStop',
  'LifecycleActivity onDestroy',
  'gesher-app: the system server ended the process (scheduleExit)',
  'gesher-app: done') $appCode) -and $ok
if (-not $ok) { Write-Host $appOut }
Start-Sleep -Milliseconds 500
$serverOut = (Get-Content "$logs/system_server.out" -Raw -ErrorAction SilentlyContinue) + ''
$null = CheckOrdered 'services: system_server attaches the app process and drives it with client transactions' $serverOut @(
  'system_server: attachApplication: process ',
  'system_server: bindApplication: process ',
  'system_server: finishAttachApplication: process ',
  'system_server: scheduleTransaction LaunchActivityItem, ResumeActivityItem to com.gesher.lifecycle/com.gesher.lifecycle.LifecycleActivity',
  'system_server: activityResumed: com.gesher.lifecycle/com.gesher.lifecycle.LifecycleActivity',
  'system_server: activityIdle: com.gesher.lifecycle/com.gesher.lifecycle.LifecycleActivity',
  'system_server: removing task ',
  'system_server: scheduleTransaction PauseActivityItem',
  'system_server: activityPaused: ',
  'system_server: scheduleTransaction StopActivityItem',
  'system_server: activityStopped: ',
  'system_server: scheduleTransaction DestroyActivityItem',
  'system_server: activityDestroyed: ',
  'system_server: scheduleExit: process ')

# The activity finishes itself (lifecycle.apk with the extra
# com.gesher.lifecycle.FINISH): finishActivity → the same three steps.
$appOut = RunTimed "$Dir/dalvikvm.exe" ($dvmApp + 'com.gesher.app.Main', '--system-server',
  '--ez', 'com.gesher.lifecycle.FINISH', 'true', (NoDrive "$Dir/lifecycle.apk")) 400
$appCode = $LASTEXITCODE
if (-not (CheckOrdered 'services: LifecycleActivity finish(): the system server pauses, stops and destroys it' $appOut @(
  'LifecycleActivity onResume',
  'LifecycleActivity finish()',
  'LifecycleActivity onPause',
  'LifecycleActivity onStop',
  'LifecycleActivity onDestroy',
  'gesher-app: the system server ended the process (scheduleExit)',
  'gesher-app: done') $appCode)) { Write-Host $appOut }

# The rest of the app model in the system server (tests/apps/components):
# MainActivity runs each step after the previous one ended, then finishes;
# gesher-app --keep-task leaves the task alone, so the run ends with the app.
# Activities: startActivityForResult, a standard activity started on top of
# itself (a new instance), an implicit start through an intent filter, a
# singleTop activity started again (onNewIntent), the back stack unwinding
# (onRestart, the result, onResume). Services: started (stopService,
# stopSelf), bound (the onBind Binder back in the same process), foreground
# (a notification channel and notification to the stand-in notification
# manager). Broadcasts: a registered receiver, a manifest receiver
# (explicit). Providers: the app's own, installed at bindApplication, and
# the settings stand-in.
function CheckSequences([string] $Name, [string] $Out, [object[]] $Sequences, [int] $Code = 0) {
  $missing = @()
  foreach ($seq in $Sequences) {
    $at = 0
    foreach ($e in $seq) {
      $i = $Out.IndexOf($e, $at)
      if ($i -lt 0) { $missing += $e } else { $at = $i + $e.Length }
    }
  }
  if ($Code -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
    return $true
  }
  Write-Host "FAIL  $Name (exit $Code; missing or out of order: $($missing -join ' | '))"
  $script:failed++
  return $false
}
$appOut = RunTimed "$Dir/dalvikvm.exe" ($dvmApp + 'com.gesher.app.Main', '--system-server', '--keep-task',
  (NoDrive "$Dir/components.apk")) 400
$appCode = $LASTEXITCODE
$c = 'components: '
$ok = CheckSequences 'services: activities start activities: startActivityForResult, standard, implicit, singleTop (onNewIntent), the back stack and the result' $appOut @(
  @("${c}MainActivity onResume #1", "${c}MainActivity onPause", "${c}SecondActivity#1 onCreate",
    "${c}SecondActivity#1 started by .MainActivity, in MainActivity's task: true", "${c}SecondActivity#1 onResume #1",
    "${c}SecondActivity#1 onPause", "${c}SecondActivity#2 onCreate",
    "${c}SecondActivity#2 started by startActivity (no result), in MainActivity's task: true", "${c}SecondActivity#2 onResume #1",
    "${c}SecondActivity#2 starts com.gesher.components.SINGLE_TOP (implicit)", "${c}SecondActivity#2 onPause",
    "${c}SingleTopActivity#1 onCreate", "${c}SingleTopActivity#1 intent n=1", "${c}SingleTopActivity#1 onResume #1",
    "${c}SingleTopActivity#1 onPause", "${c}SingleTopActivity#1 onNewIntent n=2", "${c}SingleTopActivity#1 onResume #2",
    "${c}SingleTopActivity#1 finish()", "${c}SingleTopActivity#1 onPause",
    "${c}SecondActivity#2 onResume #2", "${c}SecondActivity#2 finish()",
    "${c}SecondActivity#1 onResume #2", "${c}SecondActivity#1 setResult(RESULT_OK, answer=42), finish()",
    "${c}MainActivity onRestart", "${c}MainActivity onStart",
    "${c}MainActivity onActivityResult: request 1, result RESULT_OK, answer 42", "${c}MainActivity onResume #2"),
  @("${c}MainActivity onPause", "${c}MainActivity onStop", "${c}MainActivity onRestart"),
  @("${c}SingleTopActivity#1 finish()", "${c}SingleTopActivity#1 onStop", "${c}SingleTopActivity#1 onDestroy"),
  @("${c}SecondActivity#2 finish()", "${c}SecondActivity#2 onStop", "${c}SecondActivity#2 onDestroy"),
  @("${c}SecondActivity#1 setResult(RESULT_OK, answer=42), finish()", "${c}SecondActivity#1 onStop", "${c}SecondActivity#1 onDestroy")
) $appCode
if ($appOut.Contains('SingleTopActivity#2')) {
  Write-Host 'FAIL  services: singleTop made a second instance'
  $failed++
  $ok = $false
}
$ok = (CheckSequences 'services: startService, stopService and stopSelf: onCreate, onStartCommand, onDestroy' $appOut @(
  ,@("${c}step: started service, stopService", "${c}stopService: true", "${c}CounterService onCreate",
    "${c}CounterService onStartCommand start id 1, n=1", "${c}CounterService onStartCommand start id 2, n=2",
    "${c}CounterService onDestroy", "${c}step: started service, stopSelf", "${c}CounterService onCreate",
    "${c}CounterService onStartCommand start id 1, n=0", "${c}CounterService stopSelf(1)", "${c}CounterService onDestroy")
) $appCode) -and $ok
$ok = (CheckSequences 'services: bindService and unbindService, the Binder from onBind back in the same process' $appOut @(
  ,@("${c}step: bound service", "${c}bindService: true", "${c}LocalService onCreate", "${c}LocalService onBind",
    "${c}onServiceConnected .LocalService: LocalBinder, the service's own Binder: true, add(2, 3) = 5",
    "${c}unbindService", "${c}LocalService onUnbind", "${c}LocalService onDestroy")
) $appCode) -and $ok
$ok = (CheckSequences 'services: a foreground service: startForegroundService, its notification to the stand-in notification manager' $appOut @(
  ,@("${c}step: foreground service", "${c}ForegroundService onCreate", "${c}ForegroundService startForeground(7), channel gesher",
    "${c}ForegroundService stopForeground, stopSelf", "${c}ForegroundService onDestroy")
) $appCode) -and $ok
$ok = (CheckSequences 'services: broadcasts: registerReceiver and sendBroadcast, a manifest receiver for an explicit broadcast' $appOut @(
  ,@("${c}step: broadcasts", "${c}registered receiver onReceive com.gesher.components.PING, n=7",
    "${c}sendBroadcast to ManifestReceiver (explicit)",
    "${c}ManifestReceiver onReceive com.gesher.components.HELLO (ReceiverRestrictedContext)")
) $appCode) -and $ok
$ok = (CheckSequences 'services: content providers: the app''s own (installed at bindApplication), insert and query; Settings from the settings stand-in' $appOut @(
  @("${c}ItemsProvider onCreate", "${c}ComponentsApplication onCreate, providers installed: true"),
  @("${c}step: content providers", "${c}insert apple: content://com.gesher.components.items/items/1",
    "${c}insert banana: content://com.gesher.components.items/items/2",
    "${c}query content://com.gesher.components.items/items: 2 rows: apple, banana",
    "${c}Settings.System font_scale=1.0, Settings.Secure android_id=6765736865720000, Settings.Global auto_time=1",
    "${c}Settings.System gesher_test after putInt: 7", "${c}all steps ran", "${c}MainActivity onDestroy",
    'gesher-app: the system server ended the process (scheduleExit)', 'gesher-app: done')
) $appCode) -and $ok
if (-not $ok) { Write-Host $appOut }
Start-Sleep -Milliseconds 500
$serverOut = (Get-Content "$logs/system_server.out" -Raw -ErrorAction SilentlyContinue) + ''
$s = 'system_server: '
$null = CheckSequences 'services: system_server drives components.apk: NewIntentItem, ActivityResultItem, services, receivers, providers, notifications' $serverOut @(
  ,@("${s}com.gesher.components parsed by PackageParser2: 3 activities, 3 services, 1 receivers, 1 providers",
    "${s}publishContentProviders: com.gesher.components.items (com.gesher.components.ItemsProvider)",
    "${s}scheduleTransaction NewIntentItem to com.gesher.components/com.gesher.components.SingleTopActivity",
    "${s}scheduleTransaction ActivityResultItem, ResumeActivityItem to com.gesher.components/com.gesher.components.MainActivity",
    "${s}scheduleCreateService com.gesher.components/.CounterService", "${s}scheduleServiceArgs com.gesher.components/.CounterService",
    "${s}scheduleStopService com.gesher.components/.CounterService", "${s}stopSelf com.gesher.components/.CounterService (start id 1)",
    "${s}scheduleBindService com.gesher.components/.LocalService", "${s}publishService com.gesher.components/.LocalService",
    "${s}scheduleUnbindService com.gesher.components/.LocalService",
    "${s}notification channel gesher created for com.gesher.components",
    "${s}notification 7 posted for com.gesher.components: channel gesher, title `"Gesher foreground service`"",
    "${s}registerReceiver from com.gesher.components: com.gesher.components.PING",
    "${s}scheduleReceiver com.gesher.components/com.gesher.components.ManifestReceiver",
    "${s}settings: system gesher_test = 7", "${s}scheduleExit: process ")
)
foreach ($p in $server, $servicemanager, $binderd) {
  if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
Start-Sleep -Milliseconds 500
$serverOut = (Get-Content "$logs/system_server.out" -Raw -ErrorAction SilentlyContinue) + ''
$missing = @(
  'system_server: com.gesher.lifecycle parsed by PackageParser2: 1 activities',
  'system_server: registered package, activity, activity_task, window, display, user, input_method, notification with servicemanager',
  'system_server: finishing com.gesher.lifecycle/com.gesher.lifecycle.LifecycleActivity (app-request)'
) | Where-Object { -not $serverOut.Contains($_) }
if (-not $missing) {
  Write-Host 'PASS  services: system_server parses lifecycle.apk and registers package, activity, activity_task, window, display, user, input_method, notification'
} else {
  Write-Host "FAIL  services: system_server parses lifecycle.apk and registers its services (missing: $($missing -join ', '))"
  $failed++
}
$standIns = ([regex]::Matches($serverOut + (Get-Content "$logs/system_server.err" -Raw -ErrorAction SilentlyContinue), 'stand-in \S+ not implemented') | ForEach-Object Value) -join '; '
Write-Host "services: system_server stand-in methods called but not implemented: $(if ($standIns) { $standIns } else { 'none' })"
if ($failed -ne $failedBefore) {
  foreach ($f in 'system_server.out', 'system_server.err', 'apps-servicemanager.log', 'apps-binderd.log') {
    Write-Host "--- $f"
    Get-Content "$logs/$f" -ErrorAction SilentlyContinue | Select-Object -Last 80 | Out-Host
  }
}
Remove-Item Env:GESHER_BINDER
