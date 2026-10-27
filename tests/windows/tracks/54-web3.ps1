# The web3 track (ADR 0058, docs/webview.md, docs/network.md): androidx.webkit on
# Gesher's WebView, navigator.geolocation from LocationManager, accessibility (a WebView's
# AccessibilityNodeProvider from WebView2's UI Automation tree; Android's views for UI
# Automation clients, as Narrator reads them) and Windows' proxy settings,
# CONNECTIVITY_ACTION and PendingIntent network requests. web3app.apk (tests/apps/web3app)
# runs in gesher-app's single process and on the Gesher device; the test's servers
# (tests/windows/web3/Web3Servers.ps1) are a proxy that serves hosts nothing else resolves
# and a PAC script. Dot-sourced by run-tests.ps1: Check, NoDrive, $Dir, $root and $failed
# are its.
. "$PSScriptRoot/../webview/Runtime.ps1"
. "$PSScriptRoot/../web3/Web3Servers.ps1"
if (-not ('GesherWeb3Win32' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherWeb3Win32 {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool SystemParametersInfoW(uint action, uint param, IntPtr p, uint flags);
  // Windows' screen reader flag (SPI_SETSCREENREADER), as Narrator sets it.
  public static bool ScreenReader(bool on) { return SystemParametersInfoW(0x0047, on ? 1u : 0u, IntPtr.Zero, 2 /* SPIF_SENDCHANGE */); }
}
'@
}

$web3FailedBefore = $failed
$bcpWeb3 = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmWeb3 = @("-Xbootclasspath:$bcpWeb3", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$web3Ports = ([GesherWeb3Servers]::Start()) -split ' '
$web3Proxy = [int]$web3Ports[0]
$web3Http = [int]$web3Ports[1]
Write-Host "INFO  web3: test proxy on 127.0.0.1:$web3Proxy, HTTP server on 127.0.0.1:$web3Http"

# Runs dalvikvm with $Arguments until $Done is in its log (or $Seconds pass), runs $During
# with the window's HWND and the log reader once $Ready is in the log, closes what is left,
# and checks $Expect. $During returns problems (strings).
function Web3ProcessState([int] $LauncherId, [string] $Text) {
  $identifiers = @($LauncherId)
  $identifiers += @([regex]::Matches($Text, '(?:app process for [^:]+: pid |system server pid )(\d+)') |
    ForEach-Object { [int]$_.Groups[1].Value } | Select-Object -Unique -Last 3)
  foreach ($targetId in ($identifiers | Select-Object -Unique)) {
    try {
      $target = Get-Process -Id $targetId -ErrorAction Stop
      Write-Host "INFO  web3 teardown process: pid $targetId $($target.ProcessName), handles $($target.HandleCount), CPU $($target.CPU)"
      foreach ($thread in ($target.Threads | Select-Object -First 40)) {
        try {
          $state = $thread.ThreadState
          $reason = if ($state -eq [Diagnostics.ThreadState]::Wait) { $thread.WaitReason } else { '-' }
          Write-Host "INFO  web3 teardown thread: pid $targetId tid $($thread.Id), state $state, wait $reason"
        } catch { Write-Host "INFO  web3 teardown thread: pid $targetId unavailable: $($_.Exception.Message)" }
      }
      if (Get-Command Web3NativeProcessState -CommandType Function -ErrorAction SilentlyContinue) {
        Web3NativeProcessState $targetId
      }
    } catch { Write-Host "INFO  web3 teardown process: pid $targetId unavailable: $($_.Exception.Message)" }
  }
}

function Web3Run([string] $Name, [string[]] $Arguments, [string[]] $Expect, [string] $Done,
                 [string] $Ready = '', [scriptblock] $During = $null, [int] $Seconds = 400,
                 [string] $NativeLibrary = 'web3_network.so') {
  if (-not (Test-Path "$Dir/web3app.apk")) {
    Write-Host "FAIL  web3: $Name (web3app.apk is missing)"
    $script:failed++
    return ''
  }
  $log = Join-Path $env:RUNNER_TEMP "web3-$(Get-Random).log"
  $quoted = (@($dvmWeb3) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.Environment['GESHER_WINDOW_DEBUG'] = '1'
  $psi.Environment['GESHER_WEBVIEW_DEBUG'] = '1'
  $psi.Environment['GESHER_UIA_TRACE'] = '1'
  $psi.Environment['GESHER_WEB3_PORTS'] = "$web3Proxy $web3Http"
  $psi.Environment['GESHER_WEB3_NATIVE'] = NoDrive ([IO.Path]::GetFullPath((Join-Path $Dir $NativeLibrary)))
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $read = { if (Test-Path $log) { "$(Get-Content -Raw $log -ErrorAction SilentlyContinue)" } else { '' } }
  $problems = @()
  $deadline = (Get-Date).AddSeconds($Seconds)
  $hwnd = [IntPtr]::Zero
  $ran = $false
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $text = & $read
    if ($hwnd -eq [IntPtr]::Zero) {
      $m = [regex]::Match($text, 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)')
      if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16) }
    }
    if ($During -and -not $ran -and $Ready -and $text.Contains($Ready) -and $hwnd -ne [IntPtr]::Zero) {
      $ran = $true
      $problems += & $During $hwnd $read
    }
    if ($text.Contains($Done)) { break }
    Start-Sleep -Milliseconds 250
  }
  if ($During -and -not $ran) { $problems += "never ready ('$Ready')" }
  if (-not (& $read).Contains($Done)) { $problems += "no '$Done' in $Seconds s" }
  # The activity finishes itself; the process ends with it (else the window is closed).
  if (-not $p.WaitForExit(60000)) {
    if ($hwnd -ne [IntPtr]::Zero -and [GesherWeb3Win32]::IsWindow($hwnd)) {
      [void][GesherWeb3Win32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if (-not $p.WaitForExit(60000)) {
      # The existing deadline has already failed. Evidence collection cannot
      # turn a late exit into a passing result or extend the acceptance window.
      $problems += 'did not exit after 60+60 s and WM_CLOSE'
      Write-Host "INFO  web3: original teardown deadline FAILED for $Name before diagnostic collection (60+60 s and WM_CLOSE)"
      Web3ProcessState $p.Id "$(& $read)"
      if (-not $p.HasExited) { $p.Kill($true); $p.WaitForExit(); $problems += 'killed' }
    }
  }
  $text = & $read
  $processExit = $p.ExitCode
  Write-Host ("INFO  web3 process result: {0}, exit 0x{1:X8}" -f $(if ($Arguments -contains 'com.gesher.app.Launcher') { 'device launcher' } else { 'single-process VM' }), $processExit)
  if ($processExit -ne 0) { $problems += ('process exit 0x{0:X8}' -f $processExit) }
  if ($Arguments -contains 'com.gesher.app.Launcher') {
    $ended = [regex]::Match($text, '(?m)^gesher-app: the process of com\.gesher\.web3app ended \(exit code (-?\d+)\)\r?$')
    if (-not $ended.Success -or $ended.Groups[1].Value -ne '0') {
      $problems += 'missing a normal exit-code-zero completion from the actual app process'
    }
    Write-Host ("INFO  web3 device completion: " + $(if ($ended.Success) { $ended.Value.Trim() } else { '(no child exit-code marker)' }))
  }
  # Preserve the actual successful teardown evidence too, rather than only
  # checking that its marker existed beyond the normal first-80-line summary.
  ($text -split "`n" | Where-Object { $_ -match '^(Web3 a11y (finish|onPause|onStop|onDestroy|done)|gesher-app: window "Web3 test" teardown:)' } |
    Select-Object -Last 40 | ForEach-Object { "INFO  web3 lifecycle: $_".TrimEnd() }) | Out-Host
  foreach ($want in $Expect) {
    if (-not $text.Contains($want)) { $problems += "missing '$want'" }
  }
  if (-not $problems) {
    Write-Host "PASS  web3: $Name"
  } else {
    Write-Host "FAIL  web3: $Name ($($problems -join '; '))"
    # What the app and Gesher said (not the thread dump of a killed process).
    ($text -split "`n" | Where-Object { $_ -match '(Web3 |gesher-|Exception|Error|FATAL|Abort|chromium|^\s+at )' } |
      Select-Object -First 150) | Out-Host
    # Provider tracing can fill the first 150 lines before finish/teardown.
    # Keep its actual end, including any existing native thread-watch report.
    Write-Host 'INFO  web3 failure: last 120 nonempty application-log lines follow'
    ($text -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 120) | Out-Host
    if ($Arguments -contains 'com.gesher.app.Launcher') {
      # The launcher forwards app output, not the server's lifecycle decisions.
      # Read both original files on this runner; no local artifact download.
      foreach ($name in @('system_server.log', 'com.gesher.web3app.log')) {
        $deviceLog = Join-Path "$root/logs-web3" $name
        if (Test-Path -LiteralPath $deviceLog) {
          Write-Host "INFO  web3 failure: last 120 lines of device $name follow"
          Get-Content -LiteralPath $deviceLog -Tail 120 | Out-Host
        }
      }
    }
    $script:failed++
  }
  ($text -split "`n" | Select-String -Pattern '^(Web3 |gesher-a11y: |gesher-net: |gesher-webview: )' |
    Select-Object -First 80 | ForEach-Object { "INFO  $_".TrimEnd() }) | Out-Host
  return $text
}

# --- androidx.webkit ------------------------------------------------------------
$compatExpect = @(
  'WEB_MESSAGE_ARRAY_BUFFER=true',
  'Web3 compat ArrayBuffer listener: 70000 bytes intact true',
  'Web3 compat ArrayBuffer page-to-app port bytes intact true',
  'Web3 compat ArrayBuffer main-frame, reply, app-to-page port, empty buffer true',
  'Web3 compat ArrayBuffer callbacks intact true',
  'gesher-webview: androidx.webkit: the support library glue is Gesher''s',
  'Web3 compat all features supported true',
  "Web3 compat proxy override set to 127.0.0.1:$web3Proxy",
  'Web3 compat multi-process true',
  'Web3 compat safe browsing after setSafeBrowsingEnabled(false): false',
  'Web3 compat getWebViewClient is the app''s true',
  "Web3 compat listener: 'hello early-web3-proxied.test' from http://web3-proxied.test main frame true",
  'Web3 compat title: "got pong:hello early-web3-proxied.test"',
  "Web3 compat listener: 'window got compat-post'",
  'Web3 compat page got postWebMessage: "compat-post"',
  'Web3 compat after ScriptHandler.remove: "got pong:hello undefined"',
  'Web3 compat done')
$compatName = 'web3app.apk''s CompatActivity: androidx.webkit 1.18.0 (Maven) on Gesher''s WebView: WebViewFeature.isFeatureSupported (WEB_MESSAGE_LISTENER, DOCUMENT_START_SCRIPT, PROXY_OVERRIDE, MULTI_PROCESS, ...), ProxyController (a page only the test proxy serves), a WebMessageListener with replies, a document-start script and its removal, WebViewCompat.postWebMessage, WebSettingsCompat Safe Browsing'
function CompatProxied([string] $Name, [int] $Before) {
  $seen = ([GesherWeb3Servers]::Log -split "`n" | Select-Object -Skip $Before) -join "`n"
  if ($seen.Contains('proxy GET http://web3-proxied.test/compat.html')) {
    Write-Host "PASS  web3: $Name went through the ProxyController override (the test proxy served http://web3-proxied.test/compat.html)"
  } else {
    Write-Host "FAIL  web3: $Name did not reach the test proxy (it saw: $($seen -replace "`n", ' | '))"
    $script:failed++
  }
}

$before = ([GesherWeb3Servers]::Log -split "`n").Count - 1
$null = Web3Run "$compatName (single process)" @('com.gesher.app.Main', '--window', '--display', '400x800@160',
  '--activity', 'com.gesher.web3app.CompatActivity', (NoDrive "$Dir/web3app.apk")) ($compatExpect + 'gesher-app: done') 'Web3 compat done'
CompatProxied 'the single process''s WebView' $before

# --- navigator.geolocation ---------------------------------------------------------
$geoExpect = @(
  'Web3 geo test location set: 31.7683 35.2137',
  'Web3 geo onGeolocationPermissionsShowPrompt: file:///',
  'Web3 geo getCurrentPosition: "31.7683 35.2137 accuracy 7 altitude 754"',
  'Web3 geo watchPosition: [', '32.0853 34.7818 accuracy 7',
  'Web3 geo with geolocation disabled: "error 1',
  'Web3 geo done')
$null = Web3Run 'web3app.apk''s GeoActivity: navigator.geolocation in a WebView answered from LocationManager (test providers): getCurrentPosition after onGeolocationPermissionsShowPrompt, watchPosition following the test location, error 1 with geolocation disabled (single process)' @(
  'com.gesher.app.Main', '--window', '--display', '400x800@160', '--activity', 'com.gesher.web3app.GeoActivity',
  (NoDrive "$Dir/web3app.apk")) ($geoExpect + 'gesher-app: done') 'Web3 geo done'

# --- Accessibility -------------------------------------------------------------------
# A UI Automation client, as Narrator: the app's window's tree has the Android views (and
# WebView2's page under the WebView); it invokes the button, sets the edit text and
# toggles the check box.
$uiaClient = {
  param($hwnd, $read)
  $job = Start-ThreadJob -ArgumentList ([long]$hwnd) -ScriptBlock {
    param($h)
    $out = [Collections.Generic.List[string]]::new()
    try {
      Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes -ErrorAction Stop
      $A = [System.Windows.Automation.AutomationElement]
      Write-Output 'phase: AutomationElement.FromHandle'
      $root = $A::FromHandle([IntPtr]$h)
      Write-Output 'phase: read control tree'
      $find = {
        param($name)
        $cond = [System.Windows.Automation.PropertyCondition]::new($A::NameProperty, $name)
        for ($i = 0; $i -lt 40; $i++) {
          $e = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
          if ($e) { return $e }
          Start-Sleep -Milliseconds 500
        }
        return $null
      }
      # The tree, for the log: control view, 4 levels.
      $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
      $dump = [Collections.Generic.List[string]]::new()
      $visit = $null
      $visit = {
        param($e, $depth)
        if ($depth -gt 4 -or $dump.Count -gt 40) { return }
        $c = $e.Current
        $dump.Add(('  ' * $depth) + $c.ControlType.ProgrammaticName.Replace('ControlType.', '') + " '" + $c.Name + "' (" + $c.FrameworkId + ")")
        $child = $walker.GetFirstChild($e)
        while ($child) { & $visit $child ($depth + 1); $child = $walker.GetNextSibling($child) }
      }
      & $visit $root 0
      $out.Add("tree: " + ($dump -join ' | '))
      Write-Output 'phase: find and invoke Android button'
      $button = & $find 'Android button'
      if ($button) {
        $out.Add("button: " + $button.Current.ControlType.ProgrammaticName + " (" + $button.Current.FrameworkId + ")")
        $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        $out.Add('invoked')
      } else { $out.Add('no Android button') }
      Write-Output 'phase: find and set Android edit text'
      $edit = & $find 'Your name'
      if ($edit) {
        $out.Add("edit: " + $edit.Current.ControlType.ProgrammaticName)
        $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('Narrator')
        $out.Add('value set')
      } else { $out.Add('no edit text') }
      Write-Output 'phase: find and toggle Android check box'
      $check = & $find 'Remember me'
      if ($check) {
        $out.Add("check box: " + $check.Current.ControlType.ProgrammaticName)
        $check.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
        $out.Add('toggled')
      } else { $out.Add('no check box') }
      Write-Output 'phase: find WebView page button'
      $press = & $find 'Press me'
      if ($press) {
        $out.Add("web button: " + $press.Current.ControlType.ProgrammaticName + " (" + $press.Current.FrameworkId + ")")
        $press.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        $out.Add('web invoked')
      } else { $out.Add('no web button') }
    } catch {
      $out.Add("UI Automation failed: $($_.Exception.Message)")
    }
    return $out
  }
  $results = @()
  if (Wait-Job $job -Timeout 150) { $results = @(Receive-Job $job) }
  else { $results = @(Receive-Job $job) + 'UI Automation timed out' }
  Remove-Job $job -Force
  $results | ForEach-Object { Write-Host "INFO  web3 UI Automation: $_" }
  $problems = @()
  if ($results -notcontains 'web invoked') { $problems += 'the WebView page button was not invoked through UI Automation' }
  if ($results -notcontains 'invoked') { $problems += 'the Android button was not invoked through UI Automation' }
  if ($results -notcontains 'value set') { $problems += 'the edit text was not set through UI Automation' }
  if ($results -notcontains 'toggled') { $problems += 'the check box was not toggled through UI Automation' }
  if (-not ($results | Where-Object { $_ -like 'button: ControlType.Button*' })) { $problems += 'the Android button is not a UIA Button' }
  if (-not ($results | Where-Object { $_ -like 'web button: *' })) { $problems += 'WebView2''s page is not in the window''s UIA tree' }
  return $problems
}
$a11yExpect = @(
  'Web3 a11y host: android.webkit.WebView virtual -1',
  "button 'Press me' android.widget.Button clickable true",
  "heading 'Web content'",
  'Web3 a11y provider click true: title "pressed 1"',
  'Web3 a11y provider set text true: field "typed by accessibility"',
  'gesher-a11y: accessibility enabled (',
  'Web3 a11y AccessibilityManager enabled true',
  'gesher-a11y: announcement for UI Automation: Web3 announcement',
  'Web3 a11y UI Automation web button: title "pressed 2"',
  'Web3 a11y UI Automation: clicks 1, text set true, toggled true, accessibility enabled true',
  'Web3 a11y done')
$a11yName = 'web3app.apk''s A11yActivity: the WebView''s AccessibilityNodeProvider from WebView2''s UI Automation tree (heading, button clicked, field set), and a UI Automation client reading the app''s window (Android''s views as UIA Button, Edit, CheckBox: invoked, set, toggled; WebView2''s page under the WebView), AccessibilityManager enabled for it, an announcement'
# Narrator sets Windows' screen reader flag: the app's accessibility is on while it is.
function Web3ScreenReader([bool] $On) {
  if (-not [GesherWeb3Win32]::ScreenReader($On)) { Write-Host "INFO  web3: SPI_SETSCREENREADER $On failed" }
}
Web3ScreenReader $true
try {
$null = Web3Run "$a11yName (single process)" @('com.gesher.app.Main', '--window', '--display', '400x800@160',
  '--activity', 'com.gesher.web3app.A11yActivity', (NoDrive "$Dir/web3app.apk")) ($a11yExpect + 'gesher-app: done') 'Web3 a11y done' 'Web3 a11y ready for UI Automation' $uiaClient
} finally { Web3ScreenReader $false }

# --- The Gesher device: app processes of a system server ------------------------------
$bindingExpect = @(
  'Web3 net per-network DNS resolved example.com true',
  'Web3 net bound SocketFactory kernel interface and source address true',
  'Web3 net explicit TCP and UDP socket binding true',
  'Web3 net Network.openConnection server observed the selected source address true',
  'Web3 net process socket and DNS binding, invalid network preservation, unbind true',
  'Web3 net missing-network DNS refused fallback true',
  'Web3 net NDK: PASS selected-network sockets/process/DNS, raw DNS poll/result/NXDOMAIN/cancel/errors')
$null = Web3Run 'selected Network: real socket/process routing and DNS, bound HTTP, NDK raw DNS completion and cancellation (single process)' @('com.gesher.app.Main', '--window', '--display', '400x800@160', '--activity', 'com.gesher.web3app.BindingActivity', (NoDrive "$Dir/web3app.apk")) ($bindingExpect + 'raw DNS poll/result/NXDOMAIN/cancel/errors abi=x86_64' + 'Web3 net done') 'Web3 net done'
$null = Web3Run 'arm64 NDK selected Network: socket/process source, getaddrinfo and raw DNS query/send/result/cancel/errors (single process)' @('com.gesher.app.Main', '--window', '--display', '400x800@160', '--activity', 'com.gesher.web3app.BindingActivity', (NoDrive "$Dir/web3app.apk")) ($bindingExpect + 'raw DNS poll/result/NXDOMAIN/cancel/errors abi=arm64' + 'Web3 net done') 'Web3 net done' -NativeLibrary 'web3_network_arm64.so'
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-web3"
$env:GESHER_LOGS = NoDrive "$root/logs-web3"
New-Item -ItemType Directory -Force "$root/data-web3" | Out-Null
$env:GESHER_SHORTCUTS = '0'
$env:GESHER_WEB3_PORTS = "$web3Proxy $web3Http"
try {
  $launch = @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/web3app.apk"), '--window')
  $deviceEnd = 'gesher-app: the process of com.gesher.web3app ended'
  $before = ([GesherWeb3Servers]::Log -split "`n").Count - 1
  $null = Web3Run "the Gesher device: $compatName (web3app.apk's app process)" ($launch + '--activity', 'com.gesher.web3app.CompatActivity') ($compatExpect + 'gesher-app: app process for com.gesher.web3app: pid ') 'Web3 compat done'
  CompatProxied 'the app process''s WebView' $before

  $null = Web3Run 'the Gesher device: navigator.geolocation getCurrentPosition, watchPosition and disabled-setting errors from LocationManager test providers (app process)' ($launch + '--activity', 'com.gesher.web3app.GeoActivity') $geoExpect 'Web3 geo done'
  $null = Web3Run 'the Gesher device: arm64 NDK selected Network socket/process source, getaddrinfo and raw DNS query/send/result/cancel/errors (app process)' ($launch + '--activity', 'com.gesher.web3app.BindingActivity', '--ez', 'arm64Native', 'true') ($bindingExpect + 'raw DNS poll/result/NXDOMAIN/cancel/errors abi=arm64' + 'Web3 net done') 'Web3 net done' -NativeLibrary 'web3_network_arm64.so'

  Web3ScreenReader $true
  try {
    $null = Web3Run "the Gesher device: $a11yName (web3app.apk's app process)" ($launch + '--activity', 'com.gesher.web3app.A11yActivity') $a11yExpect 'Web3 a11y done' 'Web3 a11y ready for UI Automation' $uiaClient
  } finally { Web3ScreenReader $false }

  # Windows' proxy settings: a proxy server (the test proxy, which forwards what is not
  # its own), then a PAC script; the user's settings are restored whatever happens.
  [GesherWeb3Proxy]::Save()
  try {
    $bypass = '<local>;*.github.com;*.githubusercontent.com;*.actions.githubusercontent.com;*.blob.core.windows.net;*.visualstudio.com'
    if (-not [GesherWeb3Proxy]::SetServer("127.0.0.1:$web3Proxy", $bypass)) { Write-Host 'INFO  web3: InternetSetOption (proxy server) failed' }
    $null = Web3Run 'the Gesher device: web3app.apk''s NetActivity with Windows'' proxy server set (Internet Options): ConnectivityManager.getDefaultProxy, Proxy.getDefaultHost/Port and HttpURLConnection through it; the sticky CONNECTIVITY_ACTION; a PendingIntent network request' ($launch + '--activity', 'com.gesher.web3app.NetActivity') @(
      'Web3 net default proxy: ', "127.0.0.1",
      "Web3 net Proxy.getDefaultHost 127.0.0.1 port $web3Proxy",
      'Web3 net CONNECTIVITY_ACTION sticky: true',
      'Web3 net CONNECTIVITY_ACTION received: ', ' connected true',
      'Web3 net CONNECTIVITY_ACTION delivered true',
      'Web3 net PendingIntent request: ', ' for request true',
      'Web3 net per-network DNS resolved example.com true',
      'Web3 net bound SocketFactory kernel interface and source address true',
      'Web3 net explicit TCP and UDP socket binding true',
      'Web3 net Network.openConnection server observed the selected source address true',
      'Web3 net process socket and DNS binding, invalid network preservation, unbind true',
      'Web3 net missing-network DNS refused fallback true',
      'Web3 net NDK: PASS selected-network sockets/process/DNS, raw DNS poll/result/NXDOMAIN/cancel/errors',
      'raw DNS poll/result/NXDOMAIN/cancel/errors abi=x86_64',
      'Web3 net http://web3-proxied.test/net: 200 proxied by the test proxy for web3-proxied.test',
      'Web3 net done') 'Web3 net done'

    if (-not [GesherWeb3Proxy]::SetPac("http://127.0.0.1:$web3Http/proxy.pac")) { Write-Host 'INFO  web3: InternetSetOption (PAC) failed' }
    $before = ([GesherWeb3Servers]::Log -split "`n").Count - 1
    $null = Web3Run 'the Gesher device: web3app.apk''s NetActivity with a PAC script in Windows'' settings: a PAC default proxy, PacProxySelector answered by Windows'' evaluation of the script (one host through the proxy, the rest direct)' ($launch + '--activity', 'com.gesher.web3app.NetActivity', '--ez', 'pac', 'true') @(
      "pac http://127.0.0.1:$web3Http/proxy.pac",
      'Web3 net http://pac-proxied.test/net: 200 proxied by the test proxy for pac-proxied.test',
      'Web3 net direct under PAC: 200 direct from the test server',
      'Web3 net done') 'Web3 net done'
    $seen = ([GesherWeb3Servers]::Log -split "`n" | Select-Object -Skip $before) -join "`n"
    if ($seen.Contains('http GET /proxy.pac') -and -not $seen.Contains("proxy GET http://127.0.0.1:$web3Http/direct")) {
      Write-Host 'PASS  web3: the PAC script was fetched by Windows, and the URL it says DIRECT for did not go through the proxy'
    } else {
      Write-Host "FAIL  web3: the PAC script's direct URL (servers saw: $($seen -replace "`n", ' | '))"
      $failed++
    }
  } finally {
    [GesherWeb3Proxy]::Restore()
  }
} finally {
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'web3-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmWeb3) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  web3: the device shutdown timed out' }
  $env:ANDROID_DATA = $savedData
  $env:GESHER_LOGS = $savedLogs
  Remove-Item Env:GESHER_SHORTCUTS, Env:GESHER_WEB3_PORTS -ErrorAction SilentlyContinue
  [GesherWeb3Servers]::Stop()
}
([GesherWeb3Servers]::Log -split "`n" | Select-Object -First 40 | ForEach-Object { "INFO  web3 servers: $_" }) | Out-Host
if ($failed -eq $web3FailedBefore) { Write-Host 'INFO  web3: every check passed' }
