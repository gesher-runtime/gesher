# The activity and window lifecycle Android apps observe (docs/lifecycle.md,
# ADR 0048). Dot-sourced by run-tests.ps1 after 36-popups.ps1.
#
# lifeapp.apk (tests/apps/lifeapp) reports its lifecycle callbacks; it runs with
# a hidden window, driven by Win32 messages:
#  - losing the focus to another window (WM_KILLFOCUS): onWindowFocusChanged
#    false, the Activity stays resumed; WM_SETFOCUS gives it back;
#  - a PopupWindow that is not focusable and watches outside touches: a click
#    on the Activity outside it is ACTION_OUTSIDE for it (it dismisses) and
#    reaches the Activity;
#  - minimizing the window: onPause, onStop, onSaveInstanceState (its window
#    stays, minimized: a taskbar button); restoring it: onRestart, onStart,
#    onResume, and it draws again;
#  - on the Gesher device also: an Activity with a translucent theme and one
#    with a dialog theme leave the Activity below paused, not stopped, its
#    window shown and drawn; an opaque one stops it (onSaveInstanceState) and
#    hides its window until it finishes (onRestart, onStart, onResume).
# Twice: gesher-app --single-process --hidden-window, and on the Gesher device.
. "$PSScriptRoot/../graphics/WindowChecks.ps1"

if (-not ('GesherLifeWin32' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherLifeWin32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern uint GetPixel(IntPtr dc, int x, int y);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
  // What the desktop shows at screen point (x, y), composed by DWM: 0xRRGGBB (or 0xFFFFFFFF).
  public static uint ScreenPixel(int x, int y) {
    IntPtr dc = GetDC(IntPtr.Zero);
    uint c = GetPixel(dc, x, y);
    ReleaseDC(IntPtr.Zero, dc);
    if (c == 0xFFFFFFFF) return c;
    return ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);  // COLORREF is 0xBBGGRR
  }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  // The client area on the screen: {left, top, right, bottom}.
  public static int[] ClientOnScreen(IntPtr hwnd) {
    POINT p = new POINT();
    ClientToScreen(hwnd, ref p);
    RECT r;
    GetClientRect(hwnd, out r);
    return new int[] { p.X, p.Y, p.X + r.Right - r.Left, p.Y + r.Bottom - r.Top };
  }
  public static int[] Rect(IntPtr hwnd) { RECT r; GetWindowRect(hwnd, out r); return new int[] { r.Left, r.Top, r.Right, r.Bottom }; }
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hwnd, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
}
'@
}

$bcpLife = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmLife = @("-Xbootclasspath:$bcpLife", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# "x,y" of the last match of $Pattern's groups 1 and 2 in $Text, or $null.
function LifePoint([string] $Text, [string] $Pattern) {
  $m = [regex]::Matches($Text, $Pattern)
  if ($m.Count -eq 0) { return $null }
  $g = $m[$m.Count - 1].Groups
  return @([int]$g[1].Value, [int]$g[2].Value)
}

# The log after the first $Mark characters.
function LogAfter([scriptblock] $ReadLog, [int] $Mark) {
  $t = "$(& $ReadLog)"
  if ($t.Length -le $Mark) { return '' }
  return $t.Substring($Mark)
}

# Waits until $Want is in the log after $Mark.
function WaitAfter([scriptblock] $ReadLog, [int] $Mark, [string] $Want, [int] $Seconds = 90) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    if ((LogAfter $ReadLog $Mark).Contains($Want)) { return $true }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $false
}

# The entries of $Want missing from, or out of order in, the log after $Mark.
function OrderProblems([scriptblock] $ReadLog, [int] $Mark, [string[]] $Want, [string] $What) {
  $t = LogAfter $ReadLog $Mark
  $at = 0
  $out = @()
  foreach ($e in $Want) {
    $i = $t.IndexOf($e, $at)
    if ($i -lt 0) { $out += "${What}: '$e' missing or out of order (after: $(@($Want) -join ' | '))"; break }
    $at = $i + $e.Length
  }
  return $out
}

function LogMark([scriptblock] $ReadLog) { return "$(& $ReadLog)".Length }

function LifeRedShare([IntPtr] $app, $red, [int] $r) {
  $px = [GesherWin32]::Capture($app)
  if ($null -eq $px) { return -1 }
  $n = [GesherWin32]::Count($px, $red[0], $red[1], $red[2], $red[3], ($r -shl 16), 40)
  return $n / [Math]::Max(1, ($red[2] - $red[0]) * ($red[3] - $red[1]))
}

function LifeWaitRed([IntPtr] $app, $red, [int] $Seconds = 30, [switch] $OrDimmed) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $share = LifeRedShare $app $red 255
    # Under a dialog-themed Activity the window is dimmed (backgroundDimAmount 0.6: 0x660000).
    if ($OrDimmed) { $share = [Math]::Max($share, (LifeRedShare $app $red 102)) }
    if ($share -ge 0.9) { return $share }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  return $share
}

function LifeWaitGone([IntPtr] $hwnd, [int] $Seconds = 60) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline) {
    if (-not [GesherWin32]::IsWindow($hwnd) -or -not [GesherWin32]::IsWindowVisible($hwnd)) { return $true }
    Start-Sleep -Milliseconds 250
  }
  return $false
}

# The hwnd of the window titled $Title once it is shown (a popup, or a first frame).
function LifeWindow([scriptblock] $ReadLog, [int] $Mark, [string] $Title, [int] $Seconds = 90) {
  $t = [regex]::Escape($Title)
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $text = LogAfter $ReadLog $Mark
    $m = [regex]::Match($text, "window `"$t`" shown as a popup of `"[^`"]*`" at \S+, hwnd 0x([0-9a-f]+)")
    if (-not $m.Success) { $m = [regex]::Match($text, "window `"$t`" \d+x\d+: first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)") }
    if ($m.Success) { return [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16) }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return [IntPtr]::Zero
}

# An Activity started over LifeActivity that leaves it paused and visible
# (translucent or floating), then closed (WM_CLOSE finishes it).
function OverActivity([IntPtr] $app, [scriptblock] $ReadLog, $at, $red, [string] $Name, [string] $Title) {
  $out = @()
  $mark = LogMark $ReadLog
  [GesherWin32]::Click($app, $at[0], $at[1])
  if (-not (WaitAfter $ReadLog $mark "$Name onResume")) { return @("no $Name") }
  $hwnd = LifeWindow $ReadLog $mark $Title
  Start-Sleep 3
  $out += @(OrderProblems $ReadLog $mark @('LifeActivity onPause', "$Name onCreate", "$Name onResume") "$Name over LifeActivity")
  $after = LogAfter $ReadLog $mark
  if ($after.Contains('LifeActivity onStop')) { $out += "LifeActivity stopped under $Name (it should stay paused, visible)" }
  if (-not [GesherWin32]::IsWindowVisible($app)) { $out += "LifeActivity's window was hidden under $Name" }
  $share = LifeWaitRed $app $red 10 -OrDimmed
  Write-Host "INFO  under ${Name}: LifeActivity's window shows $([int]($share * 100))% of its red View (or dimmed)"
  if ($share -lt 0.9) { $out += "LifeActivity's window under ${Name}: $([int]($share * 100))% red" }
  if ($hwnd -eq [IntPtr]::Zero) { return $out + "no window for $Name" }
  $mark = LogMark $ReadLog
  [void][GesherWin32]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity onResume')) { return $out + "LifeActivity was not resumed after $Name finished" }
  [void](WaitAfter $ReadLog $mark "$Name onDestroy" 30)
  $out += @(OrderProblems $ReadLog $mark @("$Name onPause", 'LifeActivity onResume') "$Name finishing")
  $after = LogAfter $ReadLog $mark
  if ($after.Contains('LifeActivity onRestart')) { $out += "LifeActivity was restarted after $Name (it was not stopped)" }
  if (-not $after.Contains("$Name onDestroy")) { $out += "$Name was not destroyed" }
  return $out
}

# lifeapp.apk's window $app; returns the problems.
function LifeAppScenario([IntPtr] $app, [scriptblock] $ReadLog, [switch] $Device) {
  $out = @()
  if (-not (WaitForLog $ReadLog 'LifeActivity laid out')) { return @('no layout report') }
  Start-Sleep 2
  $text = "$(& $ReadLog)"
  $m = [regex]::Matches($text, 'LifeActivity laid out: red (\d+),(\d+),(\d+),(\d+)') | Select-Object -Last 1
  $red = @(([int]$m.Groups[1].Value + 4), ([int]$m.Groups[2].Value + 4), ([int]$m.Groups[3].Value - 4), ([int]$m.Groups[4].Value - 4))
  $redAt = @([int](($red[0] + $red[2]) / 2), [int](($red[1] + $red[3]) / 2))
  $popupAt = LifePoint $text 'popup at (\d+),(\d+)'
  $share = LifeWaitRed $app $red
  Write-Host "INFO  LifeActivity's red View: $([int]($share * 100))%"

  # 1. Another window takes the focus: onWindowFocusChanged(false), still resumed.
  $mark = LogMark $ReadLog
  [void][GesherLifeWin32]::PostMessageW($app, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_KILLFOCUS
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity onWindowFocusChanged false (resumed)' 30)) { $out += 'no onWindowFocusChanged(false), resumed, after WM_KILLFOCUS' }
  Start-Sleep 2
  if ((LogAfter $ReadLog $mark).Contains('LifeActivity onPause')) { $out += 'losing the focus paused the Activity' }
  $mark = LogMark $ReadLog
  [void][GesherLifeWin32]::PostMessageW($app, 0x0007, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_SETFOCUS
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity onWindowFocusChanged true (resumed)' 30)) { $out += 'no onWindowFocusChanged(true) after WM_SETFOCUS' }

  # 2. A PopupWindow watching outside touches: a click outside it is ACTION_OUTSIDE.
  $mark = LogMark $ReadLog
  [GesherWin32]::Click($app, $popupAt[0], $popupAt[1])
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity popup shown at')) { $out += 'no PopupWindow' } else {
    Start-Sleep 2
    $mark = LogMark $ReadLog
    [GesherWin32]::Click($app, $redAt[0], $redAt[1])
    if (-not (WaitAfter $ReadLog $mark 'LifeActivity popup: ACTION_OUTSIDE at' 30)) { $out += 'the PopupWindow got no ACTION_OUTSIDE for a click outside it' }
    if (-not (WaitAfter $ReadLog $mark 'LifeActivity popup dismissed' 30)) { $out += 'the PopupWindow was not dismissed by a click outside it' }
    if (-not (WaitAfter $ReadLog $mark 'LifeActivity red clicked' 30)) { $out += 'the click outside the PopupWindow did not reach the Activity' }
  }

  # 3. Minimized: paused, stopped, state saved; restored: restarted, resumed.
  $mark = LogMark $ReadLog
  [void][GesherLifeWin32]::ShowWindowAsync($app, 6)  # SW_MINIMIZE
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity onStop')) { $out += 'minimizing the window did not stop the Activity' } else {
    [void](WaitAfter $ReadLog $mark 'LifeActivity onSaveInstanceState' 30)
    $out += @(OrderProblems $ReadLog $mark @('LifeActivity onPause', 'LifeActivity onStop', 'LifeActivity onSaveInstanceState') 'minimized')
    Start-Sleep 2
    Write-Host "INFO  minimized: iconic $([GesherLifeWin32]::IsIconic($app)), visible $([GesherWin32]::IsWindowVisible($app))"
    if (-not [GesherLifeWin32]::IsIconic($app)) { $out += 'the window is not minimized' }
    if (-not [GesherWin32]::IsWindowVisible($app)) { $out += 'the minimized window was hidden (no taskbar button)' }
    $mark = LogMark $ReadLog
    [void][GesherLifeWin32]::ShowWindowAsync($app, 4)  # SW_SHOWNOACTIVATE: restored, not activated
    if (-not (WaitAfter $ReadLog $mark 'LifeActivity onResume')) { $out += 'restoring the window did not resume the Activity' } else {
      $out += @(OrderProblems $ReadLog $mark @('LifeActivity onRestart', 'LifeActivity onStart', 'LifeActivity onResume') 'restored')
      $share = LifeWaitRed $app $red
      Write-Host "INFO  restored: $([int]($share * 100))% of the red View drawn"
      if ($share -lt 0.9) { $out += "the restored window shows $([int]($share * 100))% of the red View" }
    }
  }

  # 4. Resized to landscape and back: LifeActivity handles size changes
  #    (onConfigurationChanged, not relaunched), its resources follow (-land, -w600dp).
  $mark = LogMark $ReadLog
  [GesherWin32]::SetClientSize($app, 640, 400)
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity onConfigurationChanged: 640x400 dp, smallest 400 dp, landscape')) { $out += 'no onConfigurationChanged for a 640x400 window' }
  elseif (-not (WaitAfter $ReadLog $mark 'resources land w600dp' 10)) { $out += 'the resources did not resolve -land and -w600dp in a 640x400 window' }
  $mark = LogMark $ReadLog
  [GesherWin32]::SetClientSize($app, 400, 800)
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity onConfigurationChanged: 400x800 dp, smallest 400 dp, portrait')) { $out += 'no onConfigurationChanged back to 400x800' }
  elseif (-not (WaitAfter $ReadLog $mark 'resources port narrow' 10)) { $out += 'the resources did not go back to port and narrow' }
  if ((LogAfter $ReadLog $mark).Contains('LifeActivity onDestroy')) { $out += 'LifeActivity was relaunched although it handles size changes' }

  if ($Device) {
    $text = "$(& $ReadLog)"
    # 4, 5. Translucent and floating Activities leave LifeActivity paused, visible.
    $out += @(OverActivity $app $ReadLog (LifePoint $text 'translucent at (\d+),(\d+)') $red 'TranslucentActivity' 'Gesher translucent activity')
    $out += @(OverActivity $app $ReadLog (LifePoint $text 'dialog activity at (\d+),(\d+)') $red 'DialogThemeActivity' 'Gesher dialog-themed activity')

    # 6. An opaque Activity stops it and hides its window until it finishes.
    $mark = LogMark $ReadLog
    $at = LifePoint $text 'opaque at (\d+),(\d+)'
    [GesherWin32]::Click($app, $at[0], $at[1])
    if (-not (WaitAfter $ReadLog $mark 'LifeActivity onStop')) { $out += 'an opaque Activity did not stop LifeActivity' } else {
      [void](WaitAfter $ReadLog $mark 'LifeActivity onSaveInstanceState' 30)
      $out += @(OrderProblems $ReadLog $mark @('LifeActivity onPause', 'OpaqueActivity onResume', 'LifeActivity onStop', 'LifeActivity onSaveInstanceState') 'under an opaque Activity')
      if (-not (LifeWaitGone $app 30)) { $out += 'LifeActivity''s window stayed shown under an opaque Activity' }
      $opaque = LifeWindow $ReadLog $mark 'Gesher opaque activity'
      if ($opaque -eq [IntPtr]::Zero) { $out += 'no window for OpaqueActivity' } else {
        # OpaqueActivity does not handle size changes: resizing its window relaunches it
        # with its saved state, in the same window.
        Start-Sleep 2
        $mark = LogMark $ReadLog
        [GesherWin32]::SetClientSize($opaque, 640, 400)
        if (-not (WaitAfter $ReadLog $mark 'OpaqueActivity onCreate with saved state')) { $out += 'resizing OpaqueActivity''s window did not relaunch it' } else {
          [void](WaitAfter $ReadLog $mark 'OpaqueActivity onResume' 30)
          $out += @(OrderProblems $ReadLog $mark @('OpaqueActivity onPause', 'OpaqueActivity onDestroy (changing configurations)', 'OpaqueActivity onCreate with saved state', 'landscape', 'resources land w600dp', 'OpaqueActivity onResume') 'OpaqueActivity relaunched')
          if (-not [GesherWin32]::IsWindow($opaque) -or -not [GesherWin32]::IsWindowVisible($opaque)) { $out += 'OpaqueActivity''s window did not stay through the relaunch' }
        }
        $mark = LogMark $ReadLog
        [GesherWin32]::SetClientSize($opaque, 400, 800)
        if (-not (WaitAfter $ReadLog $mark 'OpaqueActivity onResume')) { $out += 'OpaqueActivity was not relaunched back at 400x800' }
        Start-Sleep 2
        $mark = LogMark $ReadLog
        [void][GesherWin32]::PostMessageW($opaque, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        if (-not (WaitAfter $ReadLog $mark 'LifeActivity onResume')) { $out += 'LifeActivity was not resumed after OpaqueActivity finished' } else {
          $out += @(OrderProblems $ReadLog $mark @('OpaqueActivity onPause', 'LifeActivity onRestart', 'LifeActivity onStart', 'LifeActivity onResume') 'after an opaque Activity')
          $deadline = (Get-Date).AddSeconds(30)
          while ((Get-Date) -lt $deadline -and -not [GesherWin32]::IsWindowVisible($app)) { Start-Sleep -Milliseconds 250 }
          if (-not [GesherWin32]::IsWindowVisible($app)) { $out += 'LifeActivity''s window did not come back' }
        }
      }
    }
  }
  return $out
}

# A shown window (on the desktop) and an AlertDialog over it: what the desktop
# shows (DWM's composition, read from the screen) is the dialog's card over the
# app window dimmed behind it, and, at the corners of the dialog's popup window
# (its shadow and the transparent room around the card), the dimmed app window
# through it: the popup is composed with per-pixel alpha (a layered window).
function ComposeScenario([IntPtr] $app, [scriptblock] $ReadLog) {
  $out = @()
  if (-not (WaitForLog $ReadLog 'LifeActivity laid out')) { return @('no layout report') }
  [void][GesherLifeWin32]::SetWindowPos($app, [IntPtr]::Zero, 16, 16, 0, 0, 0x0001 -bor 0x0004)  # NOSIZE, NOZORDER
  Start-Sleep 3
  $text = "$(& $ReadLog)"
  $at = LifePoint $text 'dialog at (\d+),(\d+)'
  $a = [GesherLifeWin32]::ClientOnScreen($app)
  Write-Host "INFO  screen $([GesherLifeWin32]::GetSystemMetrics(0))x$([GesherLifeWin32]::GetSystemMetrics(1)); the app window's client area at $($a -join ',')"
  $before = [GesherLifeWin32]::ScreenPixel($a[0] + 8, $a[3] - 8)
  $mark = LogMark $ReadLog
  [GesherWin32]::Click($app, $at[0], $at[1])
  if (-not (WaitAfter $ReadLog $mark 'LifeActivity dialog shown: dim')) { return @('no dialog') }
  $m = [regex]::Match((LogAfter $ReadLog $mark), 'shown as a popup of "[^"]*" at \S+, hwnd 0x([0-9a-f]+)')
  if (-not $m.Success) { return @('no popup window for the dialog') }
  $dialog = [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16)
  $dim = [double]([regex]::Match((LogAfter $ReadLog $mark), 'dialog shown: dim ([0-9.]+)').Groups[1].Value)
  Start-Sleep 3
  $r = [GesherLifeWin32]::Rect($dialog)
  $behind = [GesherLifeWin32]::ScreenPixel($a[0] + 8, $a[3] - 8)
  # Above the card, at its middle: the room left for its shadow (surface insets), inside the
  # app window (the popup may reach past its sides in a narrow window).
  $cornerAt = @([int](($r[0] + $r[2]) / 2), ($r[1] + 2))
  $corner = [GesherLifeWin32]::ScreenPixel($cornerAt[0], $cornerAt[1])
  $center = [GesherLifeWin32]::ScreenPixel([int](($r[0] + $r[2]) / 2), [int]($r[1] + ($r[3] - $r[1]) / 3))
  Write-Host ("INFO  dim {0}: the app window at its bottom left 0x{1:x6} before the dialog, 0x{2:x6} with it; the dialog's popup at {3}: above its card at {6} 0x{4:x6}, its card 0x{5:x6}" -f $dim, $before, $behind, ($r -join ','), $corner, $center, ($cornerAt -join ','))
  function Channels([uint32] $c) { return @((($c -shr 16) -band 255), (($c -shr 8) -band 255), ($c -band 255)) }
  $b = Channels $before; $d = Channels $behind; $k = Channels $corner; $c = Channels $center
  if ($b[0] -lt 200 -or $b[1] -lt 200 -or $b[2] -lt 200) { return @(("the desktop does not show the app window (0x{0:x6} where its white background is): no screen to read the composition from" -f $before)) }
  $want = 255 * (1 - $dim)
  if ([Math]::Abs($d[0] - $want) -gt 40) { $out += ("the app window behind the dialog is 0x{0:x6}, not dimmed by {1}" -f $behind, $dim) }
  if ($c[0] -lt 200 -or $c[1] -lt 200 -or $c[2] -lt 200) { $out += ("the dialog's card shows 0x{0:x6}, not white" -f $center) }
  # Behind the transparent corner: the dimmed app window (darkened a little by the shadow), not black.
  if ($k[0] -lt 0.5 * $want -or $k[0] -gt $want + 30 -or [Math]::Abs($k[0] - $k[1]) -gt 30 -or [Math]::Abs($k[1] - $k[2]) -gt 30) {
    $out += ("the dialog's transparent edge shows 0x{0:x6}, not the dimmed app window behind it (~{1:n0} gray): the popup is not composed with alpha" -f $corner, $want)
  }
  [void][GesherWin32]::PostMessageW($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE: Back
  [void](WaitAfter $ReadLog $mark 'LifeActivity dialog dismissed' 30)
  return $out
}

# Runs lifeapp.apk until its window's first frame, runs the scenario, closes it
# with WM_CLOSE; expects exit code 0 and every line of $Expect.
function LifeAppCheck([string] $Name, [string[]] $Arguments, [switch] $Device, [switch] $Compose, [hashtable] $Environment = @{}, [string[]] $Expect, [int] $Seconds = 400) {
  if (-not (Test-Path "$Dir/lifeapp.apk")) {
    Write-Host "FAIL  gesher-app: $Name (lifeapp.apk is missing)"
    $script:failed++
    return
  }
  $log = Join-Path $env:RUNNER_TEMP "lifeapp-$(Get-Random).log"
  $quoted = (@($dvmLife) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  foreach ($k in $Environment.Keys) { $psi.Environment[$k] = $Environment[$k] }
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
  $hwnd = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($Seconds)
  $readLog = { Get-Content -Raw $log -ErrorAction SilentlyContinue }.GetNewClosure()
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $m = [regex]::Match("$(& $readLog)", 'window "Gesher lifecycle test" \d+x\d+: first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)')
    if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16); break }
    Start-Sleep -Milliseconds 250
  }
  if ($hwnd -eq [IntPtr]::Zero) { $problems += 'no first frame' } else {
    Start-Sleep -Seconds 3
    try {
      if ($Compose) { $problems += @(ComposeScenario $hwnd $readLog) }
      else { $problems += @(LifeAppScenario $hwnd $readLog -Device:$Device) }
    }
    catch { $problems += "the scenario threw: $_" }
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit after WM_CLOSE' }
  elseif ($p.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $p.ExitCode) }
  $text = "$(& $readLog)"
  foreach ($want in $Expect) {
    if (-not $text.Contains($want)) { $problems += "missing '$want'" }
  }
  if (-not $problems) {
    Write-Host "PASS  gesher-app: $Name"
    ($text -split "`n" | Select-String -Pattern '(gesher-app: (window|windows of|task)|(Life|Translucent|DialogTheme|Opaque)Activity)') | Out-Host
  } else {
    Write-Host "FAIL  gesher-app: $Name ($($problems -join '; '))"
    if (Test-Path "$Dir/dalvikvm.map") {
      $text | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $text
    }
    $script:failed++
  }
}

$lifeName = 'lifecycle of lifeapp.apk''s window: losing the focus to another window is onWindowFocusChanged(false) with the Activity resumed; a click outside a PopupWindow watching outside touches is ACTION_OUTSIDE (it dismisses) and reaches the Activity; minimizing the window pauses and stops the Activity (onSaveInstanceState after onStop), the window staying minimized; restoring it restarts and resumes the Activity, which draws again'
$lifeExpect = @('LifeActivity onResume', 'LifeActivity popup: ACTION_OUTSIDE at', 'LifeActivity onRestart', 'LifeActivity onDestroy')

# 1. A single process (gesher-app --single-process --hidden-window).
LifeAppCheck "$lifeName (single process)" `
  @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/lifeapp.apk")) `
  -Expect (@($lifeExpect) + 'paused, stopped (its window was minimized)' + 'gesher-app: done')

# 1b. Shown on the desktop: a dialog is composed with alpha over the dimmed
#     app window, with the CPU pipeline and with SkiaGL (on ANGLE).
$composeName = 'translucent popups on the desktop: an AlertDialog''s popup window is composed with per-pixel alpha (its transparent corner shows the app window, dimmed behind it, read from the screen)'
LifeAppCheck "$composeName, CPU pipeline (single process, shown window)" `
  @('com.gesher.app.Main', '--window', '--display', '360x560@160', (NoDrive "$Dir/lifeapp.apk")) -Compose `
  -Expect @('LifeActivity dialog shown: dim', ', translucent', 'gesher-app: done')
LifeAppCheck "$composeName, SkiaGL (single process, shown window)" `
  @('com.gesher.app.Main', '--window', '--display', '360x560@160', (NoDrive "$Dir/lifeapp.apk")) -Compose `
  -Environment @{ GESHER_HWUI_RENDERER = 'skiagl' } `
  -Expect @('LifeActivity dialog shown: dim', 'translucent (SkiaGL frames read back)', 'gesher-app: done')

if ($env:GESHER_LIFECYCLE_SINGLE_ONLY) { return }  # a local run of the single process only

# 2. The Gesher device (its own data and logs, like 36-popups.ps1): also
#    translucent, floating and opaque Activities over LifeActivity.
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-lifecycle"
$env:GESHER_LOGS = NoDrive "$root/logs-lifecycle"
New-Item -ItemType Directory -Force "$root/data-lifecycle" | Out-Null
$env:GESHER_SHORTCUTS = '0'
$failedBeforeLifeDevice = $failed
try {
  LifeAppCheck "the Gesher device: $lifeName; an Activity with a translucent theme and one with a dialog theme leave the Activity below paused, not stopped, its window shown and drawn; an opaque Activity stops it (onSaveInstanceState) and hides its window until it finishes (onRestart, onStart, onResume)" `
    @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/lifeapp.apk"), '--hidden-window') -Device `
    -Expect (@($lifeExpect) + 'TranslucentActivity onDestroy' + 'DialogThemeActivity onDestroy' + 'OpaqueActivity onDestroy' + 'gesher-app: the process of com.gesher.lifeapp ended (exit code 0)' + 'gesher-app: done')
} finally {
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'lifecycle-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmLife) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the device shutdown timed out' }
  if ($failed -ne $failedBeforeLifeDevice) {
    foreach ($f in Get-ChildItem "$root/logs-lifecycle" -Filter *.log -ErrorAction SilentlyContinue) {
      Write-Host "--- $($f.Name)"
      Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -Last 120 | Out-Host
    }
  }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
