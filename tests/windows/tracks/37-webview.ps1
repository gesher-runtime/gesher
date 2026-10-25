# android.webkit.WebView on Microsoft Edge WebView2 (docs/webview.md, ADR
# 0041). Dot-sourced by run-tests.ps1.
#
# 1. The WebView2 Runtime is installed (tests/windows/webview/Runtime.ps1
#    installs it when the image lacks it), and webview2_probe.exe
#    (probes/webview: compat/webview's host without Java) passes.
# 2. webapp.apk (tests/apps/webapp) in a Win32 window, twice: gesher-app
#    --single-process --window, and on the Gesher device (its app process).
#    Its asset page (file:///android_asset/index.html, green) calls a
#    @JavascriptInterface method, logs to the console, shows an alert the
#    WebChromeClient answers, fetches a URL shouldInterceptRequest answers and
#    navigates to one shouldOverrideUrlLoading takes; evaluateJavascript reads
#    values back; onPageFinished fires; a second WebView loads
#    https://example.com/ (the network). The WebViews' WebView2 windows (class
#    GesherWebView, children of the app's window) are checked at the views'
#    bounds, then after the header grows (they move) and the remote WebView is
#    hidden (its window hides); the window is read back with PrintWindow
#    (PW_RENDERFULLCONTENT: WebView2 draws with DirectComposition), and
#    CapturePreview's picture of the page ($GESHER_WEBVIEW_DEBUG) is checked
#    too. WM_CLOSE ends the run.
. "$PSScriptRoot/../graphics/WindowChecks.ps1"
. "$PSScriptRoot/../webview/Runtime.ps1"

if (-not ('GesherWebViewWin32' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class GesherWebViewWin32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] struct BITMAPINFOHEADER {
    public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount;
    public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter;
    public uint biClrUsed, biClrImportant; }
  delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lparam);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr hwnd, ref POINT p);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
  [DllImport("gdi32.dll")] static extern bool GdiFlush();
  [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bmi,
      uint usage, out IntPtr bits, IntPtr section, uint offset);
  // The direct children of class GesherWebView: "x,y wxh visible|hidden" in the
  // parent's client coordinates, in z-order (topmost first).
  public static string[] WebViewWindows(IntPtr parent) {
    var found = new List<string>();
    EnumChildWindows(parent, (h, l) => {
      var name = new StringBuilder(64);
      GetClassNameW(h, name, 64);
      if (name.ToString() != "GesherWebView" || GetParent(h) != parent) return true;
      RECT r; GetWindowRect(h, out r);
      POINT p = new POINT { X = r.Left, Y = r.Top };
      ScreenToClient(parent, ref p);
      found.Add(p.X + "," + p.Y + " " + (r.Right - r.Left) + "x" + (r.Bottom - r.Top) + " "
          + (IsWindowVisible(h) ? "visible" : "hidden"));
      return true;
    }, IntPtr.Zero);
    return found.ToArray();
  }
  // The client area with what DirectComposition shows (PW_RENDERFULLCONTENT):
  // {width, height, 0xRRGGBB row by row...}, or null.
  public static int[] CaptureFull(IntPtr hwnd) {
    RECT wr, cr; GetWindowRect(hwnd, out wr); GetClientRect(hwnd, out cr);
    int w = wr.Right - wr.Left, h = wr.Bottom - wr.Top;
    int cw = cr.Right - cr.Left, ch = cr.Bottom - cr.Top;
    POINT origin = new POINT(); ClientToScreen(hwnd, ref origin);
    int ox = origin.X - wr.Left, oy = origin.Y - wr.Top;
    IntPtr screen = GetDC(IntPtr.Zero), dc = CreateCompatibleDC(screen);
    var bi = new BITMAPINFOHEADER { biSize = 40, biWidth = w, biHeight = -h, biPlanes = 1, biBitCount = 32 };
    IntPtr bits; IntPtr bmp = CreateDIBSection(dc, ref bi, 0, out bits, IntPtr.Zero, 0);
    IntPtr old = SelectObject(dc, bmp);
    int[] result = null;
    if (PrintWindow(hwnd, dc, 2)) {
      GdiFlush();
      var all = new int[w * h];
      Marshal.Copy(bits, all, 0, all.Length);
      result = new int[2 + cw * ch];
      result[0] = cw; result[1] = ch;
      for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
          int sx = x + ox, sy = y + oy;
          result[2 + y * cw + x] = (sx < w && sy < h) ? (all[sy * w + sx] & 0xffffff) : 0;
        }
    }
    SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); ReleaseDC(IntPtr.Zero, screen);
    return result;
  }
  // Pixels in rows [y0, y1) within 40 of color (0xRRGGBB), and the rows' pixels.
  public static int[] Count(int[] px, int y0, int y1, int color) {
    int w = px[0], h = px[1], n = 0, all = 0;
    for (int y = Math.Max(0, y0); y < Math.Min(h, y1); y++)
      for (int x = 0; x < w; x++) {
        int p = px[2 + y * w + x]; all++;
        if (Math.Abs(((p >> 16) & 255) - ((color >> 16) & 255)) < 40
            && Math.Abs(((p >> 8) & 255) - ((color >> 8) & 255)) < 40
            && Math.Abs((p & 255) - (color & 255)) < 40) n++;
      }
    return new int[] { n, all };
  }
}
'@
}

$webviewFailedBefore = $failed

# --- 1. The probe -------------------------------------------------------------
if (-not $WebView2Version) {
  Write-Host 'FAIL  webview2: no WebView2 Runtime on this machine'
  $script:failed++
}
if (Test-Path "$Dir/webview2_probe.exe") {
  $probeLog = Join-Path $env:RUNNER_TEMP 'webview2-probe.log'
  $env:GESHER_WEBVIEW_DEBUG = $null
  $p = Start-Process "$Dir/webview2_probe.exe" -ArgumentList (Join-Path $env:RUNNER_TEMP 'webview2-probe-data') `
    -NoNewWindow -PassThru -RedirectStandardOutput $probeLog -RedirectStandardError "$probeLog.err"
  if (-not $p.WaitForExit(180000)) { $p.Kill($true); $p.WaitForExit() }
  $out = (Get-Content -Raw $probeLog -ErrorAction SilentlyContinue) + (Get-Content -Raw "$probeLog.err" -ErrorAction SilentlyContinue)
  "$out" -split "`n" | Where-Object { $_ -match '^(PASS|FAIL|INFO)' } | ForEach-Object { Write-Host $_.TrimEnd() }
  if ($p.ExitCode -ne 0) {
    Write-Host "FAIL  webview2: webview2_probe.exe exit $($p.ExitCode)"
    Write-Host $out
    $script:failed++
  }
} else {
  Write-Host 'FAIL  webview2: webview2_probe.exe is missing'
  $script:failed++
}

# --- 2. webapp.apk ----------------------------------------------------------
$bcpWebView = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmWebView = @("-Xbootclasspath:$bcpWebView", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# "x,y wxh" of a view the app logged ("asset 0,100 400x300 remote ...").
function WebViewPlace([string] $Line, [string] $Which) {
  $m = [regex]::Match($Line, "$Which (\d+),(\d+) (\d+)x(\d+)")
  if ($m.Success) { return "$($m.Groups[1].Value),$($m.Groups[2].Value) $($m.Groups[3].Value)x$($m.Groups[4].Value)" }
  return $null
}

function WebAppCheck([string] $Name, [string[]] $Arguments, [string[]] $Expect, [scriptblock] $Extra) {
  if (-not (Test-Path "$Dir/webapp.apk")) {
    Write-Host "FAIL  gesher-app: $Name (webapp.apk is missing)"
    $script:failed++
    return
  }
  $log = Join-Path $env:RUNNER_TEMP "webapp-$(Get-Random).log"
  $quoted = (@($dvmWebView) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.Environment['GESHER_WINDOW_DEBUG'] = '1'
  $psi.Environment['GESHER_WEBVIEW_DEBUG'] = '1'
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
  $hwnd = [IntPtr]::Zero
  $read = { if (Test-Path $log) { "$(Get-Content -Raw $log)" } else { '' } }
  $waitFor = {
    param([string] $Pattern, [int] $Seconds)
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
      $m = [regex]::Match((& $read), $Pattern)
      if ($m.Success) { return $m }
      Start-Sleep -Milliseconds 250
    }
    return [regex]::Match((& $read), $Pattern)
  }
  $m = & $waitFor 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)' 400
  if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16) } else { $problems += 'no first frame' }
  if ($hwnd -ne [IntPtr]::Zero) {
    # Phase 1: both pages loaded; the WebView2 windows at the views' bounds.
    $m = & $waitFor 'WebActivity phase 1 done: ([^\r\n]*)' 300
    if (-not $m.Success) {
      $problems += 'no phase 1'
    } else {
      $windows = [GesherWebViewWin32]::WebViewWindows($hwnd)
      Write-Host "INFO  webapp.apk phase 1: views $($m.Groups[1].Value); WebView2 windows: $($windows -join '; ')"
      $asset = WebViewPlace $m.Groups[1].Value 'asset'
      $remote = WebViewPlace $m.Groups[1].Value 'remote'
      if ($windows.Count -ne 2) { $problems += "$($windows.Count) WebView2 windows" }
      elseif ($windows -notcontains "$asset visible" -or $windows -notcontains "$remote visible") {
        $problems += "phase 1: windows '$($windows -join '; ')', views asset $asset remote $remote"
      }
    }
    # Phase 3: the header grew (the asset WebView moved down), the remote one is hidden.
    $m = & $waitFor 'WebActivity phase 3: remote hidden: ([^\r\n]*)' 120
    if (-not $m.Success) {
      $problems += 'no phase 3'
    } else {
      Start-Sleep -Seconds 1
      $windows = [GesherWebViewWin32]::WebViewWindows($hwnd)
      Write-Host "INFO  webapp.apk phase 3: views $($m.Groups[1].Value); WebView2 windows: $($windows -join '; ')"
      $asset = WebViewPlace $m.Groups[1].Value 'asset'
      # (In z-order: a window moved with SetWindowPos(HWND_TOP) comes first.)
      $hidden = @($windows | Where-Object { $_.EndsWith(' hidden') })
      if ($windows.Count -ne 2 -or $windows -notcontains "$asset visible" -or $hidden.Count -ne 1) {
        $problems += "phase 3: windows '$($windows -join '; ')', asset view $asset"
      }
      $am = [regex]::Match($asset, '^(\d+),(\d+) (\d+)x(\d+)$')
      $px = [GesherWebViewWin32]::CaptureFull($hwnd)
      if ($null -eq $px -or -not $am.Success) {
        $problems += 'PrintWindow failed'
      } else {
        $top = [int]$am.Groups[2].Value; $height = [int]$am.Groups[4].Value
        $red = [GesherWebViewWin32]::Count($px, 0, $top, 0xff0000)
        $green = [GesherWebViewWin32]::Count($px, $top + 2, $top + $height - 2, 0x00ff00)
        $greenAbove = [GesherWebViewWin32]::Count($px, 0, $top, 0x00ff00)
        # The blue 40x40 view over the asset WebView (rows 220-260 at the right edge).
        $blue = [GesherWebViewWin32]::Count($px, 224, 256, 0x0000ff)
        Write-Host "INFO  webapp.apk read back ($($px[0])x$($px[1]), PW_RENDERFULLCONTENT): header red $($red[0]) of $($red[1]), asset WebView green $($green[0]) of $($green[1]), green above it $($greenAbove[0]), blue view over it $($blue[0]) pixels"
        if ($red[0] -lt 0.95 * $red[1]) { $problems += 'the header is not red' }
        if ($green[0] -lt 0.9 * $green[1]) { $problems += 'the asset WebView is not green in the window' }
        if ($greenAbove[0] -gt 0) { $problems += 'green above the asset WebView' }
        if ($blue[0] -lt 0.9 * 40 * 32) { $problems += "the view over the asset WebView does not show ($($blue[0]) blue pixels)" }
      }
    }
    if ($Extra) { $problems += & $Extra $hwnd (& $read) }
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit after WM_CLOSE' }
  elseif ($p.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $p.ExitCode) }
  $text = & $read
  foreach ($want in $Expect) {
    if (-not "$text".Contains($want)) { $problems += "missing '$want'" }
  }
  if (-not $problems) {
    Write-Host "PASS  gesher-app: $Name"
    ("$text" -split "`n" | Select-String -Pattern '(WebActivity |gesher-webview: )' | Select-Object -First 60) | Out-Host
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

$webAppExpect = @(
  'gesher-webview: WebView2 Runtime ',
  'WebActivity onPageFinished: file:///android_asset/index.html',
  'WebActivity bridge report: sum 5, Hello WebView',
  'WebActivity bridge report: secret hidden',
  'WebActivity console: console from the page (LOG)',
  'WebActivity onJsAlert: hello from the page',
  'WebActivity onReceivedTitle: Gesher WebView test',
  'WebActivity shouldInterceptRequest: https://intercepted.example/data.json',
  'WebActivity bridge report: intercepted 42',
  'WebActivity shouldOverrideUrlLoading: https://gesher.invalid/blocked',
  'WebActivity evaluateJavascript: "Gesher WebView test|42|42"',
  'WebActivity onPageFinished: https://example.com/',
  'WebActivity remote title: "Example Domain"',
  'WebActivity progress 100',
  'gesher-webview: preview of file:///android_asset/index.html: 400x300, center #00ff00',
  'WebActivity setCookie: true',
  'WebActivity cookie: gesher=1',
  'view(s) over the WebView cut out of its window: rect 360,220-400,260',
  'WebActivity onDestroy')
$webAppName = 'webapp.apk''s WebViews on WebView2 (child windows of its Win32 window at the views'' bounds, moved and hidden with them, a view over one cut out of it): an asset page whose script calls a @JavascriptInterface method, console, alert through WebChromeClient, shouldInterceptRequest and shouldOverrideUrlLoading, evaluateJavascript returns values, onPageFinished; a remote https page; cookies; the window read back with PW_RENDERFULLCONTENT'

WebAppCheck "$webAppName (single process)" `
  @('com.gesher.app.Main', '--window', '--display', '400x800@160', (NoDrive "$Dir/webapp.apk")) `
  ($webAppExpect + @('gesher-app: done'))

# The Gesher device: the app process the system server starts hosts the WebViews.
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-webview"
$env:GESHER_LOGS = NoDrive "$root/logs-webview"
New-Item -ItemType Directory -Force "$root/data-webview" | Out-Null
$env:GESHER_SHORTCUTS = '0'
try {
  WebAppCheck "the Gesher device: $webAppName (webapp.apk's app process)" `
    @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/webapp.apk"), '--window') `
    ($webAppExpect + @('gesher-app: app process for com.gesher.webapp: pid ',
      'gesher-app: the process of com.gesher.webapp ended (exit code 0)')) `
    { param($hwnd, $text)
      $out = @()
      $m = [regex]::Match($text, 'app process for com\.gesher\.webapp: pid (\d+)')
      $owner = [GesherWin32]::ProcessId($hwnd)
      if (-not $m.Success) { $out += 'no app process pid' }
      elseif ([int]$m.Groups[1].Value -ne $owner) { $out += "the window is process $owner's, not the app process's" }
      return $out }
} finally {
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'webview-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmWebView) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the device shutdown timed out' }
  $env:ANDROID_DATA = $savedData
  $env:GESHER_LOGS = $savedLogs
  Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
. "$PSScriptRoot/../webview/Features.ps1"
if ($failed -eq $webviewFailedBefore) { Write-Host 'INFO  webview: every check passed' }
