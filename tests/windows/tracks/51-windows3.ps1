# Window and activity leftovers (windows3 track, docs/windows3.md, ADR 0053).
# Dot-sourced by run-tests.ps1 after 50-services.ps1.
#
# winapp.apk (tests/apps/winapp) reports its lifecycle, onTopResumedActivityChanged
# and what it sees of the display (configuration, Display rotation and size, window
# metrics, the -land/-port qualifier); its windows are shown on the desktop:
#  - orientation: LandActivity (screenOrientation landscape) on the default 400x800
#    display gets an 800x400 window (client rect and pixels read back), a landscape
#    configuration, the display turned 90 degrees; MainActivity's
#    setRequestedOrientation(landscape) relaunches it (it handles no configuration
#    change) in the same window turned to 800x400, and back;
#  - moveTaskToBack minimizes the window (IsIconic, still visible: its taskbar
#    button), the Activity stops; restoring the window resumes it;
#  - IME insets: the touch keyboard's rows (posted as IFrameworkInputPane reports
#    them, kMsgInputPane: the runners show no touch keyboard) are the window's
#    WindowInsets.Type.ime(); adjustResize shrinks MainActivity's content,
#    adjustPan pans PanActivity's EditText into view;
#  - the clipboard: Ctrl+A/C/V/X (SendInput) in an EditText use Windows' clipboard
#    (Get-Clipboard, Set-Clipboard), the app's clip listener hears Windows' changes;
#  - on the Gesher device also multi-resume: OtherTaskActivity in a task of its
#    own leaves MainActivity resumed; the top resumed activity follows the focused
#    window (onTopResumedActivityChanged).
# In a single process (gesher-app --window) and on the Gesher device.
. "$PSScriptRoot/../graphics/WindowChecks.ps1"
if (-not ('ImeInput' -as [type])) { Add-Type -Path "$PSScriptRoot/../ime/ImeInput.cs" }
if (-not ('GesherW3Win32' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherW3Win32 {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hwnd, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  // The client area on the screen: {left, top, right, bottom}.
  public static int[] ClientOnScreen(IntPtr hwnd) {
    POINT p = new POINT();
    ClientToScreen(hwnd, ref p);
    RECT r;
    GetClientRect(hwnd, out r);
    return new int[] { p.X, p.Y, p.X + r.Right - r.Left, p.Y + r.Bottom - r.Top };
  }
}
'@
}

$bcpW3 = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmW3 = @("-Xbootclasspath:$bcpW3", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$w3Input = 0x8043  # kMsgInputPane (compat/hwui/window/gesher_window.h)

function W3After([scriptblock] $ReadLog, [int] $Mark) {
  $t = "$(& $ReadLog)"
  if ($t.Length -le $Mark) { return '' }
  return $t.Substring($Mark)
}
function W3Mark([scriptblock] $ReadLog) { return "$(& $ReadLog)".Length }

# Waits until regex $Pattern matches the log after $Mark; returns the match (or a failed one).
function W3Wait([scriptblock] $ReadLog, [int] $Mark, [string] $Pattern, [int] $Seconds = 60) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $m = [regex]::Match((W3After $ReadLog $Mark), $Pattern)
    if ($m.Success) { return $m }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $m
}

# The entries of $Want (plain text) missing from, or out of order in, the log after $Mark.
function W3Order([scriptblock] $ReadLog, [int] $Mark, [string[]] $Want, [string] $What) {
  $t = W3After $ReadLog $Mark
  $at = 0
  foreach ($e in $Want) {
    $i = $t.IndexOf($e, $at)
    if ($i -lt 0) { return @("${What}: '$e' missing or out of order (want: $(@($Want) -join ' | '))") }
    $at = $i + $e.Length
  }
  return @()
}

# The hwnd of the window titled $Title once its first frame is drawn (after $Mark).
function W3Window([scriptblock] $ReadLog, [int] $Mark, [string] $Title, [int] $Seconds = 90) {
  $t = [regex]::Escape($Title)
  $m = W3Wait $ReadLog $Mark "window `"$t`" \d+x\d+: first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)" $Seconds
  if ($m.Success) { return [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16) }
  return [IntPtr]::Zero
}

# "x,y" of MainActivity's $Name (its last layout report) as @(x, y), or $null.
function W3Point([scriptblock] $ReadLog, [string] $Name) {
  $m = [regex]::Matches("$(& $ReadLog)", "MainActivity laid out: [^\r\n]*?$Name at (\d+),(\d+)")
  if ($m.Count -eq 0) { return $null }
  $g = $m[$m.Count - 1].Groups
  return @([int]$g[1].Value, [int]$g[2].Value)
}

# Waits until MainActivity reports a layout after $Mark (its buttons where they are now).
function W3WaitLayout([scriptblock] $ReadLog, [int] $Mark, [string] $Pattern = 'MainActivity laid out: ', [int] $Seconds = 30) {
  return (W3Wait $ReadLog $Mark $Pattern $Seconds).Success
}

function W3Size([IntPtr] $hwnd) { return ([GesherWin32]::ClientSize($hwnd) -join 'x') }

# The share of the window's pixels in its central region that are $Rgb (0xRRGGBB, within 40).
function W3Share([IntPtr] $hwnd, [int] $Rgb) {
  $px = [GesherWin32]::Capture($hwnd)
  if ($null -eq $px) { return -1 }
  $w = $px[0]; $h = $px[1]
  $x0 = [int]($w / 4); $y0 = [int]($h / 4); $x1 = [int]($w * 3 / 4); $y1 = [int]($h * 3 / 4)
  $n = [GesherWin32]::Count($px, $x0, $y0, $x1, $y1, $Rgb, 40)
  return $n / [Math]::Max(1, ($x1 - $x0) * ($y1 - $y0))
}

function W3WaitSize([IntPtr] $hwnd, [string] $Want, [int] $Seconds = 30) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    if ((W3Size $hwnd) -eq $Want) { return $true }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $false
}

$w3Land = 'landscape, 800x400 dp, smallest 400 dp, rotation 90, display 800x400, real 800x400, window 800x400, max 800x400, resources land'
$w3Port = 'portrait, 400x800 dp, smallest 400 dp, rotation 0, display 400x800, real 400x800, window 400x800, max 400x800, resources port'

# One check: PASS or FAIL (counted) with what went wrong.
function W3Check([string] $Mode, [string] $Name, $Problems, [string] $Seen = '') {
  if (-not $Problems) {
    Write-Host "PASS  windows3: $Name ($Mode)$(if ($Seen) { ": $Seen" })"
  } else {
    Write-Host "FAIL  windows3: $Name ($Mode): $(@($Problems) -join '; ')$(if ($Seen) { " [$Seen]" })"
    $script:failed++
    $script:w3Failed = $true
  }
}

# LandActivity on the default display: an 800x400 window, landscape, the display turned.
function W3LandChecks([string] $Mode, [IntPtr] $land, [scriptblock] $ReadLog, [int] $Mark = 0) {
  $out = @()
  $m = W3Wait $ReadLog $Mark 'LandActivity onResume: ([^\r\n]*)' 60
  if (-not $m.Success) { $out += 'no LandActivity onResume' }
  elseif ($m.Groups[1].Value.Trim() -ne $w3Land) { $out += "LandActivity sees '$($m.Groups[1].Value.Trim())', not '$w3Land'" }
  if ($land -eq [IntPtr]::Zero) { $out += 'no window for LandActivity' } else {
    if (-not (W3WaitSize $land '800x400')) { $out += "LandActivity's window is $(W3Size $land), not 800x400" }
    $deadline = (Get-Date).AddSeconds(20)
    do { $share = W3Share $land 0x0000ff; if ($share -ge 0.9) { break }; Start-Sleep -Milliseconds 500 } while ((Get-Date) -lt $deadline)
    if ($share -lt 0.9) { $out += "LandActivity's window shows $([int]($share * 100))% blue" }
  }
  if (-not (W3Wait $ReadLog $Mark 'LandActivity laid out: content 800x400' 20).Success) { $out += 'LandActivity''s content is not 800x400' }
  W3Check $Mode 'an Activity asking for landscape (screenOrientation) on the 400x800 display gets an 800x400 window and the turned display: Configuration landscape, Display rotation 90 and size 800x400, window metrics, -land resources' $out "window $(W3Size $land), $([int]($share * 100))% blue"
}

# MainActivity turns itself (setRequestedOrientation): relaunched in the same window, turned.
function W3TurnChecks([string] $Mode, [IntPtr] $app, [scriptblock] $ReadLog) {
  $out = @()
  foreach ($step in @(@('landscape', $w3Land, '800x400'), @('portrait', $w3Port, '400x800'))) {
    $want = $step[0]
    $at = W3Point $ReadLog 'turn'
    if ($null -eq $at) { $out += 'no Turn button'; break }
    $mark = W3Mark $ReadLog
    [GesherWin32]::Click($app, $at[0], $at[1])
    $m = W3Wait $ReadLog $mark 'MainActivity onCreate with saved state[^\r\n]*' 90
    $r = W3Wait $ReadLog $mark 'MainActivity onResume: ([^\r\n]*)' 60
    if (-not $m.Success) { $out += "setRequestedOrientation($want) did not relaunch MainActivity"; break }
    $out += @(W3Order $ReadLog $mark @("MainActivity setRequestedOrientation $want", 'MainActivity onPause', 'MainActivity onSaveInstanceState', 'MainActivity onDestroy (changing configurations)', 'MainActivity onCreate with saved state', 'MainActivity onResume') "turned to $want")
    if (-not $r.Success -or $r.Groups[1].Value.Trim() -ne $step[1]) { $out += "MainActivity turned to $want sees '$($r.Groups[1].Value.Trim())', not '$($step[1])'" }
    if (-not [GesherWin32]::IsWindow($app)) { $out += "MainActivity's window did not stay through the relaunch"; break }
    if (-not (W3WaitSize $app $step[2])) { $out += "MainActivity's window turned to $want is $(W3Size $app), not $($step[2])" }
    [void](W3WaitLayout $ReadLog $mark "MainActivity laid out: content $($step[2] -replace 'x\d+$','')x")
    Start-Sleep 2
    $green = W3Share $app 0x00ff00
    if ($green -lt 0.2) { $out += "MainActivity's window turned to $want shows $([int]($green * 100))% green (its content not drawn at the new size)" }
  }
  W3Check $Mode 'setRequestedOrientation turns the task: MainActivity (it handles no configuration change) is relaunched with its saved state in the same window, turned to 800x400 landscape, and back to 400x800 portrait' $out "window $(W3Size $app)"
}

# moveTaskToBack: the window minimized (its taskbar button stays); restoring it resumes the task.
function W3BackChecks([string] $Mode, [IntPtr] $app, [scriptblock] $ReadLog) {
  $out = @()
  $at = W3Point $ReadLog 'back'
  $mark = W3Mark $ReadLog
  [GesherWin32]::Click($app, $at[0], $at[1])
  if (-not (W3Wait $ReadLog $mark 'MainActivity moveTaskToBack: true' 30).Success) { $out += 'moveTaskToBack did not answer true' }
  if (-not (W3Wait $ReadLog $mark 'MainActivity onStop' 60).Success) { $out += 'moveTaskToBack did not stop MainActivity' }
  Start-Sleep 2
  $iconic = [GesherW3Win32]::IsIconic($app); $visible = [GesherWin32]::IsWindowVisible($app)
  if (-not $iconic) { $out += 'the window is not minimized' }
  if (-not $visible) { $out += 'the window was hidden (no taskbar button)' }
  $out += @(W3Order $ReadLog $mark @('MainActivity onPause', 'MainActivity onStop') 'moved to the back')
  $mark = W3Mark $ReadLog
  [void][GesherW3Win32]::ShowWindowAsync($app, 9)  # SW_RESTORE
  if (-not (W3Wait $ReadLog $mark 'MainActivity onResume' 60).Success) { $out += 'restoring the window did not resume MainActivity' }
  else { $out += @(W3Order $ReadLog $mark @('MainActivity onRestart', 'MainActivity onStart', 'MainActivity onResume') 'restored') }
  if (-not (W3Wait $ReadLog $mark 'MainActivity onTopResumedActivityChanged true' 30).Success) { $out += 'restored MainActivity is not the top resumed activity' }
  W3Check $Mode 'Activity.moveTaskToBack minimizes its window (iconic, still visible: its taskbar button) and stops the Activity; restoring the window restarts and resumes it' $out "iconic $iconic, visible $visible"
}

# The touch keyboard's rows over the bottom 300 pixels (as IFrameworkInputPane reports
# them): MainActivity's content (adjustResize) is 300 pixels shorter, ime() visible.
function W3ImeChecks([string] $Mode, [IntPtr] $app, [scriptblock] $ReadLog) {
  $out = @()
  $c = [GesherW3Win32]::ClientOnScreen($app)
  $h = $c[3] - $c[1]
  $mark = W3Mark $ReadLog
  [void][GesherW3Win32]::PostMessageW($app, $w3Input, [IntPtr]($c[3] - 300), [IntPtr]$c[3])
  $m = W3Wait $ReadLog $mark 'MainActivity laid out: content \d+x(\d+),[^\r\n]*edit bottom (\d+), ime visible true, bottom 300' 30
  if (-not $m.Success) { $out += 'no layout with the IME insets (ime visible, bottom 300)' } else {
    if ([int]$m.Groups[1].Value -ne $h - 300) { $out += "MainActivity's content is $($m.Groups[1].Value) pixels tall with the keyboard up, not $($h - 300) (adjustResize)" }
    if ([int]$m.Groups[2].Value -gt $h - 300) { $out += "the EditText's bottom is at $($m.Groups[2].Value), under the keyboard" }
  }
  if (-not (W3After $ReadLog $mark).Contains("the touch keyboard covers rows $($h - 300)-$h")) { $out += 'the window session did not report the keyboard''s rows' }
  $seen = $m.Value
  $mark = W3Mark $ReadLog
  [void][GesherW3Win32]::PostMessageW($app, $w3Input, [IntPtr]::Zero, [IntPtr]::Zero)
  $m = W3Wait $ReadLog $mark "MainActivity laid out: content \d+x$h,[^\r\n]*ime visible false, bottom 0" 30
  if (-not $m.Success) { $out += 'the content did not grow back when the keyboard went' }
  W3Check $Mode 'the touch keyboard''s rows (IFrameworkInputPane) are WindowInsets.Type.ime(): adjustResize shrinks the content above them, and it grows back when the keyboard goes' $out $seen
}

# Ctrl+A/C/V/X in the EditText with Windows' clipboard.
function W3ClipChecks([string] $Mode, [IntPtr] $app, [scriptblock] $ReadLog) {
  $out = @()
  if (-not [ImeInput]::Foreground($app)) { W3Check $Mode 'the clipboard' @('the window is not the foreground window (SendInput cannot reach it)'); return }
  $at = W3Point $ReadLog 'edit'
  [GesherWin32]::Click($app, $at[0], $at[1])
  Start-Sleep 1
  $ctrl = [uint16[]]@(0x11)
  Set-Clipboard -Value 'before 51'
  Start-Sleep 1
  [void][ImeInput]::Chord($ctrl, 0x41); [void][ImeInput]::Chord($ctrl, 0x43)
  $deadline = (Get-Date).AddSeconds(15)
  do { Start-Sleep -Milliseconds 300; $win = (Get-Clipboard -Raw) -as [string] } while ($win -ne 'gesher clip 51' -and (Get-Date) -lt $deadline)
  $steps = @("Ctrl+A Ctrl+C: Windows has '$win'")
  if ($win -ne 'gesher clip 51') { $out += "Ctrl+C put '$win' on Windows' clipboard" }
  $mark = W3Mark $ReadLog
  Set-Clipboard -Value 'from windows 51'
  if (-not (W3Wait $ReadLog $mark 'MainActivity clip changed: from windows 51' 30).Success) { $out += 'the app''s clip listener did not hear Set-Clipboard' }
  $mark = W3Mark $ReadLog
  [void][ImeInput]::Chord($ctrl, 0x41); [void][ImeInput]::Chord($ctrl, 0x56)
  $m = W3Wait $ReadLog $mark 'MainActivity edit: from windows 51\r?\n' 30
  $steps += "Ctrl+A Ctrl+V: $(if ($m.Success) { 'from windows 51' } else { 'not pasted' })"
  if (-not $m.Success) { $out += 'Ctrl+V did not paste Windows'' clipboard' }
  Set-Clipboard -Value 'other 51'
  Start-Sleep 1
  $mark = W3Mark $ReadLog
  [void][ImeInput]::Chord($ctrl, 0x41); [void][ImeInput]::Chord($ctrl, 0x58)
  $m = W3Wait $ReadLog $mark 'MainActivity edit: \r?\n' 30
  $deadline = (Get-Date).AddSeconds(15)
  do { Start-Sleep -Milliseconds 300; $win = (Get-Clipboard -Raw) -as [string] } while ($win -ne 'from windows 51' -and (Get-Date) -lt $deadline)
  $steps += "Ctrl+A Ctrl+X: the EditText $(if ($m.Success) { 'empty' } else { 'not emptied' }), Windows has '$win'"
  if (-not $m.Success) { $out += 'Ctrl+X did not cut the text' }
  if ($win -ne 'from windows 51') { $out += "Ctrl+X put '$win' on Windows' clipboard" }
  W3Check $Mode 'the clipboard is Windows'': Ctrl+C and Ctrl+X in an EditText put its text on it (Get-Clipboard), Ctrl+V pastes what Set-Clipboard put there, and ClipboardManager''s listener hears it' $out ($steps -join '; ')
}

# Multi-resume (the Gesher device): another task's Activity leaves MainActivity resumed;
# the top resumed activity follows the focused window.
function W3MultiChecks([string] $Mode, [IntPtr] $app, [scriptblock] $ReadLog) {
  $out = @()
  $at = W3Point $ReadLog 'task'
  $mark = W3Mark $ReadLog
  [GesherWin32]::Click($app, $at[0], $at[1])
  $other = W3Window $ReadLog $mark 'Gesher windows3 other task'
  if (-not (W3Wait $ReadLog $mark 'OtherTaskActivity onTopResumedActivityChanged true \(resumed\)' 60).Success) { $out += 'OtherTaskActivity did not become the top resumed activity' }
  if (-not (W3Wait $ReadLog $mark 'MainActivity onTopResumedActivityChanged false \(resumed\)' 30).Success) { $out += 'MainActivity was not told it is no longer the top resumed activity (resumed)' }
  Start-Sleep 2
  $after = W3After $ReadLog $mark
  if ($after.Contains('MainActivity onPause')) { $out += 'MainActivity was paused by an Activity in another task (multi-resume)' }
  if ($other -eq [IntPtr]::Zero) { $out += 'no window for OtherTaskActivity' }
  elseif ($other -eq $app) { $out += 'OtherTaskActivity took MainActivity''s window' }
  if (-not [GesherWin32]::IsWindowVisible($app)) { $out += 'MainActivity''s window was hidden' }
  # The user activates MainActivity's window: its task is the focused one.
  $mark = W3Mark $ReadLog
  $fg = [ImeInput]::Foreground($app)
  if (-not $fg) { $out += 'MainActivity''s window could not be brought to the foreground' }
  if (-not (W3Wait $ReadLog $mark 'MainActivity onTopResumedActivityChanged true \(resumed\)' 60).Success) { $out += 'activating MainActivity''s window did not make it the top resumed activity' }
  if (-not (W3Wait $ReadLog $mark 'OtherTaskActivity onTopResumedActivityChanged false \(resumed\)' 30).Success) { $out += 'OtherTaskActivity was not told it is no longer the top resumed activity (resumed)' }
  Start-Sleep 2
  $after = W3After $ReadLog $mark
  if ($after -match '(Main|OtherTask)Activity onPause') { $out += "an Activity was paused as the focus moved: $($Matches[0])" }
  if ($other -ne [IntPtr]::Zero) {
    $mark = W3Mark $ReadLog
    [void][GesherWin32]::PostMessageW($other, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    if (-not (W3Wait $ReadLog $mark 'OtherTaskActivity onDestroy' 60).Success) { $out += 'OtherTaskActivity did not finish on WM_CLOSE' }
    if ((W3After $ReadLog $mark).Contains('MainActivity onPause')) { $out += 'MainActivity was paused as OtherTaskActivity finished' }
  }
  W3Check $Mode 'multi-resume: an Activity in a task of its own (a window of its own) leaves MainActivity resumed; the top resumed activity (onTopResumedActivityChanged) is the focused window''s' $out
}

# The Gesher device's MainActivity starts LandActivity in its task.
function W3DeviceLand([string] $Mode, [IntPtr] $app, [scriptblock] $ReadLog) {
  $at = W3Point $ReadLog 'land'
  $mark = W3Mark $ReadLog
  [GesherWin32]::Click($app, $at[0], $at[1])
  $land = W3Window $ReadLog $mark 'Gesher windows3 land'
  [void](W3Wait $ReadLog $mark 'LandActivity onResume' 60)
  Start-Sleep 2
  W3LandChecks $Mode $land $ReadLog $mark
  if ($land -ne [IntPtr]::Zero) {
    $mark = W3Mark $ReadLog
    [void][GesherWin32]::PostMessageW($land, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    $out = @()
    $r = W3Wait $ReadLog $mark 'MainActivity onResume: ([^\r\n]*)' 60
    if (-not $r.Success) { $out += 'MainActivity was not resumed after LandActivity finished' }
    elseif ($r.Groups[1].Value.Trim() -ne $w3Port) { $out += "MainActivity sees '$($r.Groups[1].Value.Trim())' after LandActivity, not '$w3Port'" }
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and -not [GesherWin32]::IsWindowVisible($app)) { Start-Sleep -Milliseconds 250 }
    if (-not [GesherWin32]::IsWindowVisible($app)) { $out += 'MainActivity''s window did not come back' }
    if (-not (W3WaitSize $app '400x800')) { $out += "MainActivity's window is $(W3Size $app), not 400x800" }
    W3Check $Mode 'the portrait Activity below the landscape one comes back portrait, its window 400x800' $out
  }
}

# Runs winapp.apk with $Arguments until $Title's first frame, runs $Scenario on it,
# closes it with WM_CLOSE; expects exit code 0 and every line of $Expect.
function W3Run([string] $Mode, [string] $Title, [string[]] $Arguments, [scriptblock] $Scenario, [string[]] $Expect, [int] $Seconds = 400) {
  if (-not (Test-Path "$Dir/winapp.apk")) {
    Write-Host "FAIL  windows3: $Mode (winapp.apk is missing)"
    $script:failed++
    return
  }
  $script:w3Failed = $false
  $log = Join-Path $env:RUNNER_TEMP "winapp-$(Get-Random).log"
  $quoted = (@($dvmW3) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
  $readLog = { Get-Content -Raw $log -ErrorAction SilentlyContinue }.GetNewClosure()
  $hwnd = W3Window $readLog 0 $Title $Seconds
  if ($hwnd -eq [IntPtr]::Zero) { $problems += "no first frame of '$Title'" } else {
    Start-Sleep -Seconds 3
    [void][GesherW3Win32]::ShowWindowAsync($hwnd, 9)
    try { & $Scenario $hwnd $readLog }
    catch { $problems += "the scenario threw: $_ $($_.ScriptStackTrace)" }
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit after WM_CLOSE' }
  elseif ($p.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $p.ExitCode) }
  $text = "$(& $readLog)"
  foreach ($want in $Expect) {
    if (-not $text.Contains($want)) { $problems += "missing '$want'" }
  }
  if (-not $problems) {
    Write-Host "PASS  windows3: winapp.apk ran and ended ($Mode)"
  } else {
    Write-Host "FAIL  windows3: winapp.apk ($Mode): $($problems -join '; ')"
    $script:failed++
    $script:w3Failed = $true
  }
  if ($script:w3Failed) {
    if (Test-Path "$Dir/dalvikvm.map") {
      $text | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $text
    }
  } else {
    ($text -split "`n" | Select-String -Pattern '(gesher-app: (window|windows of|task|.*relaunch|.*orientation)|Activity (on|laid|set|move|clip|edit|drawn))' | Select-Object -First 120) | Out-Host
  }
}

$apkW3 = NoDrive "$Dir/winapp.apk"
$single = 'single process'

# 1. A single process (gesher-app --window): MainActivity turns itself, moves its task to
#    the back, the keyboard's insets, the clipboard.
W3Run $single 'Gesher windows3 main' @('com.gesher.app.Main', '--window', '--display', '400x800@160', $apkW3) {
  param($app, $readLog)
  [void](WaitForLog $readLog 'MainActivity laid out' 60)
  Start-Sleep 2
  W3TurnChecks $single $app $readLog
  W3ImeChecks $single $app $readLog
  W3ClipChecks $single $app $readLog
  W3BackChecks $single $app $readLog
} @('MainActivity onTopResumedActivityChanged true', 'relaunching for configuration changes', 'MainActivity onDestroy (finishing)', 'gesher-app: done')

# 1b. LandActivity started by itself: its manifest's orientation turns the display.
W3Run "$single, LandActivity" 'Gesher windows3 land' @('com.gesher.app.Main', '--window', '--display', '400x800@160', '--activity', 'com.gesher.winapp.LandActivity', $apkW3) {
  param($app, $readLog)
  Start-Sleep 2
  W3LandChecks $single $app $readLog
} @('its task''s display turned to ROTATION_90', 'gesher-app: done')

# 1c. PanActivity (adjustPan): the keyboard over its EditText pans the window's content.
W3Run "$single, PanActivity" 'Gesher windows3 pan' @('com.gesher.app.Main', '--window', '--display', '400x800@160', '--activity', 'com.gesher.winapp.PanActivity', $apkW3) {
  param($app, $readLog)
  $out = @()
  $before = W3Wait $readLog 0 'PanActivity drawn: edit top (\d+), edit bottom (\d+), content \d+x(\d+), ime visible false' 60
  $c = [GesherW3Win32]::ClientOnScreen($app)
  $h = $c[3] - $c[1]
  $mark = W3Mark $readLog
  [void][GesherW3Win32]::PostMessageW($app, $w3Input, [IntPtr]($c[3] - 300), [IntPtr]$c[3])
  $m = W3Wait $readLog $mark 'PanActivity drawn: edit top (-?\d+), edit bottom (-?\d+), content \d+x(\d+), ime visible true, bottom 300' 30
  if (-not $before.Success) { $out += 'no first report' }
  if (-not $m.Success) { $out += 'no frame with the IME insets' } else {
    if ([int]$m.Groups[2].Value -gt $h - 300) { $out += "the EditText's bottom is at $($m.Groups[2].Value) in the window, under the keyboard (from row $($h - 300)): not panned" }
    if ($before.Success -and [int]$m.Groups[3].Value -ne [int]$before.Groups[3].Value) { $out += "the content's height changed ($($before.Groups[3].Value) to $($m.Groups[3].Value)): resized, not panned" }
  }
  W3Check $single 'adjustPan: the touch keyboard''s IME insets pan the window''s content so the focused EditText under them shows, without resizing it' $out "$($before.Value) / $($m.Value)"
} @('gesher-app: done')

if ($env:GESHER_WINDOWS3_SINGLE_ONLY) { return }

# 2. The Gesher device (its own data and logs): also multi-resume, and LandActivity started
#    in MainActivity's task.
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-windows3"
$env:GESHER_LOGS = NoDrive "$root/logs-windows3"
New-Item -ItemType Directory -Force "$root/data-windows3" | Out-Null
$env:GESHER_SHORTCUTS = '0'
$failedBeforeW3Device = $failed
$device = 'the Gesher device'
try {
  W3Run $device 'Gesher windows3 main' @('com.gesher.app.Launcher', 'launch', $apkW3, '--window') {
    param($app, $readLog)
    [void](WaitForLog $readLog 'MainActivity laid out' 90)
    Start-Sleep 2
    W3MultiChecks $device $app $readLog
    W3DeviceLand $device $app $readLog
    W3TurnChecks $device $app $readLog
    W3ImeChecks $device $app $readLog
    W3ClipChecks $device $app $readLog
    W3BackChecks $device $app $readLog
  } @('MainActivity onTopResumedActivityChanged true', 'MainActivity onDestroy (changing configurations)', 'gesher-app: the process of com.gesher.winapp ended (exit code 0)', 'gesher-app: done')
} finally {
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'windows3-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmW3) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the device shutdown timed out' }
  if ($failed -ne $failedBeforeW3Device) {
    foreach ($f in Get-ChildItem "$root/logs-windows3" -Filter *.log -ErrorAction SilentlyContinue) {
      Write-Host "--- $($f.Name)"
      Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -Last 150 | Out-Host
    }
  }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
