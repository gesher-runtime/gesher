# Dialogs, menus and other popups in an app's window (docs/graphics.md,
# "Dialogs, menus and other popups"; ADR 0040). Dot-sourced by run-tests.ps1
# after 34-windows.ps1.
#
# popupapp.apk (tests/apps/popupapp) runs with a hidden window, like uiapp in
# 34-windows.ps1. Clicks (Win32 messages posted to its windows) open:
#  - an AlertDialog: a popup window owned by the app window (GetWindow
#    GW_OWNER, WS_POPUP, no WS_EX_APPWINDOW: no taskbar button), over the
#    activity in its client area, the app window dimmed behind it (read back
#    with PrintWindow); a click on its OK button reaches the listener and the
#    dim goes; Esc cancels it; a click outside it (on the app window) cancels
#    it (a modal popup gets the touches outside it);
#  - a Toast: a popup of the app window that is never activated nor hit, shown
#    and gone after its 2 s;
#  - the options menu's overflow: its item reaches onOptionsItemSelected;
#  - a Spinner's dropdown: a click on its second item selects it;
#  - on the Gesher device also an Activity with a dialog theme (a popup of
#    the window of the Activity below it, which stays shown; WM_CLOSE finishes
#    it) and a second Activity in the task (its window takes the task window's
#    place, the first one is hidden until the second one finishes).
# Twice: gesher-app --single-process --hidden-window, and on the Gesher device.
. "$PSScriptRoot/../graphics/WindowChecks.ps1"

if (-not ('GesherPopupWin32' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherPopupWin32 {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);
  [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
  public static IntPtr Owner(IntPtr hwnd) { return GetWindow(hwnd, 4 /* GW_OWNER */); }
  public static long Style(IntPtr hwnd) { return (long) GetWindowLongPtr(hwnd, -16) & 0xffffffffL; }
  public static long ExStyle(IntPtr hwnd) { return (long) GetWindowLongPtr(hwnd, -20) & 0xffffffffL; }
  // The client area on the screen: {left, top, right, bottom}.
  public static int[] ClientOnScreen(IntPtr hwnd) {
    POINT p = new POINT();
    ClientToScreen(hwnd, ref p);
    RECT r;
    GetClientRect(hwnd, out r);
    return new int[] { p.X, p.Y, p.X + r.Right - r.Left, p.Y + r.Bottom - r.Top };
  }
  // A key pressed and released (WM_KEYDOWN, WM_KEYUP).
  public static void Key(IntPtr hwnd, uint vk) {
    long scan = MapVirtualKeyW(vk, 0);
    PostMessageW(hwnd, 0x0100, (IntPtr) vk, (IntPtr) (1 | (scan << 16)));
    PostMessageW(hwnd, 0x0101, (IntPtr) vk, (IntPtr) (1 | (scan << 16) | (3L << 30)));
  }
}
'@
}

$bcpPopups = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmPopups = @("-Xbootclasspath:$bcpPopups", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# "x,y" of the first match of $Pattern's group 1 (and 2) in the log, or $null.
function LogPoint([string] $Text, [string] $Pattern) {
  $m = [regex]::Matches($Text, $Pattern)
  if ($m.Count -eq 0) { return $null }
  $g = $m[$m.Count - 1].Groups
  return @([int]$g[1].Value, [int]$g[2].Value)
}

# The popups shown so far: @{Title; Hwnd; Parent} in order.
function PopupsShown([string] $Text) {
  foreach ($m in [regex]::Matches($Text, 'window "([^"]*)" shown as a popup of "([^"]*)" at [^,]*, hwnd 0x([0-9a-f]+)')) {
    [pscustomobject]@{ Title = $m.Groups[1].Value; Parent = $m.Groups[2].Value; Hwnd = [IntPtr][Convert]::ToInt64($m.Groups[3].Value, 16) }
  }
}

# Waits for a popup after the first $Before; returns it or $null.
function WaitPopup([scriptblock] $ReadLog, [int] $Before, [string] $TitlePattern = '.', [int] $Seconds = 90) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $new = @(PopupsShown "$(& $ReadLog)") | Select-Object -Skip $Before | Where-Object { $_.Title -match $TitlePattern }
    if ($new) { return @($new)[-1] }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $null
}

# Waits until $Want occurs more than $Times times in the log.
function WaitForCount([scriptblock] $ReadLog, [string] $Want, [int] $Times, [int] $Seconds = 90) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $n = ([regex]::Matches("$(& $ReadLog)", [regex]::Escape($Want))).Count
    if ($n -gt $Times) { return $true }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $false
}

function CountOf([scriptblock] $ReadLog, [string] $Want) { ([regex]::Matches("$(& $ReadLog)", [regex]::Escape($Want))).Count }

# A click at display point $p (the app window's client coordinates, where the
# app reports its views) in window $hwnd (a popup of $app).
function ClickAt([IntPtr] $hwnd, [IntPtr] $app, $p) {
  $a = [GesherPopupWin32]::ClientOnScreen($app)
  $w = [GesherPopupWin32]::ClientOnScreen($hwnd)
  [GesherWin32]::Click($hwnd, $p[0] - ($w[0] - $a[0]), $p[1] - ($w[1] - $a[1]))
}

function WaitGone([IntPtr] $hwnd, [int] $Seconds = 60) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline) {
    if (-not [GesherWin32]::IsWindow($hwnd) -or -not [GesherWin32]::IsWindowVisible($hwnd)) { return $true }
    Start-Sleep -Milliseconds 250
  }
  return $false
}

# What is wrong with $popup as a popup of $app: owned by it, a WS_POPUP with
# no WS_EX_APPWINDOW (no taskbar button), visible, inside its client area.
function PopupProblems($popup, [IntPtr] $app, [string] $what) {
  $out = @()
  $owner = [GesherPopupWin32]::Owner($popup.Hwnd)
  $style = [GesherPopupWin32]::Style($popup.Hwnd)
  $ex = [GesherPopupWin32]::ExStyle($popup.Hwnd)
  $r = [GesherPopupWin32]::ClientOnScreen($popup.Hwnd)
  $a = [GesherPopupWin32]::ClientOnScreen($app)
  Write-Host ("INFO  {0} '{1}': hwnd 0x{2:x}, owner 0x{3:x} (app window 0x{4:x}), style 0x{5:x8}, ex style 0x{6:x8}, at {7} in the app window's {8}, visible {9}" -f `
    $what, $popup.Title, [long]$popup.Hwnd, [long]$owner, [long]$app, $style, $ex, ($r -join ','), ($a -join ','), [GesherWin32]::IsWindowVisible($popup.Hwnd))
  if ($owner -ne $app) { $out += ("{0}: owner 0x{1:x}, not the app window 0x{2:x}" -f $what, [long]$owner, [long]$app) }
  if (($style -band 0x80000000) -eq 0) { $out += "${what}: not WS_POPUP" }
  if (($ex -band 0x40000) -ne 0) { $out += "${what}: WS_EX_APPWINDOW (a taskbar button)" }
  if (-not [GesherWin32]::IsWindowVisible($popup.Hwnd)) { $out += "${what}: not visible" }
  $cx = ($r[0] + $r[2]) / 2; $cy = ($r[1] + $r[3]) / 2
  if ($cx -lt $a[0] -or $cx -gt $a[2] -or $cy -lt $a[1] -or $cy -gt $a[3]) { $out += "${what}: its center is outside the app window's client area" }
  return $out
}

# The red View's pixels in the app window: how many are within 40 of 0xRR0000.
function RedShare([IntPtr] $app, $red, [int] $r) {
  $px = [GesherWin32]::Capture($app)
  if ($null -eq $px) { return -1 }
  $n = [GesherWin32]::Count($px, $red[0], $red[1], $red[2], $red[3], ($r -shl 16), 40)
  return $n / [Math]::Max(1, ($red[2] - $red[0]) * ($red[3] - $red[1]))
}

function WaitRed([IntPtr] $app, $red, [int] $r, [int] $Seconds = 30) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $share = RedShare $app $red $r
    if ($share -ge 0.9) { return $share }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  return $share
}

# The popups of popupapp.apk's window $app; returns the problems.
function PopupAppScenario([IntPtr] $app, [scriptblock] $ReadLog, [switch] $Device) {
  $out = @()
  if (-not (WaitForLog $ReadLog 'PopupActivity laid out')) { return @('no layout report') }
  $text = "$(& $ReadLog)"
  $dialogAt = LogPoint $text 'laid out: dialog at (\d+),(\d+)'
  $toastAt = LogPoint $text 'toast at (\d+),(\d+)'
  $menuAt = LogPoint $text 'menu at (\d+),(\d+)'
  $spinnerAt = LogPoint $text 'spinner at (\d+),(\d+)'
  $dialogActivityAt = LogPoint $text 'dialog activity at (\d+),(\d+)'
  $secondAt = LogPoint $text 'second at (\d+),(\d+)'
  $m = [regex]::Match($text, 'red (\d+),(\d+),(\d+),(\d+)')
  $red = @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value)
  # The red View, inset a little (antialiased edges).
  $red = @(($red[0] + 4), ($red[1] + 4), ($red[2] - 4), ($red[3] - 4))
  $share = WaitRed $app $red 255
  Write-Host "INFO  red before the dialog: $([int]($share * 100))%"

  # 1. An AlertDialog: owned, over the activity, the activity dimmed; OK.
  $before = @(PopupsShown "$(& $ReadLog)").Count
  [GesherWin32]::Click($app, $dialogAt[0], $dialogAt[1])
  $dialog = WaitPopup $ReadLog $before
  if (-not $dialog -or -not (WaitForLog $ReadLog 'PopupActivity dialog shown: OK at')) { return $out + 'no AlertDialog popup' }
  $out += @(PopupProblems $dialog $app 'AlertDialog')
  $text = "$(& $ReadLog)"
  $okAt = LogPoint $text 'dialog shown: OK at (\d+),(\d+)'
  $dim = [double]([regex]::Matches($text, 'dialog shown: OK at \d+,\d+, dim ([0-9.]+)') | Select-Object -Last 1).Groups[1].Value
  $dimmed = [int][Math]::Round(255 * (1 - $dim))
  $share = WaitRed $app $red $dimmed
  Write-Host "INFO  dim $dim behind the dialog: $([int]($share * 100))% of the red View is 0x$('{0:x2}' -f $dimmed)0000"
  if ($share -lt 0.9) { $out += "dim behind the dialog: $([int]($share * 100))% of the red View is 0x$('{0:x2}' -f $dimmed)0000" }
  $px = [GesherWin32]::Capture($dialog.Hwnd)
  if ($null -eq $px) { $out += 'the dialog: PrintWindow failed' } else {
    $light = [GesherWin32]::Count($px, 0, 0, $px[0], $px[1], 0xffffff, 24)
    $dark = [GesherWin32]::Count($px, 0, 0, $px[0], $px[1], 0x000000, 110) - [GesherWin32]::Count($px, 0, 0, $px[0], $px[1], 0x000000, 8)
    Write-Host "INFO  the dialog $($px[0])x$($px[1]): $light light pixels, $dark dark (text)"
    if ($light -lt 5000 -or $dark -lt 100) { $out += "the dialog's pixels: $light light, $dark dark" }
  }
  ClickAt $dialog.Hwnd $app $okAt
  if (-not (WaitForLog $ReadLog 'PopupActivity dialog: OK')) { $out += 'the OK button did not reach its listener' }
  if (-not (WaitGone $dialog.Hwnd)) { $out += 'the dialog stayed after OK' }
  $share = WaitRed $app $red 255
  if ($share -lt 0.9) { $out += "no dim after the dialog: $([int]($share * 100))% red" }

  # 2. Esc cancels a dialog.
  $before = @(PopupsShown "$(& $ReadLog)").Count
  $cancelled = CountOf $ReadLog 'PopupActivity dialog: cancelled'
  [GesherWin32]::Click($app, $dialogAt[0], $dialogAt[1])
  $dialog = WaitPopup $ReadLog $before
  if (-not $dialog) { $out += 'no second dialog' } else {
    Start-Sleep 2
    [GesherPopupWin32]::Key($dialog.Hwnd, 0x1B)  # VK_ESCAPE
    if (-not (WaitForCount $ReadLog 'PopupActivity dialog: cancelled' $cancelled)) { $out += 'Esc did not cancel the dialog' }
    if (-not (WaitGone $dialog.Hwnd)) { $out += 'the dialog stayed after Esc' }
  }

  # 3. A click outside a dialog (on the app window) cancels it, not reaching the activity.
  $before = @(PopupsShown "$(& $ReadLog)").Count
  $cancelled = CountOf $ReadLog 'PopupActivity dialog: cancelled'
  $toasts = CountOf $ReadLog 'PopupActivity toast requested'
  [GesherWin32]::Click($app, $dialogAt[0], $dialogAt[1])
  $dialog = WaitPopup $ReadLog $before
  if (-not $dialog) { $out += 'no third dialog' } else {
    Start-Sleep 2
    [GesherWin32]::Click($app, $toastAt[0], $toastAt[1])  # the Toast button, under the dim
    if (-not (WaitForCount $ReadLog 'PopupActivity dialog: cancelled' $cancelled)) { $out += 'a click outside did not cancel the dialog' }
    if (-not (WaitGone $dialog.Hwnd)) { $out += 'the dialog stayed after a click outside' }
    Start-Sleep 2
    if ((CountOf $ReadLog 'PopupActivity toast requested') -ne $toasts) { $out += 'the click outside the dialog reached the activity' }
  }

  # 4. A Toast: a popup of the app window, never activated nor hit; gone after 2 s.
  $before = @(PopupsShown "$(& $ReadLog)").Count
  [GesherWin32]::Click($app, $toastAt[0], $toastAt[1])
  $toast = WaitPopup $ReadLog $before '^Toast$'
  if (-not $toast) { $out += 'no Toast popup' } else {
    $out += @(PopupProblems $toast $app 'Toast')
    $ex = [GesherPopupWin32]::ExStyle($toast.Hwnd)
    if (($ex -band 0x08000000) -eq 0) { $out += 'the Toast can be activated (no WS_EX_NOACTIVATE)' }
    if (($ex -band 0x20) -eq 0) { $out += 'the Toast is hit by clicks (no WS_EX_TRANSPARENT)' }
    $px = [GesherWin32]::Capture($toast.Hwnd)
    if ($null -ne $px) {
      $dark = [GesherWin32]::Count($px, 0, 0, $px[0], $px[1], 0x000000, 60)
      Write-Host "INFO  the Toast $($px[0])x$($px[1]): $dark dark pixels"
    }
    if (-not (WaitForLog $ReadLog 'toast "Gesher toast" shown for 2000 ms')) { $out += 'no toast shown' }
    if (-not (WaitForLog $ReadLog 'toast "Gesher toast" hidden' 30)) { $out += 'the toast was not hidden' }
    if (-not (WaitGone $toast.Hwnd 30)) { $out += 'the Toast window stayed' }
  }

  # 5. The options menu (its overflow): the item reaches onOptionsItemSelected.
  $before = @(PopupsShown "$(& $ReadLog)").Count
  [GesherWin32]::Click($app, $menuAt[0], $menuAt[1])
  $menu = WaitPopup $ReadLog $before
  if (-not $menu -or -not (WaitForLog $ReadLog 'PopupActivity menu item at')) { $out += 'no options menu popup' } else {
    $out += @(PopupProblems $menu $app 'options menu')
    Start-Sleep 1
    ClickAt $menu.Hwnd $app (LogPoint "$(& $ReadLog)" 'menu item at (\d+),(\d+)')
    if (-not (WaitForLog $ReadLog 'PopupActivity menu: Gesher item')) { $out += 'the menu item did not reach onOptionsItemSelected' }
    if (-not (WaitGone $menu.Hwnd)) { $out += 'the menu stayed' }
  }

  # 6. A Spinner's dropdown: its second item is selected.
  $before = @(PopupsShown "$(& $ReadLog)").Count
  [GesherWin32]::Click($app, $spinnerAt[0], $spinnerAt[1])
  $drop = WaitPopup $ReadLog $before
  if (-not $drop -or -not (WaitForLog $ReadLog 'PopupActivity dropdown item 1 at')) { $out += 'no Spinner dropdown' } else {
    $out += @(PopupProblems $drop $app 'Spinner dropdown')
    Start-Sleep 1
    ClickAt $drop.Hwnd $app (LogPoint "$(& $ReadLog)" 'dropdown item 1 at (\d+),(\d+)')
    if (-not (WaitForLog $ReadLog 'PopupActivity spinner: Two')) { $out += 'the dropdown item was not selected' }
    if (-not (WaitGone $drop.Hwnd)) { $out += 'the dropdown stayed' }
  }

  if ($Device) {
    # 7. An Activity with a dialog theme: a popup of the window below it, which stays.
    $before = @(PopupsShown "$(& $ReadLog)").Count
    [GesherWin32]::Click($app, $dialogActivityAt[0], $dialogActivityAt[1])
    $da = WaitPopup $ReadLog $before 'dialog activity'
    if (-not $da -or -not (WaitForLog $ReadLog 'DialogActivity onResume')) { $out += 'no popup for the dialog-themed Activity' } else {
      $out += @(PopupProblems $da $app 'dialog-themed Activity')
      Start-Sleep 3
      if (-not [GesherWin32]::IsWindowVisible($app)) { $out += 'the window under the dialog-themed Activity was hidden' }
      [void][GesherWin32]::PostMessageW($da.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
      if (-not (WaitForLog $ReadLog 'DialogActivity onDestroy')) { $out += 'WM_CLOSE did not finish the dialog-themed Activity' }
      if (-not (WaitGone $da.Hwnd)) { $out += 'the dialog-themed Activity''s window stayed' }
    }

    # 8. A second Activity in the task takes the task window's place.
    [GesherWin32]::Click($app, $secondAt[0], $secondAt[1])
    if (-not (WaitForLog $ReadLog 'SecondActivity onResume')) { $out += 'no second Activity' } else {
      $m = $null
      $deadline = (Get-Date).AddSeconds(90)
      do {
        $m = [regex]::Match("$(& $ReadLog)", 'window "Gesher second activity" \d+x\d+: first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)')
        if ($m.Success) { break }
        Start-Sleep -Milliseconds 250
      } while ((Get-Date) -lt $deadline)
      if (-not $m.Success) { $out += 'no window for the second Activity' } else {
        $second = [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16)
        $a = [GesherPopupWin32]::ClientOnScreen($app)
        $s = [GesherPopupWin32]::ClientOnScreen($second)
        Write-Host "INFO  second Activity: hwnd 0x$('{0:x}' -f [long]$second) at $($s -join ','), the first one's at $($a -join ','), title '$([GesherWin32]::Title($second))', owner 0x$('{0:x}' -f [long][GesherPopupWin32]::Owner($second))"
        if ([GesherPopupWin32]::Owner($second) -ne [IntPtr]::Zero) { $out += 'the second Activity''s window is owned' }
        if (($s[2] - $s[0]) -ne ($a[2] - $a[0]) -or ($s[3] - $s[1]) -ne ($a[3] - $a[1])) { $out += "the second Activity's window is $($s[2] - $s[0])x$($s[3] - $s[1]), the task's $($a[2] - $a[0])x$($a[3] - $a[1])" }
        if (-not (WaitForLog $ReadLog 'PopupActivity onStop')) { $out += 'the first Activity did not stop' }
        elseif (-not (WaitGone $app 30)) { $out += 'the first Activity''s window stayed shown under the second one' }
        [void][GesherWin32]::PostMessageW($second, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        if (-not (WaitForLog $ReadLog 'SecondActivity onDestroy')) { $out += 'WM_CLOSE did not finish the second Activity' }
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline -and -not [GesherWin32]::IsWindowVisible($app)) { Start-Sleep -Milliseconds 250 }
        if (-not [GesherWin32]::IsWindowVisible($app)) { $out += 'the first Activity''s window did not come back' }
      }
    }
  }
  return $out
}

# Runs popupapp.apk until its window's first frame, runs the scenario, closes
# it with WM_CLOSE; expects exit code 0 and every line of $Expect.
function PopupAppCheck([string] $Name, [string[]] $Arguments, [switch] $Device, [string[]] $Expect, [int] $Seconds = 400) {
  if (-not (Test-Path "$Dir/popupapp.apk")) {
    Write-Host "FAIL  gesher-app: $Name (popupapp.apk is missing)"
    $script:failed++
    return
  }
  $log = Join-Path $env:RUNNER_TEMP "popupapp-$(Get-Random).log"
  $quoted = (@($dvmPopups) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
  $hwnd = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($Seconds)
  $readLog = { Get-Content -Raw $log -ErrorAction SilentlyContinue }.GetNewClosure()
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $m = [regex]::Match("$(& $readLog)", 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)')
    if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16); break }
    Start-Sleep -Milliseconds 250
  }
  if ($hwnd -eq [IntPtr]::Zero) { $problems += 'no first frame' } else {
    Start-Sleep -Seconds 3
    try { $problems += @(PopupAppScenario $hwnd $readLog -Device:$Device) }
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
    ($text -split "`n" | Select-String -Pattern '(gesher-app: (window|windows of|toast)|PopupActivity|DialogActivity|SecondActivity)') | Out-Host
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

$popupsName = 'popups in popupapp.apk''s window: an AlertDialog is a popup owned by the app window (GW_OWNER, WS_POPUP, no WS_EX_APPWINDOW) drawn over it with the app window dimmed behind it (read back), its OK button reaches the listener, Esc and a click outside cancel it; a Toast (never activated, clicked through) shows and goes; the options menu''s item and a Spinner''s dropdown item reach the app'
$popupsExpect = @('PopupActivity onResume', 'Win32 popup 0x', 'PopupActivity dialog: OK', 'PopupActivity dialog: cancelled',
  'toast "Gesher toast" hidden', 'PopupActivity menu: Gesher item', 'PopupActivity spinner: Two', 'PopupActivity onDestroy')

# 1. A single process (gesher-app --single-process --hidden-window).
PopupAppCheck "$popupsName (single process)" `
  @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/popupapp.apk")) `
  -Expect (@($popupsExpect) + 'gesher-app: done')

# 2. The Gesher device (its own data and logs, like 34-windows.ps1): also an
#    Activity with a dialog theme and a second Activity in the task.
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-popups"
$env:GESHER_LOGS = NoDrive "$root/logs-popups"
New-Item -ItemType Directory -Force "$root/data-popups" | Out-Null
$env:GESHER_SHORTCUTS = '0'
$failedBeforePopupDevice = $failed
try {
  PopupAppCheck "the Gesher device: $popupsName; an Activity with a dialog theme is a popup of the window below it, which stays shown; a second Activity in the task takes the task window's place and size, the first window hidden until it finishes" `
    @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/popupapp.apk"), '--hidden-window') -Device `
    -Expect (@($popupsExpect) + 'DialogActivity onDestroy' + 'SecondActivity onDestroy' + 'gesher-app: the process of com.gesher.popupapp ended (exit code 0)' + 'gesher-app: done')
} finally {
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'popups-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmPopups) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the device shutdown timed out' }
  if ($failed -ne $failedBeforePopupDevice) {
    foreach ($f in Get-ChildItem "$root/logs-popups" -Filter *.log -ErrorAction SilentlyContinue) {
      Write-Host "--- $($f.Name)"
      Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -Last 80 | Out-Host
    }
  }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
