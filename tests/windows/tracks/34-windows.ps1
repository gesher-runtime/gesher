# An app's UI in a Win32 window (docs/graphics.md, "An app's window"; ADR
# 0029). Dot-sourced by run-tests.ps1 after 32-fonts.ps1, which installs the
# system fonts the app's TextView draws with.
#
# uiapp.apk (tests/apps/uiapp) runs with a hidden window: ViewRootImpl lays its
# decor view out through com.gesher.app.window.WindowSession and hwui's CPU
# pipeline draws it into the window (each frame queued: frameworks/base patch
# 0103). Once the first frame is drawn the window is read back with
# PrintWindow (a hidden window: no desktop needed) and its layout's colors and
# text are checked; then WM_CLOSE finishes the Activity and the process ends.
# Twice: gesher-app --hidden-window in a single process, and on the Gesher
# device (gesher-app's default), where the app process the system server
# starts creates and draws its own window.
. "$PSScriptRoot/../graphics/WindowChecks.ps1"

$bcpWindows = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmWindows = @("-Xbootclasspath:$bcpWindows", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# Runs dalvikvm with $Arguments (after the runtime options), its output in a
# log, until a hidden window's first frame is drawn ("first frame drawn ...
# hwnd 0x..."); reads the window back and checks it with $Verify (given the
# pixels, the HWND and the output so far; it returns a list of problems),
# closes it with WM_CLOSE and expects the run to end with exit code 0, the
# window gone and every line of $Expect in the output.
function WindowAppCheck([string] $Name, [string] $Apk, [string[]] $Arguments, [scriptblock] $Verify,
                        [string[]] $Expect, [int] $Seconds = 400) {
  if (-not (Test-Path "$Dir/$Apk")) {
    Write-Host "FAIL  gesher-app: $Name ($Apk is missing)"
    $script:failed++
    return
  }
  $log = Join-Path $env:RUNNER_TEMP "window-app-$([IO.Path]::GetFileNameWithoutExtension($Apk))-$(Get-Random).log"
  # Quoted only where needed: the launcher finds its runtime options in its command line
  # (com.gesher.server.Zygote.vmCommandLine), as ProcessStartInfo.ArgumentList writes it.
  $quoted = (@($dvmWindows) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.Environment['GESHER_WINDOW_DEBUG'] = '1'  # Win32Surface reports its first frames
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
  $hwnd = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($Seconds)
  $text = ''
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $text = if (Test-Path $log) { Get-Content -Raw $log } else { '' }
    $m = [regex]::Match("$text", 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)')
    if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16); break }
    Start-Sleep -Milliseconds 250
  }
  if ($hwnd -eq [IntPtr]::Zero) {
    $problems += 'no first frame'
  } else {
    # Let the frames after the first one land.
    Start-Sleep -Seconds 3
    $px = [GesherWin32]::Capture($hwnd)
    if ($null -eq $px) {
      $problems += 'PrintWindow failed'
    } else {
      Write-Host "INFO  $Apk's window: $($px[0])x$($px[1]), visible: $([GesherWin32]::IsWindowVisible($hwnd)), title '$([GesherWin32]::Title($hwnd))', process $([GesherWin32]::ProcessId($hwnd)), AppUserModelID '$([GesherWin32]::AppUserModelId($hwnd))', icon: $([GesherWin32]::HasIcon($hwnd))"
      $text = Get-Content -Raw $log
      $problems += & $Verify $px $hwnd "$text"
    }
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit after WM_CLOSE' }
  elseif ($p.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $p.ExitCode) }
  $text = if (Test-Path $log) { Get-Content -Raw $log } else { '' }
  foreach ($want in $Expect) {
    if (-not "$text".Contains($want)) { $problems += "missing '$want'" }
  }
  if ($hwnd -ne [IntPtr]::Zero -and [GesherWin32]::IsWindow($hwnd)) { $problems += 'the window is still there' }
  if (-not $problems) {
    Write-Host "PASS  gesher-app: $Name"
    ("$text" -split "`n" | Select-String -Pattern '(gesher-app: window|gesher-app: windows|UiActivity laid out|Win32Surface: )') | Out-Host
  } else {
    Write-Host "FAIL  gesher-app: $Name ($($problems -join '; '))"
    if (Test-Path "$Dir/dalvikvm.map") {
      "$text" | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $text
    }
    $script:failed++
  }
}

# 1. A single process (gesher-app --single-process --hidden-window): the
#    window session, the window and the app in gesher-app's process.
WindowAppCheck 'uiapp.apk drawn in a Win32 window by hwui (layout colors and text read back with PrintWindow), WM_CLOSE finishes it (single process)' `
  uiapp.apk @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/uiapp.apk")) `
  { param($px, $hwnd, $text) UiAppPixels $px } `
  @('UiActivity onResume', 'window "Gesher UI test" added', 'hardware accelerated true', 'closed by the user',
    'UiActivity onDestroy', 'gesher-app: done')

# 2. The Gesher device (gesher-app's default; ADR 0026), a device of its own
#    (its own $ANDROID_DATA and logs: 50-services.ps1 checks the shared one):
#    the launcher installs uiapp.apk and asks the system server to start it
#    with a hidden window; the system server starts its process
#    (GesherAppProcess), whose window session (WindowSession, ADR 0029) gives
#    the window a Win32 window that process creates, titled with the
#    Activity's label and under the package's AppUserModelID. WM_CLOSE
#    finishes the Activity; the system server destroys it and ends the
#    process; the launcher ends with it.
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-windows"
$env:GESHER_LOGS = NoDrive "$root/logs-windows"
New-Item -ItemType Directory -Force "$root/data-windows" | Out-Null
$env:GESHER_SHORTCUTS = '0'
try {
  WindowAppCheck 'the Gesher device: uiapp.apk''s process (started by the system server) draws it in a Win32 window of its own, titled with the Activity''s label, under the package''s AppUserModelID; WM_CLOSE finishes it, the process ends' `
    uiapp.apk @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/uiapp.apk"), '--hidden-window') `
    { param($px, $hwnd, $text)
      $out = @(UiAppPixels $px)
      $m = [regex]::Match($text, 'app process for com\.gesher\.uiapp: pid (\d+)')
      $owner = [GesherWin32]::ProcessId($hwnd)
      if (-not $m.Success) { $out += 'no app process pid' }
      elseif ([int]$m.Groups[1].Value -ne $owner) { $out += "the window is process $owner's, not the app process $($m.Groups[1].Value)'s" }
      $title = [GesherWin32]::Title($hwnd)
      if ($title -ne 'Gesher UI test') { $out += "window title '$title'" }
      $id = [GesherWin32]::AppUserModelId($hwnd)
      if ($id -ne 'Gesher.App.com.gesher.uiapp') { $out += "window AppUserModelID '$id'" }
      return $out } `
    @('gesher-app: windows of com.gesher.uiapp: hidden', 'gesher-app: app process for com.gesher.uiapp: pid ',
      'gesher-app: windows: Win32 windows of this process (hidden)', 'UiActivity onResume',
      'window "Gesher UI test" added', 'hardware accelerated true', 'closed by the user', 'UiActivity onDestroy',
      'gesher-app: the system server ended the process (scheduleExit)',
      'gesher-app: the process of com.gesher.uiapp ended (exit code 0)', 'gesher-app: done')
} finally {
  # The device stops (with a time limit: never wait on a process that may not end).
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'window-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmWindows) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the device shutdown timed out' }
  $out = (Get-Content -Raw $shutdownLog -ErrorAction SilentlyContinue) + (Get-Content -Raw "$shutdownLog.err" -ErrorAction SilentlyContinue)
  if ("$out" -notmatch 'stopped|no Gesher device') { Write-Host $out }
  if ($failed) {
    foreach ($f in Get-ChildItem "$root/logs-windows" -Filter *.log -ErrorAction SilentlyContinue) {
      Write-Host "--- $($f.Name)"
      Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -Last 60 | Out-Host
    }
  }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
