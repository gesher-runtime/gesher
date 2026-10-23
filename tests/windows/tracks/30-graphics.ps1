# Graphics (docs/graphics.md): Skia, hwui, the Win32 window and input.
# Dot-sourced by run-tests.ps1: Check, RunDex, NoDrive, $Dir, $root, $bcp and
# $failed are its.

# Skia's CPU raster backend: shapes, antialiasing, FreeType text, codecs.
Check 'skia: CPU raster, FreeType text, PNG/JPEG/WebP codecs' {
  & "$Dir/skia_probe.exe" 'C:/Windows/Fonts/arial.ttf' (Join-Path $env:RUNNER_TEMP 'skia_probe.png')
} @('all passed')

# The Win32 window (compat/hwui/window): Skia draws into its back buffer and
# PrintWindow reads it back off-screen; posted input comes out of its queue;
# Win32Surface (the ANativeWindow over it) presents RGBA frames.
Check 'window: Win32 window, PrintWindow read-back, input queue, Win32Surface' {
  & "$Dir/window_probe.exe" (Join-Path $env:RUNNER_TEMP 'window_probe.png')
} @('all passed')

# Runs a class of graphics-tests.dex (tests/dex-graphics) with framework.jar
# and gesher-app.jar on the boot class path, with a time limit (a render
# thread that goes wrong tends to wait forever); passes when it exits 0 and
# prints "<Main> all passed".
function GraphicsDex([string] $Name, [string] $Main, [string[]] $Arguments = @(), [int] $Seconds = 180) {
  if (-not (Test-Path "$Dir/graphics-tests.dex")) {
    Write-Host "FAIL  dalvikvm: $Name (graphics-tests.dex is missing)"
    $script:failed++
    return
  }
  $bcpGraphics = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @("-Xbootclasspath:$bcpGraphics", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
                   '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/graphics-tests.dex"), "gesher.$Main") + $Arguments) {
    $psi.ArgumentList.Add($a)
  }
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'  # report a fault where it happens (crash.cc)
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) { $p.Kill($true); $p.WaitForExit() }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $code = if ($timedOut) { -1 } else { $p.ExitCode }
  $out = $stdout.Result + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  if ($code -eq 0 -and $out.Contains("$Main all passed")) {
    Write-Host "PASS  dalvikvm: $Name"
    ($out -split "`n" | Select-String -Pattern '^(PASS|FAIL)  ') | Out-Host
  } else {
    Write-Host ("FAIL  dalvikvm: $Name (exit 0x{0:X8})" -f $code)
    if (Test-Path "$Dir/dalvikvm.map") {
      $out | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $out
    }
    $script:failed++
  }
}

# android.graphics on framework.jar with hwui's natives (Bitmap, Canvas, Paint
# text with a font file, BitmapFactory/compress), and a Surface over
# gesher-app's Win32 window (lockCanvas, PrintWindow read-back).
# The font is copied next to the test root: paths without a drive letter are
# on the current drive, which need not be C:.
Copy-Item 'C:/Windows/Fonts/arial.ttf' (Join-Path $root 'arial.ttf')
GraphicsDex 'android.graphics (Bitmap, Canvas, Paint text, codecs) and a Surface in a Win32 window' `
  GraphicsTest @((NoDrive (Join-Path $root 'arial.ttf')), (NoDrive $env:RUNNER_TEMP))

# hwui's renderer: HardwareRenderer, its RenderThread and the CPU pipeline
# drawing a RenderNode tree into the window's Surface.
GraphicsDex 'HardwareRenderer: a RenderNode tree into the Win32 window (CPU pipeline)' HardwareRendererTest

# An app's UI in a Win32 window (docs/graphics.md, "An app's window"):
# gesher-app --hidden-window runs uiapp.apk (tests/apps/uiapp); ViewRootImpl
# lays the decor view out through com.gesher.app.window.WindowSession and hwui
# draws it into the window. Once the first frame is drawn the window is read
# back with PrintWindow (a hidden window: no desktop needed) and its layout's
# colors and text are checked; then WM_CLOSE finishes the Activity and ends
# the process.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherWin32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] struct BITMAPINFOHEADER {
    public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount;
    public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter;
    public uint biClrUsed, biClrImportant; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool GdiFlush();
  [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bmi,
      uint usage, out IntPtr bits, IntPtr section, uint offset);
  // The client area as 0xRRGGBB values row by row after [width, height]; null on failure.
  public static int[] Capture(IntPtr hwnd) {
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
      if (PrintWindow(hwnd, dc, 1 /* PW_CLIENTONLY */)) {
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
  // Pixels within `tolerance` of 0xRRGGBB `want` in [x0, x1) x [y0, y1).
  public static int Count(int[] px, int x0, int y0, int x1, int y1, int want, int tolerance) {
    int w = px[0], h = px[1], n = 0;
    for (int y = Math.Max(0, y0); y < Math.Min(h, y1); y++)
      for (int x = Math.Max(0, x0); x < Math.Min(w, x1); x++) {
        int c = px[2 + y * w + x];
        if (Math.Abs(((c >> 16) & 255) - ((want >> 16) & 255)) <= tolerance &&
            Math.Abs(((c >> 8) & 255) - ((want >> 8) & 255)) <= tolerance &&
            Math.Abs((c & 255) - (want & 255)) <= tolerance) n++;
      }
    return n;
  }
}
'@

# Runs gesher-app with a hidden window on $Apk until its first frame, reads the
# window back, checks it with $Verify (a script block given the pixels; it
# returns a list of problems), closes it with WM_CLOSE and expects the process
# to end with the Activity destroyed. Output lines matching $Expect must appear.
function WindowAppCheck([string] $Name, [string] $Apk, [scriptblock] $Verify, [string[]] $Expect,
                        [string[]] $Extra = @(), [int] $Seconds = 300) {
  if (-not (Test-Path "$Dir/$Apk")) {
    Write-Host "FAIL  gesher-app: $Name ($Apk is missing)"
    $script:failed++
    return
  }
  $bcpApp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @("-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
                   '-Xint', '-Xusejit:false', 'com.gesher.app.Main', '--hidden-window') + $Extra +
                 @((NoDrive "$Dir/$Apk"))) {
    $psi.ArgumentList.Add($a)
  }
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $log = Join-Path $env:RUNNER_TEMP "window-app-$([IO.Path]::GetFileNameWithoutExtension($Apk)).log"
  $psi.FileName = 'cmd.exe'
  $quoted = ($psi.ArgumentList | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $psi.ArgumentList.Clear()
  $psi.Arguments = "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`""
  $psi.RedirectStandardOutput = $false
  $psi.RedirectStandardError = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
  $hwnd = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($Seconds)
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
      Write-Host "INFO  $Apk's window: $($px[0])x$($px[1]), visible: $([GesherWin32]::IsWindowVisible($hwnd))"
      $problems += & $Verify $px
    }
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  if (-not $p.WaitForExit(90000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit after WM_CLOSE' }
  elseif ($p.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $p.ExitCode) }
  $text = if (Test-Path $log) { Get-Content -Raw $log } else { '' }
  foreach ($want in $Expect + @('closed by the user', 'gesher-app: done')) {
    if (-not "$text".Contains($want)) { $problems += "missing '$want'" }
  }
  if ($hwnd -ne [IntPtr]::Zero -and [GesherWin32]::IsWindow($hwnd)) { $problems += 'the window is still there' }
  if (-not $problems) {
    Write-Host "PASS  gesher-app: $Name"
    ("$text" -split "`n" | Select-String -Pattern '^(gesher-app: window|UiActivity laid out)') | Out-Host
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

# uiapp's layout at 400x800 and 160 dpi (1 dp = 1 px): red 0-200, green and
# blue halves 200-400, a yellow TextView 400-520 with black text, white below.
$uiappVerify = {
  param($px)
  $w = $px[0]; $h = $px[1]
  $out = @()
  if ($w -ne 400 -or $h -ne 800) { $out += "client area $($w)x$($h), not 400x800" }
  foreach ($r in @(@('red', 0, 0, 400, 200, 0xff0000), @('green', 0, 200, 200, 400, 0x00ff00),
                   @('blue', 200, 200, 400, 400, 0x0000ff), @('white', 0, 520, 400, 800, 0xffffff))) {
    $area = ($r[3] - $r[1]) * ($r[4] - $r[2])
    $n = [GesherWin32]::Count($px, $r[1], $r[2], $r[3], $r[4], $r[5], 40)
    Write-Host "INFO  $($r[0]): $n of $area pixels"
    if ($n -lt $area * 0.98) { $out += "$($r[0]): $n of $area pixels" }
  }
  $yellow = [GesherWin32]::Count($px, 0, 400, 400, 520, 0xffff00, 40)
  $dark = [GesherWin32]::Count($px, 0, 400, 400, 520, 0x000000, 100)
  Write-Host "INFO  TextView: $yellow yellow and $dark dark (text) pixels of 48000"
  if ($yellow -lt 30000) { $out += "TextView background: $yellow yellow pixels" }
  if ($dark -lt 100) { $out += "TextView text: $dark dark pixels" }
  $out
}

WindowAppCheck 'uiapp.apk drawn in a Win32 window (layout colors and text read back with PrintWindow), WM_CLOSE finishes it' `
  uiapp.apk $uiappVerify @('UiActivity onResume', 'window "Gesher UI test" added', 'UiActivity onDestroy') `
  @('--display', '400x800@160')
