# Device services backed by Windows (devsvc track, docs/services.md "Device
# services", docs/decisions/0038-devsvc-windows-backed-services.md): alarms
# that fire (also with the app's process ended), jobs, the clipboard linked
# with Windows', notifications as toasts (and a notification listener), wake
# locks as power requests, the battery, clock changes. tests/apps/devsvc
# (devsvc.apk) runs one activity per service on a Gesher device of its own.
# Dot-sourced by run-tests.ps1 after 50-services.ps1, whose helpers
# (RunTimed, StartTimed, WaitTimed, CheckOrdered, DeviceState, DeviceAlive,
# DeviceLogs, $launcher, $deviceLogs, $stateFile) these use.

$devsvcApk = Join-Path $Dir 'devsvc.apk'
if (-not (Test-Path $devsvcApk) -or -not (Get-Command StartTimed -ErrorAction SilentlyContinue) -or -not $launcher) {
  Write-Host "FAIL  devsvc: devsvc.apk or the device helpers of 50-services.ps1 are missing"
  $failed++
  return
}

$failedBefore = $failed
$devsvcPkg = 'com.gesher.devsvc'
$devsvcAppId = "Gesher.App.$devsvcPkg"
$devsvcLog = Join-Path $deviceLogs "$devsvcPkg.log"
$serverLog = Join-Path $deviceLogs 'system_server.log'
$env:GESHER_APP_WINDOW = 'headless'

function DevsvcText([string] $Path) {
  # The device's processes keep their logs open: read with sharing.
  if (-not (Test-Path $Path)) { return '' }
  $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
  try {
    return [IO.StreamReader]::new($stream).ReadToEnd()
  } finally {
    $stream.Dispose()
  }
}

# The package log from a mark (its length) on.
function DevsvcSince([int] $Mark) {
  $t = DevsvcText $devsvcLog
  if ($t.Length -le $Mark) { return '' }
  return $t.Substring($Mark)
}

# Waits until the package log has every one of $Texts after $Mark.
function DevsvcWait([int] $Mark, [string[]] $Texts, [int] $Seconds) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $t = DevsvcSince $Mark
    if (-not ($Texts | Where-Object { -not $t.Contains($_) })) { return $true }
    Start-Sleep -Milliseconds 300
  } while ((Get-Date) -lt $deadline)
  return $false
}

function DevsvcLaunch([string] $Activity) {
  return StartTimed "$Dir/dalvikvm.exe" ($launcher + 'launch', (NoDrive $devsvcApk), '--activity', ".$Activity",
    '--keep-task', '--headless')
}

function DevsvcCheck([string] $Name, [string[]] $Problems, [string] $Details) {
  if (-not $Problems) {
    Write-Host "PASS  $Name"
  } else {
    Write-Host "FAIL  $Name ($($Problems -join '; '))"
    Write-Host $Details
    $script:failed++
  }
}

# Windows' toast history for the app (WinRT: Windows PowerShell 5.1 has the
# projection, PowerShell 7 does not): the tags, one per line.
$historyScript = Join-Path $root 'devsvc-toast-history.ps1'
Set-Content -Path $historyScript -Encoding UTF8 -Value @'
param([string] $AppId)
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
foreach ($t in [Windows.UI.Notifications.ToastNotificationManager]::History.GetHistory($AppId)) { 'tag=' + $t.Tag + ' group=' + $t.Group }
'@
function ToastTags {
  $lines = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $historyScript -AppId $devsvcAppId 2>&1 | Out-String
  return $lines
}

# Gesher's toast activator (hwsvc2, ADR 0051) as the installer registers it: a local
# server under HKCU whose command runs the launcher's toast-activated (here a script with
# this run's environment and runtime, the installer's runs gesher-app.ps1). Registered
# before the device starts, so the app's AppUserModelID names it as its CustomActivator.
$activatorClsid = '{C18D9445-A7F0-472E-985F-AC354EBEB660}'
$activatorKey = "HKCU:\Software\Classes\CLSID\$activatorClsid"
$activatorLog = Join-Path $deviceLogs 'toast-activated.log'
$activatorCmd = Join-Path $root 'devsvc-toast-activated.cmd'
$wrapper = @('@echo off')
foreach ($e in Get-ChildItem Env: | Where-Object { $_.Name -match '^(ANDROID_|GESHER_)' }) { $wrapper += "set `"$($e.Name)=$($e.Value)`"" }
$quoted = @($launcher | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } })
$wrapper += "`"$Dir\dalvikvm.exe`" $($quoted -join ' ') toast-activated %* >> `"$activatorLog`" 2>&1"
Set-Content -Path $activatorCmd -Value $wrapper -Encoding ascii
New-Item "$activatorKey\LocalServer32" -Force | Out-Null
Set-Item $activatorKey -Value 'Gesher toast activator'
Set-Item "$activatorKey\LocalServer32" -Value "`"$env:SystemRoot\System32\cmd.exe`" /d /c `"$activatorCmd`""
Remove-Item $activatorLog -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $deviceLogs | Out-Null

# A device of this script's own, its job periods divided by 60 (a periodic job's
# 15-minute minimum becomes 15 s), with devsvc.apk installed.
Remove-Item $devsvcLog -ErrorAction SilentlyContinue
$env:GESHER_JOBS_TIME_SCALE = '60'
$startOut = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'start') 400
Remove-Item Env:GESHER_JOBS_TIME_SCALE
$installOut = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'install', (NoDrive $devsvcApk)) 300
if ($LASTEXITCODE -ne 0) {
  Write-Host "FAIL  devsvc: the device starts and installs devsvc.apk (exit $LASTEXITCODE)"
  Write-Host ($startOut + $installOut)
  $failed++
}

# --- alarms ------------------------------------------------------------------------
# A listener alarm fires in the activity's process; once it finished and its
# process ended, an exact RTC_WAKEUP alarm starts another process for its
# manifest receiver, an ELAPSED_REALTIME window alarm starts a service, a
# repeating alarm fires twice (its receiver cancels it), an alarm clock fires;
# a cancelled alarm never does.
$mark = (DevsvcText $devsvcLog).Length
$serverMark = (DevsvcText $serverLog).Length
$t = DevsvcLaunch 'AlarmActivity'
$out = WaitTimed $t 400
$code = $LASTEXITCODE
$ok = DevsvcWait $mark @('devsvc: alarms: AlarmReceiver onReceive com.gesher.devsvc.ALARM',
  'devsvc: alarms: AlarmService onStartCommand', 'devsvc: alarms: repeating alarm cancelled',
  'devsvc: alarms: AlarmReceiver onReceive com.gesher.devsvc.CLOCK') 120
Start-Sleep 3
$log = DevsvcSince $mark
$server = (DevsvcText $serverLog)
if ($server.Length -gt $serverMark) { $server = $server.Substring($serverMark) } else { $server = '' }
$activityPid = if ($log -match 'alarms: listener alarm set \(pid (\d+)\)') { $Matches[1] } else { '' }
# (EXTRA_ALARM_COUNT is not filled into an immutable PendingIntent, as on Android.)
$receiverPid = if ($log -match 'AlarmReceiver onReceive com\.gesher\.devsvc\.ALARM \(alarm count \d+\) \(pid (\d+)\)') { $Matches[1] } else { '' }
$problems = @()
if ($code -ne 0) { $problems += "launch exit $code" }
if (-not $log.Contains('devsvc: alarms: OnAlarmListener onAlarm after ')) { $problems += 'the listener alarm did not fire' }
DevsvcCheck 'devsvc: alarms: an OnAlarmListener alarm fires in the app''s process (setExact, ELAPSED_REALTIME)' $problems ($out + "`n--- package log`n" + $log)
$problems = @()
if (-not $receiverPid) { $problems += 'the RTC_WAKEUP alarm''s receiver did not run' }
elseif ($receiverPid -eq $activityPid) { $problems += "the receiver ran in the activity's process ($activityPid)" }
if (-not $out.Contains("gesher-app: the process of $devsvcPkg ended")) { $problems += 'the activity''s process did not end first' }
if (-not $server.Contains("starting a process for $devsvcPkg (broadcast to com.gesher.devsvc.AlarmReceiver)")) { $problems += 'the system server did not start a process for the alarm' }
if (-not $server.Contains('alarm: firing RTC_WAKEUP alarm of com.gesher.devsvc')) { $problems += 'no RTC_WAKEUP alarm fired' }
DevsvcCheck 'devsvc: alarms: with the app''s process ended, an exact RTC_WAKEUP alarm starts a process for its manifest receiver (PendingIntent broadcast)' $problems ($log + "`n--- system server`n" + $server)
$problems = @()
if (-not $ok) { $problems += 'not every alarm fired in 120 s' }
if (-not $log.Contains('devsvc: alarms: repeating alarm 2 ')) { $problems += 'the repeating alarm did not fire twice' }
if ($log.Contains('devsvc: alarms: repeating alarm 3 ')) { $problems += 'the repeating alarm fired after it was cancelled' }
if ($log.Contains('CANCELLED')) { $problems += 'the cancelled alarm fired' }
if (-not $log.Contains('next alarm clock in ')) { $problems += 'no next alarm clock' }
DevsvcCheck 'devsvc: alarms: setWindow to a service, setRepeating (twice, then cancelled by its receiver), setAlarmClock (getNextAlarmClock); a cancelled alarm never fires' $problems ($log + "`n--- system server`n" + $server)

# --- jobs --------------------------------------------------------------------------
# A one-off job (minimum latency, deadline, charging: a PC on AC power) and a
# periodic job (its first period at once) run in a process started for them; the
# periodic one runs again in its next period (15 s here), and is cancelled.
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'JobActivity'
$out = WaitTimed $t 400
$code = $LASTEXITCODE
$ok = DevsvcWait $mark @('devsvc: jobs: onStartJob 1 (one-off) run 1', 'devsvc: jobs: job 1 finished',
  'devsvc: jobs: onStartJob 2 (periodic) run 1', 'devsvc: jobs: onStartJob 2 (periodic) run 2',
  'devsvc: jobs: periodic job cancelled after 2 runs') 150
Start-Sleep 2
$log = DevsvcSince $mark
$problems = @()
if ($code -ne 0) { $problems += "launch exit $code" }
if (-not $log.Contains('devsvc: jobs: scheduled (1, 1), pending 2')) { $problems += 'schedule did not answer RESULT_SUCCESS with 2 pending' }
if (-not $ok) { $problems += 'not every job ran in 150 s' }
if ($log.Contains('onStartJob 2 (periodic) run 3')) { $problems += 'the cancelled periodic job ran again' }
DevsvcCheck 'devsvc: jobs: JobScheduler runs a one-off job (latency, deadline, charging) and a periodic job in the app''s JobService, its process started for them; the periodic job runs again in its next period' $problems ($out + "`n--- package log`n" + $log + "`n--- system server`n" + ((DevsvcText $serverLog) -split "`n" | Select-String 'jobscheduler' | Out-String))

# --- clipboard ---------------------------------------------------------------------
# The app's clip (HTML text) is on Windows' clipboard, as text and as HTML
# Format; what PowerShell puts there reaches the app's listener.
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'ClipActivity'
$set = DevsvcWait $mark @('devsvc: clip: set; reads back') 300
Start-Sleep 1
$winText = (Get-Clipboard -Raw) -as [string]
$winHtml = & powershell.exe -NoProfile -Command 'Get-Clipboard -TextFormatType Html' 2>&1 | Out-String
Set-Clipboard -Value 'from windows 57'
$out = WaitTimed $t 200
$code = $LASTEXITCODE
$log = DevsvcSince $mark
$problems = @()
if (-not $set) { $problems += 'the app did not set its clip' }
if ($winText -ne 'android text 57') { $problems += "Windows' clipboard has '$winText'" }
if (-not $winHtml.Contains('<b>android html 57</b>')) { $problems += "Windows' HTML Format has no '<b>android html 57</b>'" }
if (-not $log.Contains('devsvc: clip: changed from Windows: from windows 57 (label Windows)')) { $problems += 'the app''s listener did not get the clip from Windows' }
if ($code -ne 0) { $problems += "launch exit $code" }
DevsvcCheck 'devsvc: clipboard: an app''s clip is on Windows'' clipboard (Get-Clipboard: text, and HTML Format), and Set-Clipboard reaches the app''s OnPrimaryClipChangedListener and getPrimaryClip' $problems ($out + "`n--- package log`n" + $log + "`n--- HTML`n" + $winHtml)

# --- notifications -----------------------------------------------------------------
# Two notifications are two toasts of the app's AppUserModelID in Windows' toast
# history (tags 7 and 8|second); cancel removes one, cancelAll the other. The
# app's notification listener is bound and told.
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'NotifyActivity'
$posted = DevsvcWait $mark @('devsvc: notifications: posted 7') 300
Start-Sleep 2
$tags1 = ToastTags
$cancelled = DevsvcWait $mark @('devsvc: notifications: cancelled 7') 60
Start-Sleep 2
$tags2 = ToastTags
$all = DevsvcWait $mark @('devsvc: notifications: cancelled all') 60
Start-Sleep 2
$tags3 = ToastTags
$out = WaitTimed $t 200
$code = $LASTEXITCODE
$log = DevsvcSince $mark
$problems = @()
if (-not ($posted -and $cancelled -and $all)) { $problems += 'the app did not post and cancel' }
if (-not ($tags1.Contains('tag=7 group=android') -and $tags1.Contains('tag=8|second group=android'))) { $problems += "after posting the history has: $($tags1.Trim())" }
if ($tags2.Contains('tag=7 group=android') -or -not $tags2.Contains('tag=8|second')) { $problems += "after cancel(7) the history has: $($tags2.Trim())" }
if ($tags3.Contains('tag=')) { $problems += "after cancelAll the history has: $($tags3.Trim())" }
if ($code -ne 0) { $problems += "launch exit $code" }
DevsvcCheck "devsvc: notifications: posted notifications are toasts of $devsvcAppId in Windows' toast history (ToastNotificationManager.History), removed by cancel and cancelAll" $problems ($out + "`n--- package log`n" + $log + "`n--- system server`n" + ((DevsvcText $serverLog) -split "`n" | Select-String 'notification' | Out-String))
$problems = @()
foreach ($e in 'devsvc: listener: connected', 'devsvc: listener: posted com.gesher.devsvc 7 "Gesher notification"', 'devsvc: listener: removed com.gesher.devsvc 7') {
  if (-not $log.Contains($e)) { $problems += "no '$e'" }
}
DevsvcCheck 'devsvc: notifications: a NotificationListenerService is bound and gets onListenerConnected, onNotificationPosted and onNotificationRemoved' $problems $log

# --- power and battery -------------------------------------------------------------
# A partial wake lock is a Windows power request (powercfg /requests) until it
# is released; the battery is Windows' (GetSystemPowerStatus).
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'PowerActivity'
$held = DevsvcWait $mark @('devsvc: power: wake lock held') 300
Start-Sleep 2
$req1 = & powercfg /requests 2>&1 | Out-String
$released = DevsvcWait $mark @('devsvc: power: wake lock released') 60
Start-Sleep 2
$req2 = & powercfg /requests 2>&1 | Out-String
$out = WaitTimed $t 200
$code = $LASTEXITCODE
$log = DevsvcSince $mark
$hasBattery = [bool](Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
$problems = @()
if (-not ($held -and $released)) { $problems += 'the app did not hold and release its wake lock' }
if (-not $req1.Contains('Android wake lock: com.gesher.devsvc (devsvc:test)')) { $problems += 'powercfg /requests does not list the wake lock' }
if ($req2.Contains('com.gesher.devsvc (devsvc:test)')) { $problems += 'the request stayed after the release' }
if ($code -ne 0) { $problems += "launch exit $code" }
DevsvcCheck 'devsvc: power: a partial wake lock is a Windows power request (powercfg /requests) until released' $problems ($log + "`n--- powercfg (held)`n" + $req1 + "`n--- powercfg (released)`n" + $req2)
$problems = @()
if ($hasBattery) {
  if ($log -notmatch 'power: battery present=true level=\d+/100') { $problems += 'the battery is not present in ACTION_BATTERY_CHANGED' }
} elseif (-not $log.Contains('devsvc: power: battery present=false level=100/100 plugged=1 status=5')) {
  $problems += 'a PC without a battery is not AC-plugged, full, battery not present'
}
if ($log -notmatch 'power: BatteryManager capacity=\d+ charging=(true|false)') { $problems += 'BatteryManager does not answer' }
DevsvcCheck "devsvc: battery: the sticky ACTION_BATTERY_CHANGED and BatteryManager from GetSystemPowerStatus (this PC: $(if ($hasBattery) { 'a battery' } else { 'no battery, AC' }))" $problems $log

# --- clock changes -----------------------------------------------------------------
# WM_TIMECHANGE (what Windows broadcasts when the time is set; sent here to the
# device's event window, without changing the clock: Windows does not pass an
# application's broadcast of it on) becomes TIME_SET for registered and
# manifest receivers.
if (-not ('Devsvc.Broadcast' -as [type])) {
  Add-Type -Namespace Devsvc -Name Broadcast -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr SendMessageTimeoutW(System.IntPtr hWnd, uint msg, System.IntPtr wParam, System.IntPtr lParam, uint flags, uint timeout, out System.IntPtr result);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowW(string className, string windowName);
'@
}
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'TimeActivity'
$registered = DevsvcWait $mark @('devsvc: time: receiver registered') 300
Start-Sleep 1
$r = [IntPtr]::Zero
# The system server logs its event window's handle (WindowsHost.addListener); send
# WM_TIMECHANGE to it by handle. FindWindow is only a fallback: on the CI runners it
# does not see the window (another window station or desktop).
$eventWindow = [IntPtr]::Zero
if ((DevsvcText $serverLog) -match 'devsvc: Windows events from a hidden window 0x([0-9a-fA-F]+)') {
  $eventWindow = [IntPtr]::new([Convert]::ToInt64($Matches[1], 16))
}
if ($eventWindow -eq [IntPtr]::Zero) {
  $eventWindow = [Devsvc.Broadcast]::FindWindowW('GesherDeviceServices', $null)
}
$sent = [IntPtr]::Zero
if ($eventWindow -ne [IntPtr]::Zero) {
  $sent = [Devsvc.Broadcast]::SendMessageTimeoutW($eventWindow, 0x001E, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref] $r)
}
$out = WaitTimed $t 200
$code = $LASTEXITCODE
$got = DevsvcWait $mark @('devsvc: time: manifest receiver got android.intent.action.TIME_SET') 60
$log = DevsvcSince $mark
$problems = @()
if (-not $registered) { $problems += 'the receiver was not registered' }
if ($eventWindow -eq [IntPtr]::Zero) { $problems += 'the device''s event window (GesherDeviceServices) was not found' }
elseif ($sent -eq [IntPtr]::Zero) { $problems += "WM_TIMECHANGE was not delivered to $eventWindow (SendMessageTimeout failed or timed out)" }
if (-not $log.Contains('devsvc: time: registered receiver got android.intent.action.TIME_SET')) { $problems += 'the registered receiver got no TIME_SET' }
if (-not $got) { $problems += 'the manifest receiver got no TIME_SET' }
if ($code -ne 0) { $problems += "launch exit $code" }
DevsvcCheck 'devsvc: clock: WM_TIMECHANGE becomes TIME_SET for registered and manifest receivers' $problems ($out + "`n--- package log`n" + $log + "`n--- system server`n" + ((DevsvcText $serverLog) -split "`n" | Select-String 'alarm:|devsvc:|TIME_SET' | Out-String))

# --- hwsvc2: the vibrator ------------------------------------------------------------
# A device without a vibrator (ADR 0051): hasVibrator false, no vibrator ids, the null
# vibrator; vibrate and cancel are accepted.
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'VibrateActivity'
$out = WaitTimed $t 300
$code = $LASTEXITCODE
$null = DevsvcWait $mark @('devsvc: vibrator: vibrate and cancel returned') 30
$log = DevsvcSince $mark
$problems = @()
if ($code -ne 0) { $problems += "launch exit $code" }
foreach ($e in 'devsvc: vibrator: SystemVibrator hasVibrator=false hasAmplitudeControl=false',
    'devsvc: vibrator: manager SystemVibratorManager ids=0 default hasVibrator=false vibrator(0)=NullVibrator',
    'devsvc: vibrator: vibrate and cancel returned') {
  if (-not $log.Contains($e)) { $problems += "no '$e'" }
}
if ($log.Contains('no vibrator manager service')) { $problems += 'the app found no vibrator_manager service' }
DevsvcCheck 'devsvc: vibrator: getSystemService(VIBRATOR_SERVICE) and VibratorManager as on a device without a vibrator (vibrator_manager: no ids, hasVibrator false, the null vibrator; vibrate and cancel accepted)' $problems ($out + "`n--- package log`n" + $log)

# --- hwsvc2: content URI job triggers ------------------------------------------------
# A job triggered by changes under the app's provider runs once for two inserts (its
# update delay batches them) with both URIs; a ContentObserver is told of each.
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'ContentActivity'
$out = WaitTimed $t 300
$code = $LASTEXITCODE
$ok = DevsvcWait $mark @('devsvc: jobs: onStartJob 3 (content) run 1', 'devsvc: jobs: job 3 finished') 60
Start-Sleep 3
$log = DevsvcSince $mark
$problems = @()
if ($code -ne 0) { $problems += "launch exit $code" }
if (-not $ok) { $problems += 'the content-triggered job did not run in 60 s' }
if ($log -notmatch 'content: job scheduled \(1\), pending reason \d+') { $problems += 'schedule did not succeed' }
if (-not $log.Contains('devsvc: jobs: triggered by [content://com.gesher.devsvc.notes/items/1, content://com.gesher.devsvc.notes/items/2] authorities [com.gesher.devsvc.notes]')) { $problems += 'the job''s JobParameters lack the triggered URIs and authority' }
if ($log.Contains('onStartJob 3 (content) run 2')) { $problems += 'the job ran twice' }
foreach ($n in 1, 2) {
  if (-not $log.Contains("devsvc: content: observer onChange content://com.gesher.devsvc.notes/items/$n")) { $problems += "the observer was not told of items/$n" }
}
DevsvcCheck 'devsvc: jobs: a content URI trigger (TriggerContentUri, descendants) runs the job once for a provider''s two notifyChange calls, with the triggered URIs and authority; a ContentObserver gets both changes' $problems ($out + "`n--- package log`n" + $log + "`n--- system server`n" + ((DevsvcText $serverLog) -split "`n" | Select-String 'jobscheduler' | Out-String))

# --- hwsvc2: sleep and resume ------------------------------------------------------
# WM_POWERBROADCAST to the device's event window (as Windows sends it): PBT_APMSUSPEND is
# SCREEN_OFF (not interactive) and holds an RTC alarm that comes due; PBT_APMRESUMEAUTOMATIC
# is SCREEN_ON and the alarm fires then.
function EventWindow {
  $w = [IntPtr]::Zero
  $all = [regex]::Matches((DevsvcText $serverLog), 'devsvc: Windows events from a hidden window 0x([0-9a-fA-F]+)')
  if ($all.Count -gt 0) { $w = [IntPtr]::new([Convert]::ToInt64($all[$all.Count - 1].Groups[1].Value, 16)) }
  return $w
}
function SendToEventWindow([int] $Msg, [long] $WParam) {
  $w = EventWindow
  if ($w -eq [IntPtr]::Zero) { return $false }
  $r = [IntPtr]::Zero
  return [Devsvc.Broadcast]::SendMessageTimeoutW($w, $Msg, [IntPtr]::new($WParam), [IntPtr]::Zero, 2, 5000, [ref] $r) -ne [IntPtr]::Zero
}
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'SleepActivity'
$ready = DevsvcWait $mark @('devsvc: sleep: ready, interactive true') 300
$suspendSent = SendToEventWindow 0x0218 4       # WM_POWERBROADCAST, PBT_APMSUSPEND
$off = DevsvcWait $mark @('devsvc: sleep: got android.intent.action.SCREEN_OFF, interactive false') 30
Start-Sleep 8                                     # past the alarm's time (4 s)
$early = (DevsvcSince $mark).Contains('devsvc: sleep: the RTC alarm fired')
$resumeSent = SendToEventWindow 0x0218 0x12     # PBT_APMRESUMEAUTOMATIC
$on = DevsvcWait $mark @('devsvc: sleep: got android.intent.action.SCREEN_ON, interactive true', 'devsvc: sleep: the RTC alarm fired') 30
$out = WaitTimed $t 120
$code = $LASTEXITCODE
$log = DevsvcSince $mark
$problems = @()
if (-not $ready) { $problems += 'the activity did not get ready' }
if (-not ($suspendSent -and $resumeSent)) { $problems += 'WM_POWERBROADCAST was not delivered to the event window' }
if (-not $off) { $problems += 'no SCREEN_OFF (not interactive) after PBT_APMSUSPEND' }
if ($early) { $problems += 'the RTC alarm fired while Windows was "asleep"' }
if (-not $on) { $problems += 'no SCREEN_ON and alarm after PBT_APMRESUMEAUTOMATIC' }
if ($code -ne 0) { $problems += "launch exit $code" }
DevsvcCheck 'devsvc: sleep: PBT_APMSUSPEND is SCREEN_OFF (isInteractive false) and holds a due RTC alarm; PBT_APMRESUMEAUTOMATIC is SCREEN_ON and the alarm catches up' $problems ($out + "`n--- package log`n" + $log + "`n--- system server`n" + ((DevsvcText $serverLog) -split "`n" | Select-String 'alarm:|power:' | Out-String))

# --- hwsvc2: a real time zone change -------------------------------------------------
# Windows' zone set with Set-TimeZone (restored after): the device notices it (its clock
# watch looks at GetDynamicTimeZoneInformation at least once a minute, besides
# WM_TIMECHANGE/WM_SETTINGCHANGE, which do not reach its window on the runners) and sends
# TIMEZONE_CHANGED with the zone's IANA id; app processes reset their default zone.
$zoneBefore = (Get-TimeZone).Id
$zoneTo = if ($zoneBefore -ne 'Tokyo Standard Time') { 'Tokyo Standard Time' } else { 'Pacific Standard Time' }
$iana = ''
$null = [TimeZoneInfo]::TryConvertWindowsIdToIanaId($zoneTo, [ref] $iana)
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'ZoneActivity'
$registered = DevsvcWait $mark @('devsvc: zone: receiver registered') 300
$setError = ''
try {
  Set-TimeZone -Id $zoneTo -ErrorAction Stop
  $got = DevsvcWait $mark @('devsvc: zone: got android.intent.action.TIMEZONE_CHANGED') 150
} catch {
  $setError = "$_"
  $got = $false
} finally {
  Set-TimeZone -Id $zoneBefore -ErrorAction SilentlyContinue
}
$out = WaitTimed $t 200
$code = $LASTEXITCODE
$log = DevsvcSince $mark
$problems = @()
if ($setError) { $problems += "Set-TimeZone failed: $setError" }
if (-not $registered) { $problems += 'the receiver was not registered' }
if (-not $got) { $problems += 'no TIMEZONE_CHANGED in 150 s' }
$default = if ($log -match 'zone: got android\.intent\.action\.TIMEZONE_CHANGED time-zone=(\S+) default=(\S+)') { $Matches[2] } else { '' }
if ($got -and -not $log.Contains("time-zone=$iana ")) { $problems += "TIMEZONE_CHANGED does not name $iana" }
# The default zone follows Windows' once the runtime reads Windows' zone (the posix
# track's /etc/timezone, with its tzdata in the build).
$followsWindows = Test-Path (Join-Path $Dir 'tzdata')
if ($got -and $followsWindows -and $default -ne $iana) { $problems += "the app's default zone after it is $default, not $iana" }
if ($code -ne 0) { $problems += "launch exit $code" }
DevsvcCheck "devsvc: clock: a real time zone change (Set-TimeZone $zoneTo) becomes TIMEZONE_CHANGED ($iana) for a registered receiver; the app's default zone then: $default" $problems ($out + "`n--- package log`n" + $log + "`n--- system server`n" + ((DevsvcText $serverLog) -split "`n" | Select-String 'alarm:' | Out-String))

# --- hwsvc2: toast clicks after the device stopped -----------------------------------
# What Windows' notification platform does with a click on a toast whose app id names a
# CustomActivator: if no process serves the class, it runs the activator's LocalServer32
# command, then calls INotificationActivationCallback.Activate with the app id and the
# toast's launch (or button) arguments. Here with the device stopped (the launcher COM
# runs starts it and hands it the click; the stored notification's intent is sent), and
# again with the device running (its system server serves the activator).
if (-not ('GesherToastActivation' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
[ComImport, Guid("53E31837-6600-4A81-9395-75CFFE746F94"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface INotificationActivationCallback {
  void Activate([MarshalAs(UnmanagedType.LPWStr)] string appUserModelId, [MarshalAs(UnmanagedType.LPWStr)] string invokedArgs, IntPtr data, uint count);
}
public static class GesherToastActivation {
  [DllImport("ole32.dll")] static extern int CoCreateInstance(ref Guid clsid, IntPtr outer, uint ctx, ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object o);
  // Activates the class's object (running the local server command when none serves it).
  public static string Activate(string clsid, string serverCommand, string appId, string args, int seconds) {
    Guid c = new Guid(clsid);
    Guid iid = typeof(INotificationActivationCallback).GUID;
    bool started = false;
    DateTime deadline = DateTime.UtcNow.AddSeconds(seconds);
    int hr;
    object o;
    while (true) {
      hr = CoCreateInstance(ref c, IntPtr.Zero, 4 /* CLSCTX_LOCAL_SERVER */, ref iid, out o);
      if (hr == 0) break;
      if (!started && serverCommand != null) {
        ProcessStartInfo psi = new ProcessStartInfo("cmd.exe", "/d /s /c \"" + serverCommand + "\"");
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        Process.Start(psi);
        started = true;
      }
      if (DateTime.UtcNow > deadline) return String.Format("CoCreateInstance 0x{0:x8}", hr);
      Thread.Sleep(250);
    }
    try { ((INotificationActivationCallback) o).Activate(appId, args, IntPtr.Zero, 0); }
    catch (Exception e) { return "Activate: " + e.Message; }
    finally { Marshal.ReleaseComObject(o); }
    return started ? "ok (local server started)" : "ok (served by a running process)";
  }
}
'@
}
$xmlScript = Join-Path $root 'devsvc-toast-xml.ps1'
Set-Content -Path $xmlScript -Encoding UTF8 -Value @'
param([string] $AppId, [string] $Tag)
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
foreach ($t in [Windows.UI.Notifications.ToastNotificationManager]::History.GetHistory($AppId)) { if ($t.Tag -eq $Tag) { $t.Content.GetXml() } }
'@
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'ClickActivity'
$posted = DevsvcWait $mark @('devsvc: click: posted 21') 300
$out = WaitTimed $t 120
Start-Sleep 2
$customActivator = (Get-ItemProperty "HKCU:\Software\Classes\AppUserModelId\$devsvcAppId" -ErrorAction SilentlyContinue).CustomActivator
$xml = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $xmlScript -AppId $devsvcAppId -Tag '21' 2>&1 | Out-String
$launchArgs = if ($xml -match '<toast[^>]* launch="([^"]+)"') { [Net.WebUtility]::HtmlDecode($Matches[1]) } else { '' }
$buttonArgs = if ($xml -match '<action[^>]* arguments="([^"]+)"') { [Net.WebUtility]::HtmlDecode($Matches[1]) } else { '' }
$stateBefore = DeviceState
$null = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'shutdown') 120
Start-Sleep 2
$stopped = $stateBefore -and -not (DeviceAlive $stateBefore)
$kept = (ToastTags).Contains('tag=21 group=android')
$serverCommand = (Get-Item "$activatorKey\LocalServer32").GetValue('')
$mark = (DevsvcText $devsvcLog).Length
$r1 = [GesherToastActivation]::Activate($activatorClsid, $serverCommand, $devsvcAppId, $launchArgs, 120)
$clicked = DevsvcWait 0 @('devsvc: click: receiver got com.gesher.devsvc.CLICKED from content') 300
$launched = DevsvcText $activatorLog
$server1 = DevsvcText $serverLog
$r2 = [GesherToastActivation]::Activate($activatorClsid, $null, $devsvcAppId, $buttonArgs, 30)
$acted = DevsvcWait 0 @('devsvc: click: receiver got com.gesher.devsvc.ACTED from button') 120
$log = DevsvcText $devsvcLog
$server2 = DevsvcText $serverLog
$problems = @()
if (-not $posted) { $problems += 'notification 21 was not posted' }
if ($customActivator -ne $activatorClsid) { $problems += "the app id's CustomActivator is '$customActivator'" }
if (-not $launchArgs -or -not $buttonArgs) { $problems += "the toast's XML has no launch or action arguments: $xml" }
if (-not $stopped) { $problems += 'the device did not stop' }
if (-not $kept) { $problems += 'the toast left the history when the device stopped' }
if (-not $r1.StartsWith('ok (local server started)')) { $problems += "activation with the device stopped: $r1" }
if (-not $launched.Contains('toast activator: the click went to the Gesher device')) { $problems += 'the launcher COM ran did not hand the click to the device' }
if (-not $server1.Contains("notification 21 of $devsvcPkg (an earlier run's): its toast was clicked")) { $problems += 'the restarted device did not take the stored notification' }
if (-not $clicked) { $problems += 'the content intent''s receiver did not run' }
if (-not $r2.StartsWith('ok (served by a running process)')) { $problems += "activation with the device running: $r2" }
if (-not $server2.Contains('through the toast activator')) { $problems += 'the running system server did not serve the activator' }
if (-not $acted) { $problems += 'the action button''s receiver did not run' }
DevsvcCheck "devsvc: notifications: with the Gesher device stopped, activating a toast through Gesher's toast activator ($activatorClsid, the app id's CustomActivator; its LocalServer32 run) starts the device and sends the content intent; with the device running its system server serves the activator and a button sends its action's intent" $problems ("activation 1: $r1; activation 2: $r2; launch '$launchArgs'; button '$buttonArgs'`n--- activator log`n" + $launched + "`n--- package log`n" + $log.Substring([Math]::Min($mark, $log.Length)) + "`n--- system server`n" + (($server2 -split "`n") | Select-String 'notification|toast' | Out-String))

# --- hwsvc2: uninstall removes the app id and its toasts ------------------------------
$uninstallOut = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'uninstall', $devsvcPkg) 300
$code = $LASTEXITCODE
Start-Sleep 2
$problems = @()
if ($code -ne 0) { $problems += "uninstall exit $code" }
if (Test-Path "HKCU:\Software\Classes\AppUserModelId\$devsvcAppId") { $problems += "HKCU\Software\Classes\AppUserModelId\$devsvcAppId is still there" }
if ((ToastTags).Contains('tag=')) { $problems += "the toast history still has: $((ToastTags).Trim())" }
if (Test-Path (Join-Path $root "data/system/notifications/$devsvcPkg")) { $problems += 'the stored notifications are still there' }
if (-not (DevsvcText $serverLog).Contains("notification: $devsvcPkg removed")) { $problems += 'the system server did not remove the package''s notifications' }
DevsvcCheck "devsvc: notifications: uninstalling the package removes its toasts, its stored notifications and HKCU\Software\Classes\AppUserModelId\$devsvcAppId" $problems ($uninstallOut + "`n--- system server`n" + ((DevsvcText $serverLog) -split "`n" | Select-String 'notification' | Out-String))

# The device stops; the app's toast registration goes.
$null = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'shutdown') 120
Remove-Item "HKCU:\Software\Classes\AppUserModelId\$devsvcAppId" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $activatorKey -Recurse -Force -ErrorAction SilentlyContinue
if ($failed -ne $failedBefore) { DeviceLogs }
Remove-Item Env:GESHER_APP_WINDOW -ErrorAction SilentlyContinue
