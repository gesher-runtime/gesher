# The locale track (docs/locale.md, docs/decisions/0049-locale-per-app-and-formats.md):
# what an app sees of the locale, per-app locales (LocaleManager), and Windows'
# language and format settings changing while apps run. Dot-sourced by
# run-tests.ps1 after 34-windows.ps1, whose WindowAppCheck and $dvmWindows it
# uses. tests/apps/localeapp (localeapp.apk) reports on stdout ("localeapp: ").
#
# 1. A single process (gesher-app's own services):
#    a. the locales, resources, layout direction, numbers, dates, times, first day
#       and android:localeConfig the app sees, against Windows' settings;
#    b. LocaleManager.setApplicationLocales(he-IL): the process's configuration
#       changes at once (Application.onConfigurationChanged); the next run of the
#       app (an app restart) starts with he-IL (Hebrew resources, right to left);
#       cleared, the system's again;
#    c. Windows' regional format changed while the app runs (Set-Culture de-DE,
#       WM_SETTINGCHANGE "intl" sent to the process's events window): an activity
#       that handles locale changes gets onConfigurationChanged with the new
#       regional preferences; restored, again.
# 2. The Gesher device (a device of its own, headless):
#    a. as 1a, in an app process the system server started;
#    b. setApplicationLocales(he-IL) relaunches the activity with he-IL
#       (ActivityRelaunchItem; its saved state kept), sends LOCALE_CHANGED to the
#       app's registered and manifest receivers; the app's next process, and the
#       next device (shutdown and start), still have he-IL; cleared;
#    c. Windows' regional format changed (de-DE) and restored while the app runs:
#       the system server's events window gets WM_SETTINGCHANGE; the process gets
#       the new configuration (Application.onConfigurationChanged), LOCALE_CHANGED,
#       CONFIGURATION_CHANGED and TIME_SET with the 24-hour extra; the activity in
#       front (configChanges="locale") gets onConfigurationChanged, the stopped one
#       under it (no configChanges) is relaunched as it comes back, and again when
#       the format is restored.
# The user's regional format is restored after each change.

$localeApk = Join-Path $Dir 'localeapp.apk'
if (-not (Test-Path $localeApk) -or -not $dvmWindows) {
  Write-Host "FAIL  locale: localeapp.apk or 34-windows.ps1's runtime options are missing"
  $failed++
  return
}
$failedBeforeLocale = $failed
$localePkg = 'com.gesher.localeapp'

if (-not ('GesherLocale.Native' -as [type])) {
  Add-Type -Namespace GesherLocale -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr SendMessageTimeoutW(System.IntPtr hWnd, uint msg, System.IntPtr wParam, string lParam, uint flags, uint timeout, out System.IntPtr result);
'@
}

# What Windows says, in a new process: the display languages, the regional format,
# its first day (java.util.Calendar's numbers), 24-hour time, 1234567.5 in it.
function LocaleWindowsNow {
  $script = Join-Path $env:RUNNER_TEMP 'locale-windows-now.ps1'
  Set-Content $script @'
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class LocaleMui {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
  static extern bool GetUserPreferredUILanguages(uint flags, out uint count, char[] buf, ref uint size);
  public static string Languages() {
    uint count, size = 0;
    GetUserPreferredUILanguages(8, out count, null, ref size);
    var buf = new char[size];
    GetUserPreferredUILanguages(8, out count, buf, ref size);
    return string.Join(",", new string(buf).Split(new[] { '\0' }, StringSplitOptions.RemoveEmptyEntries));
  }
}
"@
$c = [Globalization.CultureInfo]::new((Get-Culture).Name, $true)
[ordered]@{ ui = [LocaleMui]::Languages(); format = $c.Name; firstDay = [int]$c.DateTimeFormat.FirstDayOfWeek + 1;
  is24 = $c.DateTimeFormat.ShortTimePattern.Contains('H'); number = (1234567.5).ToString('#,##0.###', $c) } | ConvertTo-Json -Compress
'@
  return (& pwsh -NoProfile -File $script) | ConvertFrom-Json
}

function LocaleSetFormat([string] $Name) {
  & powershell.exe -NoProfile -Command "Set-Culture $Name" 2>&1 | Out-Null
}

# WM_SETTINGCHANGE ("intl", what Windows broadcasts when the regional settings
# change) to an events window, by handle. True when it was delivered.
function LocaleSettingChange([IntPtr] $Window) {
  if ($Window -eq [IntPtr]::Zero) { return $false }
  $r = [IntPtr]::Zero
  $sent = [GesherLocale.Native]::SendMessageTimeoutW($Window, 0x001A, [IntPtr]::Zero, 'intl', 2, 5000, [ref] $r)
  return $sent -ne [IntPtr]::Zero
}

function LocaleEventsWindow([string] $Text) {
  $m = [regex]::Matches($Text, 'devsvc: Windows events from a hidden window 0x([0-9a-fA-F]+)')
  if ($m.Count -eq 0) { return [IntPtr]::Zero }
  return [IntPtr]::new([Convert]::ToInt64($m[$m.Count - 1].Groups[1].Value, 16))
}

# A file the device's processes keep open, read with sharing.
function LocaleText([string] $Path) {
  if (-not (Test-Path $Path)) { return '' }
  $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
  try { return [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8).ReadToEnd() } finally { $stream.Dispose() }
}

# Runs dalvikvm (34-windows.ps1's runtime options, then $Arguments) to its end or
# $Seconds; its output. $LASTEXITCODE: its exit code (-1: timed out).
function LocaleRun([string[]] $Arguments, [int] $Seconds = 300) {
  $t = LocaleStart $Arguments
  return LocaleWait $t $Seconds
}

function LocaleStart([string[]] $Arguments) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @($dvmWindows) + $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $p = [Diagnostics.Process]::Start($psi)
  return @{ Process = $p; Out = $p.StandardOutput.ReadToEndAsync(); Err = $p.StandardError.ReadToEndAsync() }
}

function LocaleWait($t, [int] $Seconds) {
  try {
  $timedOut = -not $t.Process.WaitForExit($Seconds * 1000)
  $out = ''
  if ($timedOut) {
    $out += "`n(timed out after $Seconds s)"
    try { $t.Process.Kill($true) } catch { $out += "`n(process kill failed: $_)" }
    if (-not $t.Process.WaitForExit(5000)) { $out += "`n(process still alive five seconds after kill)" }
  }
  foreach ($pipe in @(@{ Task = $t.Out; Name = 'stdout' }, @{ Task = $t.Err; Name = 'stderr' })) {
    try {
      if ($pipe.Task.Wait(10000)) { $out += $pipe.Task.Result }
      else { $timedOut = $true; $out += "`n($($pipe.Name) did not complete within ten seconds)" }
    } catch { $timedOut = $true; $out += "`n($($pipe.Name) read failed: $_)" }
  }
  $global:LASTEXITCODE = if ($timedOut) { -1 } else { $t.Process.ExitCode }
  return $out
  } finally { $t.Process.Dispose() }
}

function LocaleCheck([string] $Name, [string[]] $Problems, [string] $Details) {
  if (-not $Problems) {
    Write-Host "PASS  locale: $Name"
  } else {
    Write-Host "FAIL  locale: $Name ($($Problems -join '; '))"
    Write-Host $Details
    $script:failed++
  }
}

# The report line of $Prefix ("MainActivity state "), the last one; '' if none.
function LocaleLine([string] $Text, [string] $Prefix) {
  $lines = @("$Text" -split "`r?`n" | Where-Object { $_.Contains("localeapp: $Prefix") })
  if ($lines.Count -eq 0) { return '' }
  return $lines[$lines.Count - 1]
}

# A report's field ("locales", "app", "greeting"...).
function LocaleField([string] $Line, [string] $Field) {
  if ($Line -match "(^|\s)$Field=(\[[^\]]*\]|\S+)") { return $Matches[2] }
  return $null
}

# Problems of a "state" report against what is expected of it.
function LocaleStateProblems([string] $Line, [hashtable] $Want) {
  if (-not $Line) { return @('no report') }
  $problems = @()
  foreach ($k in $Want.Keys) {
    $v = LocaleField $Line $k
    $w = $Want[$k]
    if ($w -is [scriptblock]) {
      if (-not (& $w $v)) { $problems += "$k=$v" }
    } elseif ($v -ne $w) {
      $problems += "$k=$v (expected $w)"
    }
  }
  return $problems
}

$win = LocaleWindowsNow
Write-Host "INFO  locale: Windows' display languages $($win.ui), regional format $($win.format), first day $($win.firstDay), 24h $($win.is24), 1234567.5 = $($win.number)"
$firstUi = (($win.ui -split ',')[0] -split '-')[0]
$savedLocales = $env:GESHER_LOCALES
Remove-Item Env:GESHER_LOCALES -ErrorAction SilentlyContinue
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$canChangeFormat = ($win.ui -split ',')[0] -eq 'en-US' -and $win.format -ne 'de-DE'
$systemWant = @{
  'locales'      = { param($v) $v -and $v.StartsWith($firstUi) }
  'app'          = '[]'
  'greeting'     = 'Hello'
  'dir'          = 'ltr'
  'number'       = $win.number
  '24h'          = "$($win.is24)".ToLower()
  'firstDay'     = "$($win.firstDay)"
  'localeConfig' = 'en-US,he-IL,fr-FR'
}
$heWant = @{
  'locales'  = { param($v) $v -and $v.StartsWith('he-IL,') }
  'config'   = { param($v) $v -and $v.StartsWith('he-IL,') }
  'app'      = '[he-IL]'
  'greeting' = 'Shalom'
  'dir'      = 'rtl'
}

# --- 1. A single process ----------------------------------------------------------------
$env:ANDROID_DATA = NoDrive "$root/data-locale-single"
New-Item -ItemType Directory -Force "$root/data-locale-single" | Out-Null
try {
  $single = @('com.gesher.app.Main', '--activity')
  $out = LocaleRun ($single + '.MainActivity', (NoDrive $localeApk))
  $code = $LASTEXITCODE
  $line = LocaleLine $out 'MainActivity state '
  $problems = @(LocaleStateProblems $line $systemWant)
  if ((LocaleField $line 'system') -ne (LocaleField $line 'locales')) { $problems += 'LocaleManager.getSystemLocales is not the default list' }
  if ($code -ne 0) { $problems += "exit $code" }
  LocaleCheck "the app sees Windows' locales ($(LocaleField $line 'locales')), its resources, numbers ($(LocaleField $line 'number')), dates ($(LocaleField $line 'date')), times ($(LocaleField $line 'time'), 24-hour $(LocaleField $line '24h')), first day and android:localeConfig (single process)" $problems $out

  $out = LocaleRun ($single + '.SetLocalesActivity', (NoDrive $localeApk))
  $code = $LASTEXITCODE
  $problems = @()
  foreach ($want in @('SetLocalesActivity setApplicationLocales(he-IL): getApplicationLocales he-IL',
      'locale: configuration change in this process (app locales he-IL): he-IL,',
      'localeapp: LocaleApp onConfigurationChanged he-IL,',
      'SetLocalesActivity is not recreated in a single process')) {
    if (-not $out.Contains($want)) { $problems += "missing '$want'" }
  }
  if ($code -ne 0) { $problems += "exit $code" }
  LocaleCheck 'LocaleManager.setApplicationLocales(he-IL) changes the process''s configuration at once (Application.onConfigurationChanged; the activity is not relaunched in a single process)' $problems $out

  $out = LocaleRun ($single + '.MainActivity', (NoDrive $localeApk))
  $code = $LASTEXITCODE
  $line = LocaleLine $out 'MainActivity state '
  $problems = @(LocaleStateProblems $line $heWant)
  if ($code -ne 0) { $problems += "exit $code" }
  LocaleCheck 'the app locales are kept across an app restart: he-IL first, Hebrew resources, right to left (single process)' $problems $out

  $out = LocaleRun ($single + '.ClearLocalesActivity', (NoDrive $localeApk))
  $out2 = LocaleRun ($single + '.MainActivity', (NoDrive $localeApk))
  $line = LocaleLine $out2 'MainActivity state '
  $problems = @(LocaleStateProblems $line @{ 'app' = '[]'; 'greeting' = 'Hello'; 'dir' = 'ltr'; 'locales' = $systemWant['locales'] })
  LocaleCheck 'cleared (an empty list), the app has the system''s locales again (single process)' $problems ($out + $out2)

  # c. Windows' regional format changing while the app runs, in its window.
  if (-not $canChangeFormat) {
    Write-Host "INFO  locale: not an en-US display ($($win.ui), $($win.format)); the format change checks are skipped"
  } else {
    $script:localeSingleProblems = @()
    try {
      WindowAppCheck 'locale: Windows'' regional format changed to de-DE and back while localeapp.apk runs: its activity that handles locale changes gets onConfigurationChanged with the new regional preferences (Monday, 24-hour), the Application too (single process)' `
        localeapp.apk @('com.gesher.app.Main', '--hidden-window', '--activity', '.HandlesActivity', (NoDrive $localeApk)) `
        { param($px, $hwnd, $text, $readLog)
          $problems = @()
          $events = LocaleEventsWindow "$text"
          if ($events -eq [IntPtr]::Zero) { return @('no events window in the log') }
          LocaleSetFormat 'de-DE'
          if (-not (LocaleSettingChange $events)) { $problems += 'WM_SETTINGCHANGE not delivered' }
          $deadline = (Get-Date).AddSeconds(60)
          while ((Get-Date) -lt $deadline -and -not "$(& $readLog)".Contains('HandlesActivity onConfigurationChanged #1')) { Start-Sleep -Milliseconds 300 }
          $t = "$(& $readLog)"
          $line = LocaleLine $t 'HandlesActivity onConfigurationChanged #1 '
          $problems += @(LocaleStateProblems $line @{ 'locales' = { param($v) $v -and $v.Contains('-u-') -and $v.Contains('fw-mon') }; 'firstDay' = '2'; '24h' = 'true' } | ForEach-Object { "change: $_" })
          LocaleSetFormat $win.format
          $restoreSent = LocaleSettingChange $events
          $deadline = (Get-Date).AddSeconds(60)
          while ((Get-Date) -lt $deadline -and -not "$(& $readLog)".Contains('HandlesActivity onConfigurationChanged #2')) { Start-Sleep -Milliseconds 300 }
          $line = LocaleLine "$(& $readLog)" 'HandlesActivity onConfigurationChanged #2 '
          $problems += @(LocaleStateProblems $line @{ 'locales' = { param($v) $v -and -not $v.Contains('fw-mon') }; 'firstDay' = "$($win.firstDay)"; '24h' = "$($win.is24)".ToLower() } | ForEach-Object { "restore: $_" })
          if (-not $restoreSent) {
            # Set-Culture can broadcast the change before our explicit send.
            # HandlesActivity finishes after #2, destroying the events window.
            if ($line) {
              Write-Host 'INFO  locale: restore callback already received; the explicit WM_SETTINGCHANGE send raced activity shutdown'
            } else {
              $problems += 'WM_SETTINGCHANGE (restore) not delivered'
            }
          }
          ("$(& $readLog)" -split "`n" | Select-String -Pattern 'locale: |onConfigurationChanged') | Out-Host
          return $problems } `
        @('HandlesActivity onCreate', 'locale: Windows'' settings changed: ', 'LocaleApp onConfigurationChanged ',
          'HandlesActivity onConfigurationChanged (it handles the change)', 'HandlesActivity onConfigurationChanged #2', 'gesher-app: done')
    } finally {
      LocaleSetFormat $win.format
    }
  }
} finally {
  $env:ANDROID_DATA = $savedData
}

# --- 2. The Gesher device -------------------------------------------------------------
$env:ANDROID_DATA = NoDrive "$root/data-locale"
$env:GESHER_LOGS = NoDrive "$root/logs-locale"
New-Item -ItemType Directory -Force "$root/data-locale" | Out-Null
$env:GESHER_SHORTCUTS = '0'
$env:GESHER_APP_WINDOW = 'headless'
$launcherArgs = @('com.gesher.app.Launcher')
$packageLog = Join-Path "$root/logs-locale" "$localePkg.log"
$serverLog = Join-Path "$root/logs-locale" 'system_server.log'
function LocaleLaunch([string] $Activity, [int] $Seconds = 400) {
  return LocaleRun ($launcherArgs + 'launch', (NoDrive $localeApk), '--activity', $Activity, '--keep-task', '--headless') $Seconds
}
function LocaleServerSince([int] $Mark) {
  $t = LocaleText $serverLog
  if ($t.Length -le $Mark) { return '' }
  return $t.Substring($Mark)
}
function LocalePackageSince([int] $Mark) {
  $t = LocaleText $packageLog
  if ($t.Length -le $Mark) { return '' }
  return $t.Substring($Mark)
}
function LocalePackageWait([int] $Mark, [string] $Text, [int] $Seconds) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline) {
    if ((LocalePackageSince $Mark).Contains($Text)) { return $true }
    Start-Sleep -Milliseconds 300
  }
  return $false
}
try {
  $start = LocaleRun ($launcherArgs + 'start') 400
  $out = LocaleLaunch '.MainActivity'
  $code = $LASTEXITCODE
  $line = LocaleLine $out 'MainActivity state '
  $problems = @(LocaleStateProblems $line $systemWant)
  $server = LocaleText $serverLog
  if (-not $server.Contains('locale: the device''s locales ')) { $problems += 'the system server did not log its locales' }
  if ($code -ne 0) { $problems += "exit $code" }
  ($server -split "`n" | Select-String -Pattern 'locale: ') | Out-Host
  LocaleCheck "the app sees the device's locales ($(LocaleField $line 'locales')) and formats in an app process the system server started (number $(LocaleField $line 'number'), date $(LocaleField $line 'date'), time $(LocaleField $line 'time'), 24-hour $(LocaleField $line '24h'), localeConfig $(LocaleField $line 'localeConfig'))" $problems ($start + $out)

  # b. Per-app locales.
  $serverMark = (LocaleText $serverLog).Length
  $out = LocaleLaunch '.SetLocalesActivity'
  $code = $LASTEXITCODE
  $server = LocaleServerSince $serverMark
  $line = LocaleLine $out 'SetLocalesActivity recreated (he-IL) '
  $problems = @(LocaleStateProblems $line $heWant | ForEach-Object { "recreated: $_" })
  foreach ($want in @('SetLocalesActivity setApplicationLocales(he-IL): getApplicationLocales he-IL',
      'SetLocalesActivity onDestroy (changing configurations true)',
      'LocaleApp onConfigurationChanged he-IL,',
      'LocaleApp registered receiver got android.intent.action.LOCALE_CHANGED',
      'LocaleReceiver (manifest) got android.intent.action.LOCALE_CHANGED')) {
    if (-not $out.Contains($want)) { $problems += "missing '$want'" }
  }
  if ($out.Contains('SetLocalesActivity was not recreated')) { $problems += 'not recreated' }
  if (-not $server.Contains("locale: $localePkg's app locales set to he-IL")) { $problems += 'the system server did not set them' }
  if (-not $server.Contains('ActivityRelaunchItem')) { $problems += 'no ActivityRelaunchItem' }
  if (-not $server.Contains('ConfigurationChangeItem to ')) { $problems += 'no ConfigurationChangeItem' }
  if ($code -ne 0) { $problems += "exit $code" }
  ($server -split "`n" | Select-String -Pattern 'locale: |ConfigurationChangeItem|ActivityRelaunchItem|activityRelaunched') | Out-Host
  LocaleCheck 'LocaleManager.setApplicationLocales(he-IL) on the device: the system server relaunches the activity with he-IL (ActivityRelaunchItem, saved state kept: Hebrew resources, right to left), the process gets the configuration (Application.onConfigurationChanged), LOCALE_CHANGED reaches the app''s registered and manifest receivers' $problems ($out + "`n--- system server`n" + $server)
  $setPid = LocaleField $line 'pid'

  $out = LocaleLaunch '.MainActivity'
  $line = LocaleLine $out 'MainActivity state '
  $problems = @(LocaleStateProblems $line $heWant)
  if ((LocaleField $line 'pid') -eq $setPid) { $problems += "the same process ($setPid)" }
  LocaleCheck 'the app locales are kept across an app restart (a new app process starts with he-IL) on the device' $problems $out

  # The next device (shut down and started again) still has them.
  $stop = LocaleRun ($launcherArgs + 'shutdown') 120
  $saved = Get-Content "$root/data-locale/system/app_locales" -ErrorAction SilentlyContinue | Out-String
  $out = LocaleLaunch '.MainActivity'
  $line = LocaleLine $out 'MainActivity state '
  $problems = @(LocaleStateProblems $line $heWant)
  if (-not $saved.Contains("$localePkg=he-IL")) { $problems += "data/system/app_locales has no $localePkg=he-IL" }
  LocaleCheck 'the app locales are kept across a device restart (data/system/app_locales)' $problems ($stop + $out)

  $out = LocaleLaunch '.ClearLocalesActivity'
  $code = $LASTEXITCODE
  $out2 = LocaleLaunch '.MainActivity'
  $line = LocaleLine $out2 'MainActivity state '
  $problems = @(LocaleStateProblems $line $systemWant)
  if (-not $out.Contains('ClearLocalesActivity recreated () ')) { $problems += 'ClearLocalesActivity was not recreated' }
  if ($code -ne 0) { $problems += "exit $code" }
  LocaleCheck 'cleared, the app has the device''s locales again (relaunched, then in a new process)' $problems ($out + $out2)

  # c. Windows' regional format changing while the app runs.
  if (-not $canChangeFormat) {
    Write-Host "INFO  locale: not an en-US display ($($win.ui), $($win.format)); the device's format change checks are skipped"
  } else {
    $mark = (LocaleText $packageLog).Length
    $serverMark = (LocaleText $serverLog).Length
    $t = LocaleStart ($launcherArgs + 'launch', (NoDrive $localeApk), '--activity', '.WaitActivity', '--keep-task', '--headless')
    $problems = @()
    try {
      if (-not (LocalePackageWait $mark 'HandlesActivity onResume' 300)) { $problems += 'HandlesActivity did not start' }
      Start-Sleep 3  # WaitActivity, under it, is stopped
      $events = LocaleEventsWindow (LocaleText $serverLog)
      if ($events -eq [IntPtr]::Zero) { $problems += 'no events window in the system server''s log' }
      LocaleSetFormat 'de-DE'
      if (-not (LocaleSettingChange $events)) { $problems += 'WM_SETTINGCHANGE not delivered' }
      if (-not (LocalePackageWait $mark 'WaitActivity onResume #2' 120)) { $problems += 'WaitActivity was not relaunched (#2) after the change' }
      Start-Sleep 2
      LocaleSetFormat $win.format
      if (-not (LocaleSettingChange $events)) { $problems += 'WM_SETTINGCHANGE (restore) not delivered' }
    } finally {
      LocaleSetFormat $win.format
    }
    $out = LocaleWait $t 300
    $code = $LASTEXITCODE
    $log = LocalePackageSince $mark
    $server = LocaleServerSince $serverMark
    $is24 = "$($win.is24)".ToLower()
    $line = LocaleLine $log 'HandlesActivity onConfigurationChanged #1 '
    $problems += @(LocaleStateProblems $line @{ 'locales' = { param($v) $v -and $v.Contains('fw-mon') }; 'firstDay' = '2'; '24h' = 'true'; 'time' = '13:05' } | ForEach-Object { "HandlesActivity: $_" })
    $line = LocaleLine $log 'WaitActivity onCreate #2 '
    $problems += @(LocaleStateProblems $line @{ 'locales' = { param($v) $v -and $v.Contains('fw-mon') }; 'firstDay' = '2'; '24h' = 'true' } | ForEach-Object { "WaitActivity #2: $_" })
    $line = LocaleLine $log 'WaitActivity onCreate #3 '
    $problems += @(LocaleStateProblems $line @{ 'locales' = { param($v) $v -and -not $v.Contains('fw-mon') }; 'firstDay' = "$($win.firstDay)"; '24h' = $is24 } | ForEach-Object { "WaitActivity #3: $_" })
    $wants = @('WaitActivity onCreate #2 (saved state true)', 'WaitActivity onDestroy #1 (changing configurations true)',
      'WaitActivity onCreate #3 (saved state true)', 'LocaleApp onConfigurationChanged ',
      'LocaleApp registered receiver got android.intent.action.LOCALE_CHANGED',
      'LocaleApp registered receiver got android.intent.action.CONFIGURATION_CHANGED',
      'LocaleReceiver (manifest) got android.intent.action.LOCALE_CHANGED')
    if (-not $win.is24) { $wants += 'LocaleApp registered receiver got android.intent.action.TIME_SET 24-hour extra 1' }
    foreach ($want in $wants) { if (-not $log.Contains($want)) { $problems += "missing '$want'" } }
    if ($log.Contains('HandlesActivity onCreate (saved state true)')) { $problems += 'HandlesActivity was relaunched' }
    if ($log.Contains('(unexpected')) { $problems += 'WaitActivity got onConfigurationChanged' }
    if (-not $server.Contains('locale: Windows'' settings changed: ')) { $problems += 'the system server saw no change' }
    if ($code -ne 0) { $problems += "exit $code" }
    ($server -split "`n" | Select-String -Pattern 'locale: |ConfigurationChangeItem|ActivityRelaunchItem|settings: ') | Out-Host
    LocaleCheck 'Windows'' regional format changed to de-DE and back while the app runs on the device: WM_SETTINGCHANGE reaches the system server, the process gets the configuration (Application.onConfigurationChanged), LOCALE_CHANGED (registered and manifest receivers), CONFIGURATION_CHANGED, TIME_SET with the 24-hour extra (TIME_12_24); the activity in front that handles locale changes gets onConfigurationChanged (Monday, 24-hour, 13:05), the stopped one under it is relaunched as it comes back, and again on the restore' $problems ($out + "`n--- package log`n" + $log + "`n--- system server`n" + $server)
  }
} finally {
  $null = LocaleRun ($launcherArgs + 'shutdown') 120
  if ($failed -ne $failedBeforeLocale) {
    foreach ($f in Get-ChildItem "$root/logs-locale" -Filter *.log -ErrorAction SilentlyContinue) {
      Write-Host "--- $($f.Name)"
      LocaleText $f.FullName -split "`n" | Select-Object -Last 80 | Out-Host
    }
  }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_SHORTCUTS, Env:GESHER_APP_WINDOW -ErrorAction SilentlyContinue
  if ($null -ne $savedLocales) { $env:GESHER_LOCALES = $savedLocales }
  $back = LocaleWindowsNow
  Write-Host "INFO  locale: regional format now $($back.format)"
}
