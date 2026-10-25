# webapp.apk's FeaturesActivity (tests/apps/webapp, docs/webview.md) in
# gesher-app --single-process --window: what the first WebView test does not
# cover. Dot-sourced by tests/windows/tracks/37-webview.ps1 (uses its $Dir,
# $dvmWebView, GesherWebViewWin32, Check-style counters).
#
# The test's server (TestServer.ps1: Basic auth, POST echo, TLS with a client
# certificate request) runs in this process; real mouse input (SendInput, the
# cursor moved and clicked on the screen) clicks the page's file inputs, the
# round button over the WebView and the page next to its circle; the Windows
# file dialog is answered by typing into it (WM_SETTEXT, IDOK); the window is
# read back (PW_RENDERFULLCONTENT) for the round cut-out, and UI Automation
# looks for the page's heading in the WebView2 window.
. "$PSScriptRoot/TestServer.ps1"

if (-not ('GesherWebViewInput' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class GesherWebViewInput {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; public uint pad1, pad2; }
  delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
  [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lparam);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lparam);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr dlg, int id);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, string l);
  [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  static string Class(IntPtr h) { var s = new StringBuilder(64); GetClassNameW(h, s, 64); return s.ToString(); }

  public static void Topmost(IntPtr hwnd, bool on) {
    SetWindowPos(hwnd, on ? new IntPtr(-1) : new IntPtr(-2), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010);
    if (on) SetForegroundWindow(hwnd);
  }
  // The top-level window under a client point of hwnd (screen coordinates "x,y" too).
  public static string Under(IntPtr hwnd, int x, int y) {
    var p = new POINT { X = x, Y = y }; ClientToScreen(hwnd, ref p);
    IntPtr w = WindowFromPoint(p);
    return (GetAncestor(w, 2) == hwnd ? "ours" : "0x" + w.ToString("x")) + " (" + Class(w) + ") at " + p.X + "," + p.Y;
  }
  // A real left click at a client point of hwnd: the cursor moves there and the
  // button goes down and up (SendInput, absolute coordinates on the virtual desk).
  public static uint Click(IntPtr hwnd, int x, int y) {
    var p = new POINT { X = x, Y = y }; ClientToScreen(hwnd, ref p);
    int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
    int nx = (int)(((p.X - vx) * 65535.0 + vw / 2) / Math.Max(1, vw - 1));
    int ny = (int)(((p.Y - vy) * 65535.0 + vh / 2) / Math.Max(1, vh - 1));
    uint abs = 0x8000 | 0x4000;  // ABSOLUTE | VIRTUALDESK
    var inputs = new INPUT[] {
      new INPUT { type = 0, mi = new MOUSEINPUT { dx = nx, dy = ny, flags = abs | 0x0001 } },  // MOVE
      new INPUT { type = 0, mi = new MOUSEINPUT { dx = nx, dy = ny, flags = abs | 0x0001 | 0x0002 } },  // LEFTDOWN
      new INPUT { type = 0, mi = new MOUSEINPUT { dx = nx, dy = ny, flags = abs | 0x0001 | 0x0004 } },  // LEFTUP
    };
    return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
  }
  // The process's visible file dialog (class #32770), or zero.
  public static IntPtr FileDialog(int pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h) && Class(h) == "#32770") { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  // Types the path into the dialog's file name box and presses Open: "" or what failed.
  public static string AnswerFileDialog(IntPtr dlg, string path) {
    IntPtr edit = IntPtr.Zero;
    IntPtr combo = GetDlgItem(dlg, 0x47C);  // cmb13: the file name ComboBoxEx32
    if (combo != IntPtr.Zero) {
      EnumChildWindows(combo, (h, l) => { if (Class(h) == "Edit") { edit = h; return false; } return true; }, IntPtr.Zero);
    }
    if (edit == IntPtr.Zero) return "no file name box";
    SendMessageW(edit, 0x000C /* WM_SETTEXT */, IntPtr.Zero, path);
    PostMessageW(dlg, 0x0111 /* WM_COMMAND */, (IntPtr)1 /* IDOK */, GetDlgItem(dlg, 1));
    return "";
  }
  public static int Pid(IntPtr hwnd) { uint p; GetWindowThreadProcessId(hwnd, out p); return (int)p; }
}
'@
}

# The UIA tree of the WebView2 window: the first descendant named $Name, or a
# reason it is not there.
function WebViewUia([IntPtr] $Container, [string] $Name) {
  try {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes -ErrorAction Stop
  } catch {
    return "no UI Automation client ($($_.Exception.Message))"
  }
  try {
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($Container)
    $cond = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
      $e = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
      if ($e) { return "found: $($e.Current.ControlType.ProgrammaticName) '$($e.Current.Name)'" }
      Start-Sleep -Milliseconds 500
    }
    return 'not found'
  } catch {
    return "UI Automation failed: $($_.Exception.Message)"
  }
}

function FeaturesCheck {
  $name = 'webapp.apk''s FeaturesActivity on WebView2: WebMessagePorts both ways, find in page, capturePicture, geolocation and camera/microphone permissions (granted and denied), window.open into the WebView onCreateWindow gave and window.close, onShowFileChooser and the Windows file dialog (clicked with real mouse input), text zoom (text only), Basic authentication, postUrl with a body, a certificate error and a client certificate request, the back/forward list, an MHTML web archive, a round button cut out of the WebView2 window by its outline and real clicks inside and outside its circle'
  if (-not (Test-Path "$Dir/webapp.apk")) {
    Write-Host "FAIL  gesher-app: $name (webapp.apk is missing)"; $script:failed++; return
  }
  $ports = ([GesherWebTestServer]::Start()) -split ' '
  $pick = Join-Path $env:RUNNER_TEMP 'gesher-pick.txt'
  [IO.File]::WriteAllText($pick, 'hello from Windows')
  $data = Join-Path $env:RUNNER_TEMP "features-data-$(Get-Random)"
  $log = Join-Path $env:RUNNER_TEMP "features-$(Get-Random).log"
  $cmdArgs = @($dvmWebView) + @('com.gesher.app.Main', '--window', '--display', '400x800@160',
    '--data-dir', (NoDrive $data), '--activity', 'com.gesher.webapp.FeaturesActivity', (NoDrive "$Dir/webapp.apk"))
  $quoted = ($cmdArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.Environment['GESHER_WEBAPP_SERVER'] = "$($ports[0]) $($ports[1])"
  $psi.Environment['GESHER_WEBVIEW_ARGS'] = '--use-fake-device-for-media-stream'
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $problems = @()
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
  $hwnd = [IntPtr]::Zero
  $m = & $waitFor 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)' 400
  if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16) } else { $problems += 'no first frame' }
  $clicks = [System.Collections.Generic.List[string]]::new()
  if ($hwnd -ne [IntPtr]::Zero) {
    [GesherWebViewInput]::Topmost($hwnd, $true)
    $click = {
      param([int] $X, [int] $Y, [string] $What)
      $under = [GesherWebViewInput]::Under($hwnd, $X, $Y)
      $sent = [GesherWebViewInput]::Click($hwnd, $X, $Y)
      $clicks.Add("$What at $X,$Y ($under, $sent inputs)")
    }
    # The file inputs (WebChromeClient.onShowFileChooser, then the Windows dialog).
    $m = & $waitFor 'Features click pickApp at (\d+),(\d+)' 240
    if (-not $m.Success) { $problems += 'no pickApp step' } else {
      & $click ([int]$m.Groups[1].Value) ([int]$m.Groups[2].Value) 'pickApp'
      $m = & $waitFor 'Features click pickWin at (\d+),(\d+)' 60
      if (-not $m.Success) { $problems += 'no pickWin step' } else {
        & $click ([int]$m.Groups[1].Value) ([int]$m.Groups[2].Value) 'pickWin'
        $pid2 = [GesherWebViewInput]::Pid($hwnd)
        $deadline = (Get-Date).AddSeconds(60)
        $dlg = [IntPtr]::Zero
        while ((Get-Date) -lt $deadline -and $dlg -eq [IntPtr]::Zero -and -not $p.HasExited) {
          Start-Sleep -Milliseconds 300
          $dlg = [GesherWebViewInput]::FileDialog($pid2)
        }
        if ($dlg -eq [IntPtr]::Zero) { $problems += 'no Windows file dialog' } else {
          Start-Sleep -Milliseconds 1000
          $answer = [GesherWebViewInput]::AnswerFileDialog($dlg, $pick)
          Write-Host "INFO  features: the Windows file dialog 0x$($dlg.ToString('x')) answered with $pick $answer"
          if ($answer) { $problems += "file dialog: $answer" }
        }
      }
    }
    # The round button: its circle shows the app (blue), its corners the page (green).
    $m = & $waitFor 'Features click fab' 90
    if (-not $m.Success) { $problems += 'no button step' } else {
      Start-Sleep -Milliseconds 500
      $px = [GesherWebViewWin32]::CaptureFull($hwnd)
      if ($null -eq $px) { $problems += 'PrintWindow failed' } else {
        $at = { param($x, $y) '{0:x6}' -f $px[2 + $y * $px[0] + $x] }
        $center = & $at 330 330; $corner = & $at 303 303; $edge = & $at 357 330; $beside = & $at 290 330
        Write-Host "INFO  features read back: button center #$center, its corner #$corner, inside its right edge #$edge, beside it #$beside"
        $isBlue = { param($c) $v = [Convert]::ToInt32($c, 16); (($v -shr 16) -band 255) -lt 60 -and (($v -shr 8) -band 255) -lt 60 -and ($v -band 255) -gt 200 }
        $isGreen = { param($c) $v = [Convert]::ToInt32($c, 16); (($v -shr 16) -band 255) -lt 60 -and (($v -shr 8) -band 255) -gt 200 -and ($v -band 255) -lt 60 }
        if (-not (& $isBlue $center)) { $problems += "the button's center is #$center, not blue" }
        if (-not (& $isBlue $edge)) { $problems += "the button's right edge is #$edge, not blue" }
        if (-not (& $isGreen $corner)) { $problems += "the button's corner is #$corner, not the page's green" }
        if (-not (& $isGreen $beside)) { $problems += "beside the button is #$beside, not green" }
      }
      & $click 330 330 'button'
      $m = & $waitFor 'Features click corner' 20
      if (-not $m.Success) { $problems += 'the button did not get the click' } else {
        & $click 304 304 'corner'
      }
    }
    $m = & $waitFor 'Features done' 180
    if (-not $m.Success) { $problems += 'not done' }
    # Accessibility: the page's heading in the UIA tree of the first WebView2 window.
    $uia = 'no WebView2 window'
    $kids = Get-WebViewContainers $hwnd
    if ($kids.Count -gt 0) { $uia = WebViewUia $kids[0] 'Gesher features' }
    Write-Host "INFO  features: UI Automation of the WebView2 window: $uia"
    if (-not $uia.StartsWith('found')) { $problems += "UIA: $uia" }
    foreach ($c in $clicks) { Write-Host "INFO  features: clicked $c" }
    [GesherWebViewInput]::Topmost($hwnd, $false)
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit after WM_CLOSE' }
  [GesherWebTestServer]::Stop()
  $text = & $read
  $expect = @(
    'Features page: port received hello page (origin "")',
    'Features port message: echo ping',
    'Features find: match 0 of 4',
    'Features find: match 1 of 4',
    'Features capturePicture: 400x400, at 200,300 #00ff00',
    'Features onGeolocationPermissionsShowPrompt: file:///',
    'Features onPermissionRequest: file:/// android.webkit.resource.VIDEO_CAPTURE',
    'Features page: camera: stream with 1 track(s)',
    'Features onPermissionRequest: file:/// android.webkit.resource.AUDIO_CAPTURE',
    'Features page: microphone: NotAllowedError',
    'Features onCreateWindow: dialog false, user gesture false',
    'Features page: window.open returned a window',
    'Features popup onPageFinished: file:///android_asset/popup.html',
    'Features page: opener got: hello opener',
    'Features onCloseWindow: the popup',
    'Features onShowFileChooser 1: mode 0, accept .txt text/plain',
    'Features page: picked pickApp: from-app.txt (18): hello from the app',
    'Features onShowFileChooser 2: mode 0, accept text/plain',
    'gesher-webview: file chooser: a Windows file dialog',
    'Features page: picked pickWin: gesher-pick.txt (18): hello from Windows',
    'Features text zoom 200: "32px 100 400"',
    'Features text zoom 100: "16px 100 400"',
    'view(s) over the WebView cut out of its window: oval 300,300-360,360',
    'Features fab clicked',
    'Features onReceivedHttpAuthRequest: 127.0.0.1 realm gesher-test',
    'Features auth title: "auth ok"',
    'Features postUrl title: "POST a=1&b=two"',
    'Features onReceivedClientCertRequest: 127.0.0.1:',
    'Features tls title: "tls client certificate: none"',
    'Features canGoBackOrForward(-2): true',
    'Features done')
  foreach ($want in $expect) { if (-not "$text".Contains($want)) { $problems += "missing '$want'" } }
  $patterns = @(
    'Features page: geolocation: (position|error [23])',
    'Features page: page mousedown at 30[0-9],30[0-9]',
    'Features onReceivedSslError: 3 for https://127\.0\.0\.1:\d+/, issued to 127\.0\.0\.1',
    'Features history: 3 item\(s\), current 2: http://127\.0\.0\.1:\d+/auth \| http://127\.0\.0\.1:\d+/post \| https://127\.0\.0\.1:\d+/',
    'Features nav onPageFinished: http://127\.0\.0\.1:\d+/auth[\s\S]*Features nav onPageFinished: http://127\.0\.0\.1:\d+/auth',
    'Features saveWebArchive: page\.mht, \d+ bytes, multipart/related true, the page''s text true')
  foreach ($want in $patterns) { if ("$text" -notmatch $want) { $problems += "missing /$want/" } }
  if ("$text" -match 'page mousedown at 3[23]\d,3[23]\d') {
    $problems += 'the page got the click on the button'
  }
  Write-Host "INFO  features: the server saw: $(([GesherWebTestServer]::Log -split "`n" | Where-Object { $_ }) -join '; ')"
  if (-not $problems) {
    Write-Host "PASS  gesher-app: $name"
    ("$text" -split "`n" | Select-String -Pattern '(Features |gesher-webview: )' | Select-Object -First 80) | Out-Host
  } else {
    Write-Host "FAIL  gesher-app: $name ($($problems -join '; '))"
    Write-Host $text
    $script:failed++
  }
}

# The GesherWebView windows (children of the app's window).
function Get-WebViewContainers([IntPtr] $Parent) {
  if (-not ('GesherWebViewKids' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class GesherWebViewKids {
  delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lparam);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  public static IntPtr[] Of(IntPtr parent) {
    var list = new List<IntPtr>();
    EnumChildWindows(parent, (h, l) => {
      var s = new StringBuilder(64); GetClassNameW(h, s, 64);
      if (s.ToString() == "GesherWebView" && IsWindowVisible(h)) list.Add(h);
      return true;
    }, IntPtr.Zero);
    return list.ToArray();
  }
}
'@
  }
  return ,([GesherWebViewKids]::Of($Parent))
}

FeaturesCheck
