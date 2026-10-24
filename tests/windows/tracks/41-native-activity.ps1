# Phase 6, native libraries: android.app.NativeActivity (docs/native-libs.md,
# "NativeActivity"; ADR 0032). Dot-sourced by run-tests.ps1 after
# 40-native-libs.ps1, so Check, NoDrive, $Dir, $root and $failed are in scope.
#
# nativeactivity.apk (tests/apps/nativeactivity) has no Java code; its
# Activity is the framework's NativeActivity, whose natives
# (compat/bionic/native_activity.cc) load lib/x86_64/libnativeactivity.so
# (android_native_app_glue) and call its android.app.func_name. The window is
# the Activity's own Win32 window (WindowSession, ADR 0029), taken by
# NativeActivity with Window.takeSurface: its Surface is the .so's
# ANativeWindow. The library draws a GLES 2 frame through EGL (checked with
# glReadPixels in the .so, and read back here with PrintWindow), redraws it
# from AChoreographer frame callbacks; a posted click reaches it through the
# window's InputChannel, ViewRootImpl's native input stages and AInputQueue,
# and makes it draw an ANativeWindow_lock frame (red, a white square where the
# click was), read back too; a posted key reaches it as well. Then WM_CLOSE
# (single process) or Escape (the .so calls ANativeActivity_finish; on the
# device) finishes the Activity: the glue sees PAUSE, STOP, TERM_WINDOW and
# DESTROY, and the process ends.
. "$PSScriptRoot/../graphics/WindowChecks.ps1"
if (-not ('GesherNativeActivityWin32' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherNativeActivityWin32 {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] struct BITMAPINFOHEADER {
    public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount;
    public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter;
    public uint biClrUsed, biClrImportant; }
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool GdiFlush();
  [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bmi,
      uint usage, out IntPtr bits, IntPtr section, uint offset);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y,
      int cx, int cy, uint flags);
  // Onto the desktop at (0, 0), shown, not activated (SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW).
  public static bool MoveOnScreen(IntPtr hwnd) {
    return SetWindowPos(hwnd, IntPtr.Zero, 0, 0, 0, 0, 0x0001 | 0x0010 | 0x0040);
  }
  // The most frequent colors, as "rrggbb x count, ...".
  public static string Top(int[] px, int n) {
    var counts = new System.Collections.Generic.Dictionary<int, int>();
    for (int i = 2; i < px.Length; i++) { int c; counts.TryGetValue(px[i], out c); counts[px[i]] = c + 1; }
    var list = new System.Collections.Generic.List<System.Collections.Generic.KeyValuePair<int, int>>(counts);
    list.Sort((a, b) => b.Value.CompareTo(a.Value));
    var sb = new System.Text.StringBuilder();
    for (int i = 0; i < list.Count && i < n; i++) {
      if (i > 0) sb.Append(", ");
      sb.Append(list[i].Key.ToString("x6")).Append(" x ").Append(list[i].Value);
    }
    return sb.ToString();
  }
  // As GesherWin32.Capture, with PrintWindow flags: PW_CLIENTONLY (1) asks the
  // window to paint itself (the CPU frame); | PW_RENDERFULLCONTENT (3) reads what
  // DWM composed, so a swap chain's frame too, but only what is on the desktop.
  public static int[] Capture(IntPtr hwnd) { return Capture(hwnd, 3); }
  public static int[] Capture(IntPtr hwnd, uint flags) {
    RECT r;
    if (!GetClientRect(hwnd, out r)) return null;
    int w = r.Right - r.Left, h = r.Bottom - r.Top;
    if (w < 1 || h < 1) return null;
    IntPtr screen = GetDC(IntPtr.Zero), dc = CreateCompatibleDC(screen);
    var bmi = new BITMAPINFOHEADER { biSize = 40, biWidth = w, biHeight = -h, biPlanes = 1, biBitCount = 32 };
    IntPtr bits;
    IntPtr bmp = CreateDIBSection(dc, ref bmi, 0, out bits, IntPtr.Zero, 0);
    int[] pixels = null;
    if (bmp != IntPtr.Zero) {
      IntPtr old = SelectObject(dc, bmp);
      if (PrintWindow(hwnd, dc, flags)) {
        GdiFlush();
        pixels = new int[w * h + 2];
        pixels[0] = w; pixels[1] = h;
        Marshal.Copy(bits, pixels, 2, w * h);
        for (int i = 2; i < pixels.Length; i++) pixels[i] &= 0xffffff;
      }
      SelectObject(dc, old);
      DeleteObject(bmp);
    }
    DeleteDC(dc);
    ReleaseDC(IntPtr.Zero, screen);
    return pixels;
  }
}
'@
}

$bcpNativeActivity = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmNativeActivity = @("-Xbootclasspath:$bcpNativeActivity", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# The app's GL frame: a blue clear and a green triangle over the middle
# (tests/apps/nativeactivity/jni/main.c).
function NativeActivityGlPixels($px) {
  $w = $px[0]; $h = $px[1]
  $out = @()
  $green = [GesherWin32]::Count($px, 0, 0, $w, $h, 0x00ff00, 40)
  $blue = [GesherWin32]::Count($px, 0, 0, $w, $h, 0x0000ff, 40)
  $cx = [int]($w / 2); $cy = [int]($h / 2)
  $center = [GesherWin32]::Count($px, $cx - 10, $cy - 10, $cx + 10, $cy + 10, 0x00ff00, 40)
  # The top corners: a window moved onto a desktop shorter than it is loses its
  # bottom rows to the screen's edge (they read back white).
  $corners = [GesherWin32]::Count($px, 0, 0, 20, 20, 0x0000ff, 40) + [GesherWin32]::Count($px, $w - 20, 0, $w, 20, 0x0000ff, 40)
  Write-Host "INFO  NativeActivity GL frame read back: $($w)x$($h), $green green, $blue blue, center $center/400 green, corners $corners/800 blue (most: $([GesherNativeActivityWin32]::Top($px, 3)))"
  $want = $w * $h / 8
  if ($green -lt $want * 0.9 -or $green -gt $want * 1.1) { $out += "GL frame: $green green pixels, not about $want" }
  if ($center -lt 400 -or $corners -lt 800) { $out += 'GL frame: no green triangle in the middle of a blue window' }
  return $out
}

# The CPU frame after the click at ($x, $y): red, a white 40x40 square there.
function NativeActivityCpuPixels($px, [int] $x, [int] $y) {
  $w = $px[0]; $h = $px[1]
  $out = @()
  $white = [GesherWin32]::Count($px, $x - 20, $y - 20, $x + 20, $y + 20, 0xffffff, 40)
  $red = [GesherWin32]::Count($px, 0, 0, $w, $h, 0xff0000, 40)
  Write-Host "INFO  NativeActivity CPU frame read back: $($w)x$($h), $white/1600 white at ($x, $y), $red red (most: $([GesherNativeActivityWin32]::Top($px, 3)))"
  if ($white -lt 1600 * 0.95) { $out += "CPU frame: $white white pixels at ($x, $y)" }
  if ($red -lt ($w * $h - 1600) * 0.95) { $out += "CPU frame: $red red pixels" }
  return $out
}

# Waits until the log matches $Pattern; $null if the process ends or $Seconds pass first.
function NativeActivityWait([string] $Log, [string] $Pattern, $Process, [int] $Seconds) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline) {
    $text = if (Test-Path $Log) { Get-Content -Raw $Log } else { '' }
    $m = [regex]::Match("$text", $Pattern)
    if ($m.Success) { return $m }
    if ($Process.HasExited) { return $null }
    Start-Sleep -Milliseconds 250
  }
  return $null
}

# WM_KEYDOWN and WM_KEYUP for a virtual key with its scan code.
function NativeActivityKey([IntPtr] $Hwnd, [int] $Vk, [int] $Scan) {
  [void][GesherWin32]::PostMessageW($Hwnd, 0x0100, [IntPtr]$Vk, [IntPtr]([long]1 -bor ([long]$Scan -shl 16)))
  [void][GesherWin32]::PostMessageW($Hwnd, 0x0101, [IntPtr]$Vk,
    [IntPtr]([long]1 -bor ([long]$Scan -shl 16) -bor ([long]1 -shl 30) -bor ([long]1 -shl 31)))
}

function NativeActivityCheck([string] $Name, [string[]] $Arguments, [string] $Finish, [string[]] $Expect,
                             [int] $Seconds = 400) {
  if (-not (Test-Path "$Dir/nativeactivity.apk")) {
    Write-Host "FAIL  native libs: $Name (nativeactivity.apk is missing)"
    $script:failed++
    return
  }
  $log = Join-Path $env:RUNNER_TEMP "nativeactivity-$(Get-Random).log"
  $quoted = (@($dvmNativeActivity) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
  $hwnd = [IntPtr]::Zero
  $m = NativeActivityWait $log 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)' $p $Seconds
  if ($null -eq $m) {
    $problems += 'no window'
  } else {
    $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16)
    # The GL frame, after the choreographer's frames.
    if ($null -eq (NativeActivityWait $log 'choreographer: 30 frames' $p 60)) { $problems += 'no choreographer frames' }
    Start-Sleep -Milliseconds 500
    $px = [GesherNativeActivityWin32]::Capture($hwnd)
    $gl = if ($null -eq $px) { @('PrintWindow failed (GL frame)') } else { @(NativeActivityGlPixels $px) }
    if ($gl -and $Arguments -contains '--hidden-window') {
      # A hidden window is shown off the desktop (ADR 0029); what the swap chain
      # presents there may not reach PrintWindow. The same frames on the desktop
      # (the .so renders every frame until the click):
      Write-Host "INFO  the GL frame of the window off the desktop: $($gl -join '; '); again with the window on the desktop (not activated)"
      [void][GesherNativeActivityWin32]::MoveOnScreen($hwnd)
      Start-Sleep -Seconds 2
      $px = [GesherNativeActivityWin32]::Capture($hwnd)
      $gl = if ($null -eq $px) { @('PrintWindow failed (GL frame)') } else { @(NativeActivityGlPixels $px) }
    }
    $problems += $gl
    Write-Host "INFO  nativeactivity.apk's window: title '$([GesherWin32]::Title($hwnd))', process $([GesherWin32]::ProcessId($hwnd)), visible: $([GesherWin32]::IsWindowVisible($hwnd))"
    # A click at (100, 150): WM_LBUTTONDOWN (MK_LBUTTON) and WM_LBUTTONUP, client coordinates.
    $lp = [IntPtr]((150 -shl 16) -bor 100)
    [void][GesherWin32]::PostMessageW($hwnd, 0x0201, [IntPtr]1, $lp)
    Start-Sleep -Milliseconds 100
    [void][GesherWin32]::PostMessageW($hwnd, 0x0202, [IntPtr]::Zero, $lp)
    if ($null -eq (NativeActivityWait $log 'cpu frame: .*post=ok' $p 60)) {
      $problems += 'the click did not reach the .so (no CPU frame)'
    } else {
      Start-Sleep -Seconds 1
      $px = [GesherNativeActivityWin32]::Capture($hwnd, 1)
      if ($null -eq $px) { $problems += 'PrintWindow failed (CPU frame)' } else { $problems += NativeActivityCpuPixels $px 100 150 }
    }
    # A key: A (VK 0x41, scan code 0x1E).
    NativeActivityKey $hwnd 0x41 0x1E
    if ($null -eq (NativeActivityWait $log 'input: key up, keycode 29' $p 60)) { $problems += 'the key did not reach the .so' }
    if ($Finish -eq 'escape') {
      # Escape (VK_ESCAPE, scan code 1): the .so calls ANativeActivity_finish.
      NativeActivityKey $hwnd 0x1B 0x01
    } else {
      [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
    }
  }
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit' }
  elseif ($p.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $p.ExitCode) }
  $text = if (Test-Path $log) { Get-Content -Raw $log } else { '' }
  foreach ($want in $Expect) {
    if (-not "$text".Contains($want)) { $problems += "missing '$want'" }
  }
  if ($hwnd -ne [IntPtr]::Zero -and [GesherWin32]::IsWindow($hwnd)) { $problems += 'the window is still there' }
  if (-not $problems) {
    Write-Host "PASS  native libs: $Name"
    ("$text" -split "`n" | Select-String -Pattern '(NativeActivityApp|gesher NativeActivity|gesher-app: window)') | Out-Host
  } else {
    Write-Host "FAIL  native libs: $Name ($($problems -join '; '))"
    if (Test-Path "$Dir/dalvikvm.map") {
      "$text" | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $text
    }
    $script:failed++
  }
}

# What every run shows: the natives, the ANativeActivity, the glue's thread,
# the window's GL frame, the choreographer, the click and the key, and the end.
$nativeActivityLines = @(
  ': calling gesher_nativeactivity_onCreate',
  'onCreate (func_name): sdk 36, internal ', 'assets yes, callbacks yes, package com.gesher.nativeactivity through activity->env',
  'android_main: asset "Hello from a NativeActivity asset", package com.gesher.nativeactivity through AttachCurrentThread',
  'lifecycle: START', 'lifecycle: RESUME', 'lifecycle: INPUT_CHANGED', 'lifecycle: INIT_WINDOW', 'lifecycle: GAINED_FOCUS',
  'window: 400x800', 'gl: EGL 1.5', 'window surface 400x800', 'readpixels=ok', 'gl frame: swap=ok',
  'choreographer: refresh rate callback, vsync period 16666667 ns',
  'choreographer: vsync callback, 1 frame timeline(s), presentation after the frame time, deadline after it',
  'choreographer: 30 frames drawn from postFrameCallback64, frame times increasing',
  'input: motion down at (100, 150)', 'input: motion up at (100, 150)',
  'cpu frame: 400x800 stride 400 format 1, white square at (100, 150): lock=ok, post=ok',
  'input: key down, keycode 29, scancode 30', 'input: key up, keycode 29, scancode 30',
  'lifecycle: PAUSE', 'lifecycle: STOP', 'lifecycle: TERM_WINDOW', 'lifecycle: DESTROY', 'android_main: done after')

# 1. A single process (gesher-app --single-process --hidden-window); WM_CLOSE.
NativeActivityCheck 'gesher-app runs nativeactivity.apk (NativeActivity, no Java code): ANativeActivity_onCreate, GLES 2 on the Activity''s window (glReadPixels, PrintWindow), AChoreographer, a click and a key through AInputQueue, an ANativeWindow_lock frame; WM_CLOSE finishes it (single process)' `
  @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/nativeactivity.apk")) 'close' `
  ($nativeActivityLines + @('closed by the user', 'gesher-app: done'))

# 2. The Gesher device (its own $ANDROID_DATA and logs): the app process the
#    system server starts runs the NativeActivity in its own window; Escape
#    makes the .so call ANativeActivity_finish; the process ends.
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-nativeactivity"
$env:GESHER_LOGS = NoDrive "$root/logs-nativeactivity"
New-Item -ItemType Directory -Force "$root/data-nativeactivity" | Out-Null
$env:GESHER_SHORTCUTS = '0'
try {
  NativeActivityCheck 'the Gesher device: nativeactivity.apk''s process (started by the system server) runs the NativeActivity in its own window: GLES 2, AChoreographer, a click and a key reach the .so; Escape calls ANativeActivity_finish, the process ends' `
    @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/nativeactivity.apk"), '--hidden-window') 'escape' `
    ($nativeActivityLines + @('input: escape: ANativeActivity_finish', 'gesher-app: app process for com.gesher.nativeactivity: pid ',
      'gesher-app: the system server ended the process (scheduleExit)',
      'gesher-app: the process of com.gesher.nativeactivity ended (exit code 0)', 'gesher-app: done'))
} finally {
  # The device stops (with a time limit: never wait on a process that may not end).
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'nativeactivity-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmNativeActivity) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the device shutdown timed out' }
  if ($failed) {
    foreach ($f in Get-ChildItem "$root/logs-nativeactivity" -Filter *.log -ErrorAction SilentlyContinue) {
      Write-Host "--- $($f.Name)"
      Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -Last 80 | Out-Host
    }
  }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
