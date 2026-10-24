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
  if (Test-Path $Path) { return [IO.File]::ReadAllText($Path) }
  return ''
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
$receiverPid = if ($log -match 'AlarmReceiver onReceive com\.gesher\.devsvc\.ALARM \(alarm count 1\) \(pid (\d+)\)') { $Matches[1] } else { '' }
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
# WM_TIMECHANGE (what Windows broadcasts when the time is set; sent here
# without changing the clock) becomes TIME_SET for registered and manifest
# receivers.
if (-not ('Devsvc.Broadcast' -as [type])) {
  Add-Type -Namespace Devsvc -Name Broadcast -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr SendMessageTimeoutW(System.IntPtr hWnd, uint msg, System.IntPtr wParam, System.IntPtr lParam, uint flags, uint timeout, out System.IntPtr result);
'@
}
$mark = (DevsvcText $devsvcLog).Length
$t = DevsvcLaunch 'TimeActivity'
$registered = DevsvcWait $mark @('devsvc: time: receiver registered') 300
Start-Sleep 1
$r = [IntPtr]::Zero
$null = [Devsvc.Broadcast]::SendMessageTimeoutW([IntPtr]0xffff, 0x001E, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref] $r)
$out = WaitTimed $t 200
$code = $LASTEXITCODE
$got = DevsvcWait $mark @('devsvc: time: manifest receiver got android.intent.action.TIME_SET') 60
$log = DevsvcSince $mark
$problems = @()
if (-not $registered) { $problems += 'the receiver was not registered' }
if (-not $log.Contains('devsvc: time: registered receiver got android.intent.action.TIME_SET')) { $problems += 'the registered receiver got no TIME_SET' }
if (-not $got) { $problems += 'the manifest receiver got no TIME_SET' }
if ($code -ne 0) { $problems += "launch exit $code" }
DevsvcCheck 'devsvc: clock: WM_TIMECHANGE becomes TIME_SET for registered and manifest receivers' $problems ($out + "`n--- package log`n" + $log)

# The device stops; the app's toast registration goes.
$null = RunTimed "$Dir/dalvikvm.exe" ($launcher + 'shutdown') 120
Remove-Item "HKCU:\Software\Classes\AppUserModelId\$devsvcAppId" -Recurse -Force -ErrorAction SilentlyContinue
if ($failed -ne $failedBefore) { DeviceLogs }
Remove-Item Env:GESHER_APP_WINDOW -ErrorAction SilentlyContinue
