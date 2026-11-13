# The shell track (docs/installer.md): an installed app's label and launcher
# icon, as its Start menu shortcut gets them (com.gesher.app.shell.AppShell
# icon APK OUT.ico), checked as Windows reads the .ico. Dot-sourced by
# run-tests.ps1, whose NoDrive, $Dir, $root and $failed these use. The
# shortcuts themselves are checked against the installed Gesher, in the
# package job (.github/workflows/ci.yml).
. (Join-Path $PSScriptRoot '..\shell\ShellChecks.ps1')

$shellOut = Join-Path $root 'shell-icons'
New-Item -ItemType Directory -Force $shellOut | Out-Null
$bcpShell = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'

# lifecycle.apk: a legacy PNG mipmap, red only at xxxhdpi. components.apk: an
# adaptive icon, a WebP foreground (white only at xxxhdpi) over a blue color,
# in the icon mask. peer.apk has no icon: the framework's default.
$shellApps = @(
  @{ Apk = 'lifecycle'; Package = 'com.gesher.lifecycle'; Label = 'Gesher Lifecycle';
     Source = '^res/mipmap-xxxhdpi[-v0-9]*/ic_launcher\.png$';
     Pixels = @{ '2,2' = 'transparent'; '128,128' = @(0xD3, 0x2F, 0x2F); '128,40' = @(0xD3, 0x2F, 0x2F) } },
  @{ Apk = 'components'; Package = 'com.gesher.components'; Label = 'Gesher Components';
     Source = '^res/mipmap-anydpi[-v0-9]*/ic_launcher\.xml \(adaptive icon: background #ff1e63d6, foreground res/mipmap-xxxhdpi[-v0-9]*/ic_launcher_foreground\.webp \(\w+\)\)';
     Pixels = @{ '2,2' = 'transparent'; '128,128' = @(0xFF, 0xFF, 0xFF); '128,30' = @(0x1E, 0x63, 0xD6) } },
  @{ Apk = 'peer'; Package = 'com.gesher.peer'; Label = $null; Source = '^the framework''s default icon \(no icon\): '; Pixels = @{} }
)
foreach ($app in $shellApps) {
  $ico = Join-Path $shellOut "$($app.Apk).ico"
  $name = "shell: $($app.Apk).apk's label and launcher icon as a multi-size .ico"
  $out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpShell" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' com.gesher.app.shell.AppShell icon (NoDrive "$Dir/$($app.Apk).apk") (NoDrive $ico) 2>&1 | Out-String
  $code = $LASTEXITCODE
  $problems = @()
  if ($code -ne 0) { $problems += "exit $code" }
  $iconLine = ($out -split "`n" | Where-Object { $_ -match '^gesher-app: icon of ' } | Select-Object -First 1)
  $iconSource = if ($iconLine) { ($iconLine.Trim() -split ': ', 3)[2] } else { '' }
  if ($iconSource -notmatch $app.Source) { $problems += "the icon's source does not match $($app.Source)" }
  if ($app.Label -and -not $out.Contains("gesher-app: label of $($app.Package): $($app.Label)")) { $problems += "the label is not $($app.Label)" }
  if (-not $out.Contains("gesher-app: AppUserModelID of $($app.Package): Gesher.App.$($app.Package)")) { $problems += 'no AppUserModelID' }
  if (-not $problems) { $problems += @(Test-AppIco $ico $app.Pixels) }
  if (-not $problems) {
    Write-Host "PASS  $name ($iconSource)"
  } else {
    Write-Host "FAIL  $name ($($problems -join '; '))"
    Write-Host $out
    $failed++
  }
}

# gesher-launch.exe (compat/shell/gesher_launch.cc): what an app's shortcut, a
# double-clicked .apk and a toast click start. A copy runs a stand-in
# gesher-app.ps1 next to it: the arguments arrive as written, the script's exit
# code and output come back, and powershell runs with CREATE_NO_WINDOW (its
# console has no window: nothing flashes). It hands on the foreground
# (AllowSetForegroundWindow), so the app's window comes to the front.
$launchDir = Join-Path $shellOut 'launch'
New-Item -ItemType Directory -Force $launchDir | Out-Null
$launchName = 'shell: gesher-launch.exe runs gesher-app.ps1 with no console window, its arguments, output and exit code passed on'
$problems = @()
if (-not (Test-Path "$Dir/gesher-launch.exe")) {
  $problems += "no $Dir/gesher-launch.exe"
} else {
  Copy-Item "$Dir/gesher-launch.exe" $launchDir -Force
  $probeFile = Join-Path $launchDir 'probe.txt'
  Remove-Item $probeFile -ErrorAction SilentlyContinue
  Set-Content -LiteralPath (Join-Path $launchDir 'gesher-app.ps1') -Encoding utf8 -Value @'
Add-Type -Namespace GesherLaunchProbe -Name Native -MemberDefinition '[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();'
$window = [GesherLaunchProbe.Native]::GetConsoleWindow()
Set-Content -LiteralPath (Join-Path $PSScriptRoot 'probe.txt') -Encoding utf8 -Value @("console-window=$window", "args=$($args -join '|')")
'gesher-launch probe: output passed on'
exit 7
'@
  $psi = [Diagnostics.ProcessStartInfo]::new((Join-Path $launchDir 'gesher-launch.exe'), 'launch "com.example two words" tail')
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.WorkingDirectory = $launchDir
  $p = [Diagnostics.Process]::Start($psi)
  $launchErr = $p.StandardError.ReadToEndAsync()
  $launchOut = $p.StandardOutput.ReadToEndAsync()
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $problems += 'it did not exit within 120 s' }
  $p.WaitForExit()
  $launchText = $launchOut.Result + $launchErr.Result
  if ($p.ExitCode -ne 7) { $problems += "exit $($p.ExitCode), not the script's 7" }
  if (-not $launchText.Contains('gesher-launch probe: output passed on')) { $problems += "the script's output was not passed on ($launchText)" }
  $probe = if (Test-Path $probeFile) { Get-Content -LiteralPath $probeFile } else { @() }
  if ($probe -notcontains 'args=launch|com.example two words|tail') { $problems += "the script got: $($probe -join '; ')" }
  if ($probe -notcontains 'console-window=0') { $problems += "powershell's console has a window ($($probe -join '; ')): not CREATE_NO_WINDOW" }
  $launchBytes = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("$Dir/gesher-launch.exe"))
  if (-not $launchBytes.Contains('AllowSetForegroundWindow')) { $problems += 'gesher-launch.exe does not import AllowSetForegroundWindow (the app''s window would open behind Explorer)' }
}
if (-not $problems) {
  Write-Host "PASS  $launchName"
} else {
  Write-Host "FAIL  $launchName ($($problems -join '; '))"
  $failed++
}

# gesher-launch's splash window (compat/shell/gesher_launch.cc): a launch shows
# one (class GesherSplash) while the app starts, with the app's label and icon
# (its shortcut's, under %LOCALAPPDATA%\Gesher\data\data\<package>\gesher-shell)
# and AppUserModelID, until a visible window with that AppUserModelID is up.
# A copy runs a stand-in gesher-app.ps1 (Windows PowerShell) that looks for its
# parent's splash and, per $env:SPLASH_PROBE_MODE: 'app' shows a window with the
# app's AppUserModelID and sees the splash go; 'close' closes the splash (the
# launch is cancelled and its processes are terminated); 'none' expects no splash (install,
# --headless). A double-clicked APK's package comes from apk-icons.list.
$splashName = 'shell: gesher-launch shows a splash (the app''s label, icon and AppUserModelID) until the app''s window is up; closing it cancels the launch; none for install or --headless'
$problems = @()
if (-not (Test-Path "$Dir/gesher-launch.exe")) {
  $problems += "no $Dir/gesher-launch.exe"
} else {
  $splashDir = Join-Path $shellOut 'splash'
  $splashData = Join-Path $splashDir 'localappdata'
  $splashShell = Join-Path $splashData 'Gesher\data\data\com.gesher.splashprobe\gesher-shell'
  New-Item -ItemType Directory -Force $splashShell, (Join-Path $splashData 'Gesher\data\system') | Out-Null
  Copy-Item "$Dir/gesher-launch.exe" $splashDir -Force
  # As Properties.store writes it: ISO-8859-1, other characters \u-escaped.
  Set-Content -LiteralPath (Join-Path $splashShell 'shortcut.properties') -Encoding ascii -Value @(
    '#Gesher device', 'package=com.gesher.splashprobe', 'label=Splash Probe \u00e9', 'app_user_model_id=Gesher.App.com.gesher.splashprobe')
  $splashIco = Join-Path $shellOut 'lifecycle.ico'  # drawn above
  if (Test-Path $splashIco) { Copy-Item $splashIco (Join-Path $splashShell 'icon.ico') -Force }
  $probeApk = Join-Path $splashDir 'probe.apk'
  Set-Content -LiteralPath $probeApk -Value 'not an APK: the stand-in never reads it'
  $apkItem = Get-Item -LiteralPath $probeApk
  $apkMillis = [DateTimeOffset]::new($apkItem.LastWriteTimeUtc).ToUnixTimeMilliseconds()
  Set-Content -LiteralPath (Join-Path $splashData 'Gesher\data\system\apk-icons.list') -Encoding utf8NoBOM -Value (
    "$($apkItem.Length)`t$apkMillis`t$(Join-Path $splashShell 'icon.ico')`tcom.gesher.splashprobe")
  Set-Content -LiteralPath (Join-Path $splashDir 'gesher-app.ps1') -Encoding utf8 -Value @'
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SplashProbe {
  [StructLayout(LayoutKind.Sequential, Pack = 4)] struct PropertyKey { public Guid FormatId; public uint PropertyId; }
  [StructLayout(LayoutKind.Sequential)] struct PropVariant { public ushort vt; public ushort r1, r2, r3; public IntPtr p; public IntPtr p2; }
  [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IPropertyStore {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int GetAt(uint index, out PropertyKey key);
    [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
    [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
    [PreserveSig] int Commit();
  }
  [DllImport("shell32.dll")] static extern int SHGetPropertyStoreForWindow(IntPtr hwnd, ref Guid iid,
      [MarshalAs(UnmanagedType.Interface)] out IPropertyStore store);
  [DllImport("ole32.dll")] static extern int PropVariantClear(ref PropVariant value);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
  // The first window of class `cls` of process `pid`, or zero.
  public static IntPtr Find(string cls, uint pid) {
    IntPtr h = IntPtr.Zero;
    while ((h = FindWindowExW(IntPtr.Zero, h, cls, null)) != IntPtr.Zero) {
      uint owner;
      GetWindowThreadProcessId(h, out owner);
      if (owner == pid) return h;
    }
    return IntPtr.Zero;
  }
  public static string Title(IntPtr hwnd) {
    var s = new StringBuilder(512);
    GetWindowTextW(hwnd, s, s.Capacity);
    return s.ToString();
  }
  public static bool HasIcon(IntPtr hwnd) {
    return SendMessageW(hwnd, 0x007F, (IntPtr) 1, IntPtr.Zero) != IntPtr.Zero;  // WM_GETICON ICON_BIG
  }
  static PropertyKey AppIdKey() {
    return new PropertyKey { FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), PropertyId = 5 };
  }
  public static string AppUserModelId(IntPtr hwnd) {
    Guid iid = new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99");
    IPropertyStore store;
    int hr = SHGetPropertyStoreForWindow(hwnd, ref iid, out store);
    if (hr != 0) return "(HRESULT 0x" + hr.ToString("x8") + ")";
    try {
      var key = AppIdKey();
      PropVariant value;
      hr = store.GetValue(ref key, out value);
      if (hr != 0) return "(HRESULT 0x" + hr.ToString("x8") + ")";
      try {
        return value.vt == 31 ? Marshal.PtrToStringUni(value.p) : "(vt " + value.vt + ")";
      } finally {
        PropVariantClear(ref value);
      }
    } finally {
      Marshal.ReleaseComObject(store);
    }
  }
  public static int SetAppUserModelId(IntPtr hwnd, string id) {
    Guid iid = new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99");
    IPropertyStore store;
    int hr = SHGetPropertyStoreForWindow(hwnd, ref iid, out store);
    if (hr != 0) return hr;
    try {
      var key = AppIdKey();
      var value = new PropVariant { vt = 31, p = Marshal.StringToCoTaskMemUni(id) };  // VT_LPWSTR
      hr = store.SetValue(ref key, ref value);
      if (hr == 0) hr = store.Commit();
      PropVariantClear(ref value);
      return hr;
    } finally {
      Marshal.ReleaseComObject(store);
    }
  }
}
"@
$launcher = [uint32](Get-CimInstance Win32_Process -Filter "ProcessId = $PID").ParentProcessId
$mode = $env:SPLASH_PROBE_MODE
$lines = @("args=$($args -join '|')")
$splash = [IntPtr]::Zero
$tries = if ($mode -eq 'none') { 30 } else { 150 }
for ($i = 0; $i -lt $tries -and $splash -eq [IntPtr]::Zero; $i++) {
  $splash = [SplashProbe]::Find('GesherSplash', $launcher)
  if ($splash -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
}
$lines += "splash=$($splash -ne [IntPtr]::Zero)"
$code = 0
if ($splash -ne [IntPtr]::Zero) {
  $title = [SplashProbe]::Title($splash)
  $lines += "title=$title", "title-ok=$($title -eq "Splash Probe $([char]0xE9)")", "visible=$([SplashProbe]::IsWindowVisible($splash))",
    "icon=$([SplashProbe]::HasIcon($splash))", "aumid=$([SplashProbe]::AppUserModelId($splash))"
  if ($mode -eq 'app') {
    # Not yet: no window of the app is up.
    Start-Sleep -Milliseconds 1000
    $lines += "early=$([SplashProbe]::IsWindow($splash))"
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Splash probe app'
    $form.Width = 300; $form.Height = 300
    $null = $form.Handle
    $lines += "set-aumid=$([SplashProbe]::SetAppUserModelId($form.Handle, 'Gesher.App.com.gesher.splashprobe'))"
    $form.Show()
    # gesher-launch starts this powershell with SW_HIDE, which the first show takes.
    [void][SplashProbe]::ShowWindow($form.Handle, 5)  # SW_SHOW
    $gone = $false
    for ($i = 0; $i -lt 100; $i++) {
      [System.Windows.Forms.Application]::DoEvents()
      if (-not [SplashProbe]::IsWindow($splash)) { $gone = $true; break }
      Start-Sleep -Milliseconds 100
    }
    $lines += "gone=$gone"
    $form.Close()
  } elseif ($mode -eq 'close') {
    $lines += "close-hwnd=$($splash.ToInt64())", "worker-pid=$PID",
      "worker-start=$([Diagnostics.Process]::GetCurrentProcess().StartTime.ToFileTimeUtc())"
    Set-Content -LiteralPath (Join-Path $PSScriptRoot "splash-$mode.txt") -Encoding ascii -Value $lines
    if (-not [SplashProbe]::PostMessageW($splash, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) { exit 23 }
    Start-Sleep -Milliseconds 1000
    $code = 5
  } elseif ($mode -in 'late', 'late-cancel') {
    # A detached launcher: it ends (exit 0) once the activity has started, before
    # the app's window is up; splash-child.ps1 shows that window later.
    Set-Content -LiteralPath (Join-Path $PSScriptRoot "splash-$mode.txt") -Encoding ascii -Value $lines
    Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
      (Join-Path $PSScriptRoot 'splash-child.ps1'), $mode, $splash.ToInt64()
    exit 0
  }
}
Set-Content -LiteralPath (Join-Path $PSScriptRoot "splash-$mode.txt") -Encoding ascii -Value $lines
exit $code
'@
  # The app's window after a detached launcher ended: 'late' shows it 3 s later and
  # sees the splash, still up, go; 'late-cancel' closes the splash first, then shows
  # a window of the app, another 2 s later, and one of class GesherSplash with the
  # app's AppUserModelID (another launch's splash): gesher-launch closes the app's
  # windows as they come and leaves the other splash alone.
  Set-Content -LiteralPath (Join-Path $splashDir 'splash-child.ps1') -Encoding utf8 -Value @'
param([string] $mode, [long] $splashHandle)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SplashChild {
  [StructLayout(LayoutKind.Sequential, Pack = 4)] struct PropertyKey { public Guid FormatId; public uint PropertyId; }
  [StructLayout(LayoutKind.Sequential)] struct PropVariant { public ushort vt; public ushort r1, r2, r3; public IntPtr p; public IntPtr p2; }
  [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IPropertyStore {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int GetAt(uint index, out PropertyKey key);
    [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
    [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
    [PreserveSig] int Commit();
  }
  [DllImport("shell32.dll")] static extern int SHGetPropertyStoreForWindow(IntPtr hwnd, ref Guid iid,
      [MarshalAs(UnmanagedType.Interface)] out IPropertyStore store);
  [DllImport("ole32.dll")] static extern int PropVariantClear(ref PropVariant value);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
  delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct WndClass {
    public uint style; public WndProc proc; public int c1, c2; public IntPtr inst, icon, cursor, bg; public string menu, cls; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WndClass wc);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowExW(int ex, string cls, string title,
      uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
  static WndProc def = DefWindowProcW;
  public static int SetAppUserModelId(IntPtr hwnd, string id) {
    Guid iid = new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99");
    IPropertyStore store;
    int hr = SHGetPropertyStoreForWindow(hwnd, ref iid, out store);
    if (hr != 0) return hr;
    try {
      var key = new PropertyKey { FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), PropertyId = 5 };
      var value = new PropVariant { vt = 31, p = Marshal.StringToCoTaskMemUni(id) };
      hr = store.SetValue(ref key, ref value);
      if (hr == 0) hr = store.Commit();
      PropVariantClear(ref value);
      return hr;
    } finally {
      Marshal.ReleaseComObject(store);
    }
  }
  // A visible window of class GesherSplash, as another launch's splash.
  public static IntPtr OtherSplash() {
    var wc = new WndClass { proc = def, cls = "GesherSplash" };
    RegisterClassW(ref wc);
    return CreateWindowExW(0, "GesherSplash", "another launch", 0x10CF0000, 0, 0, 200, 200, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
  }
}
"@
$id = 'Gesher.App.com.gesher.splashprobe'
$splash = [IntPtr]$splashHandle
$lines = @()
function Show-App {
  $form = New-Object System.Windows.Forms.Form
  $form.Text = 'Splash probe app'
  $null = $form.Handle
  $script:lines += "set-aumid=$([SplashChild]::SetAppUserModelId($form.Handle, $id))"
  $form.Show()
  [void][SplashChild]::ShowWindow($form.Handle, 5)  # started with SW_HIDE
  return $form
}
$other = [IntPtr]::Zero
if ($mode -eq 'late-cancel') {
  Start-Sleep -Milliseconds 800
  $lines += "splash-after-exit=$([SplashChild]::IsWindow($splash))"
  [void][SplashChild]::PostMessageW($splash, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1500
  $other = [SplashChild]::OtherSplash()
  [void][SplashChild]::SetAppUserModelId($other, $id)
} else {
  Start-Sleep 3
  $lines += "splash-after-exit=$([SplashChild]::IsWindow($splash))"
}
$forms = @(Show-App)
$sw = [Diagnostics.Stopwatch]::StartNew()
$gone = $false
while ($sw.Elapsed.TotalSeconds -lt 12) {
  [System.Windows.Forms.Application]::DoEvents()
  if (-not $gone -and $sw.Elapsed.TotalSeconds -lt 5) { $gone = -not [SplashChild]::IsWindow($splash) }
  if ($mode -eq 'late' -and $gone) { break }
  if ($mode -eq 'late-cancel' -and $forms.Count -eq 1 -and $sw.Elapsed.TotalSeconds -ge 2) { $forms += Show-App }
  Start-Sleep -Milliseconds 50
}
if ($mode -eq 'late') { $lines += "gone=$gone" }
$closed = @($forms | Where-Object { $_.IsDisposed -or -not $_.Visible }).Count
$lines += "app-closed=$closed/$($forms.Count)"
if ($other -ne [IntPtr]::Zero) { $lines += "other-splash-kept=$([SplashChild]::IsWindow($other))" }
foreach ($form in $forms) { if (-not $form.IsDisposed) { $form.Close() } }
Add-Content -LiteralPath (Join-Path $PSScriptRoot "splash-$mode.txt") -Encoding ascii -Value ($lines + 'child-done=True')
'@
  if (-not ('SplashLifetimeProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SplashLifetimeProbe {
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
}
'@
  }
  $cases = @(
    @{ Mode = 'app'; Args = 'launch com.gesher.splashprobe'; Code = 0 },
    @{ Mode = 'app'; Args = "`"$probeApk`""; Code = 0 },
    @{ Mode = 'close'; Args = 'launch com.gesher.splashprobe'; Code = 1 },
    @{ Mode = 'none'; Args = "install `"$probeApk`""; Code = 0 },
    @{ Mode = 'none'; Args = 'launch com.gesher.splashprobe --headless'; Code = 0 },
    @{ Mode = 'late'; Args = 'launch com.gesher.splashprobe'; Code = 0; Detach = $true },
    @{ Mode = 'late-cancel'; Args = 'launch com.gesher.splashprobe'; Code = 1; Detach = $true }
  )
  foreach ($case in $cases) {
    $probeOut = Join-Path $splashDir "splash-$($case.Mode).txt"
    Remove-Item $probeOut -ErrorAction SilentlyContinue
    $psi = [Diagnostics.ProcessStartInfo]::new((Join-Path $splashDir 'gesher-launch.exe'), $case.Args)
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $splashDir
    $psi.EnvironmentVariables['LOCALAPPDATA'] = $splashData
    $psi.EnvironmentVariables['SPLASH_PROBE_MODE'] = $case.Mode
    foreach ($v in 'GESHER_APP_WINDOW', 'GESHER_SPLASH', 'GESHER_DISPLAY', 'GESHER_LAUNCH_DETACH') { $psi.EnvironmentVariables.Remove($v) }
    # A detached launch (as with no output handles: the Start menu's).
    if ($case.Detach) { $psi.EnvironmentVariables['GESHER_LAUNCH_DETACH'] = '1' }
    $p = [Diagnostics.Process]::Start($psi)
    $what = "gesher-launch $($case.Args) ($($case.Mode))"
    if (-not $p.WaitForExit(120000)) { $p.Kill($true); $problems += "$($what): did not exit within 120 s"; continue }
    if ($p.ExitCode -ne $case.Code) { $problems += "$($what): exit $($p.ExitCode), not $($case.Code)" }
    # The child that shows the app's window reports last.
    for ($i = 0; $case.Detach -and $i -lt 300; $i++) {
      if ((Test-Path $probeOut) -and (Get-Content -LiteralPath $probeOut) -contains 'child-done=True') { break }
      Start-Sleep -Milliseconds 100
    }
    $probe = if (Test-Path $probeOut) { @(Get-Content -LiteralPath $probeOut) } else { @() }
    if ($case.Mode -eq 'close') {
      # The cancelled child cannot report its own retirement; observe it here.
      $closing = @($probe | Where-Object { $_ -match '^close-hwnd=\d+$' })
      $worker = @($probe | Where-Object { $_ -match '^worker-pid=\d+$' })
      $birth = @($probe | Where-Object { $_ -match '^worker-start=\d+$' })
      if ($closing.Count -eq 1 -and $worker.Count -eq 1 -and $birth.Count -eq 1) {
        $handle = [IntPtr][long]$closing[0].Split('=')[1]
        $child = Get-Process -Id ([int]$worker[0].Split('=')[1]) -ErrorAction SilentlyContinue
        $retired = -not $child
        if ($child) {
          try { $retired = $child.HasExited -or $child.StartTime.ToFileTimeUtc() -ne [long]$birth[0].Split('=')[1] }
          catch { $retired = $child.HasExited }
        }
        $probe += "gone=$(-not [SplashLifetimeProbe]::IsWindow($handle))", "worker-retired=$retired"
      }
    }
    Write-Host "$($what): $($probe -join '; ')"
    $want = switch ($case.Mode) {
      'app' { 'splash=True', 'title-ok=True', 'visible=True', 'aumid=Gesher.App.com.gesher.splashprobe', 'early=True', 'set-aumid=0', 'gone=True' }
      'close' { 'splash=True', 'gone=True', 'worker-retired=True' }
      'late' { 'splash=True', 'splash-after-exit=True', 'set-aumid=0', 'gone=True', 'app-closed=0/1', 'child-done=True' }
      'late-cancel' { 'splash=True', 'splash-after-exit=True', 'app-closed=2/2', 'other-splash-kept=True', 'child-done=True' }
      'none' { 'splash=False' }
    }
    if ($case.Mode -ne 'none' -and (Test-Path (Join-Path $splashShell 'icon.ico'))) { $want += 'icon=True' }
    foreach ($w in $want) { if ($probe -notcontains $w) { $problems += "$($what): no '$w' ($($probe -join '; '))" } }
  }
}
if (-not $problems) {
  Write-Host "PASS  $splashName"
} else {
  Write-Host "FAIL  $splashName ($($problems -join '; '))"
  $failed++
}

# gesher_shellext.dll (compat/shell/gesher_shellext.cc), Gesher.Apk's icon
# handler, called as Explorer calls it (DllGetClassObject, IPersistFile::Load,
# IExtractIconW::GetIconLocation) on a copy away from gesher-launch.exe (so it
# draws nothing): an APK recorded in apk-icons.list (its size and last write
# time; under %LOCALAPPDATA%\Gesher\data\system) is named with its app's .ico,
# also as a copy (same size and time); an empty file gets gesher.ico, next to
# the DLL.
$extName = 'shell: the .apk icon handler names a recorded APK''s app icon (also for a copy), gesher.ico for others'
$problems = @()
if (-not (Test-Path "$Dir/gesher_shellext.dll")) {
  $problems += "no $Dir/gesher_shellext.dll"
} else {
  if (-not ('GesherShellExtProbe.Handler' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace GesherShellExtProbe {
  public static class Handler {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)]
    static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int GetClassObjectFn(ref Guid clsid, ref Guid iid, out IntPtr ppv);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int QueryInterfaceFn(IntPtr self, ref Guid iid, out IntPtr ppv);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate uint ReleaseFn(IntPtr self);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int CreateInstanceFn(IntPtr self, IntPtr outer, ref Guid iid, out IntPtr ppv);
    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Unicode)]
    delegate int LoadFn(IntPtr self, string name, uint mode);
    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Unicode)]
    delegate int GetIconLocationFn(IntPtr self, uint flags, StringBuilder file, uint cch, out int index, out uint outFlags);

    static T Slot<T>(IntPtr obj, int slot) where T : Delegate {
      IntPtr vtable = Marshal.ReadIntPtr(obj);
      return Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(vtable, slot * IntPtr.Size));
    }

    static void Release(IntPtr obj) { if (obj != IntPtr.Zero) Slot<ReleaseFn>(obj, 2)(obj); }

    // The icon file the handler names for `apk`, and its GIL_* flags; throws on a failed call.
    public static string IconOf(string dll, string apk, out uint flags) {
      IntPtr module = LoadLibraryW(dll);
      if (module == IntPtr.Zero) throw new Exception("LoadLibrary " + dll + ": " + Marshal.GetLastWin32Error());
      IntPtr entry = GetProcAddress(module, "DllGetClassObject");
      if (entry == IntPtr.Zero) throw new Exception("no DllGetClassObject");
      Guid clsid = new Guid("6B0C3C9E-2D1B-4E57-9B7A-3F2C8E4D1A50");
      Guid iidFactory = new Guid("00000001-0000-0000-C000-000000000046");
      Guid iidPersistFile = new Guid("0000010B-0000-0000-C000-000000000046");
      Guid iidExtractIcon = new Guid("000214FA-0000-0000-C000-000000000046");
      IntPtr factory = IntPtr.Zero, persist = IntPtr.Zero, extract = IntPtr.Zero;
      try {
        int hr = Marshal.GetDelegateForFunctionPointer<GetClassObjectFn>(entry)(ref clsid, ref iidFactory, out factory);
        if (hr != 0) throw new Exception("DllGetClassObject: 0x" + hr.ToString("X8"));
        hr = Slot<CreateInstanceFn>(factory, 3)(factory, IntPtr.Zero, ref iidPersistFile, out persist);
        if (hr != 0) throw new Exception("CreateInstance: 0x" + hr.ToString("X8"));
        hr = Slot<LoadFn>(persist, 5)(persist, apk, 0);
        if (hr != 0) throw new Exception("IPersistFile::Load: 0x" + hr.ToString("X8"));
        hr = Slot<QueryInterfaceFn>(persist, 0)(persist, ref iidExtractIcon, out extract);
        if (hr != 0) throw new Exception("QueryInterface(IExtractIconW): 0x" + hr.ToString("X8"));
        StringBuilder file = new StringBuilder(1040);
        int index;
        hr = Slot<GetIconLocationFn>(extract, 3)(extract, 0, file, (uint)file.Capacity, out index, out flags);
        if (hr != 0) throw new Exception("GetIconLocation: 0x" + hr.ToString("X8"));
        return file.ToString();
      } finally {
        Release(extract);
        Release(persist);
        Release(factory);
      }
    }

    [DllImport("kernel32.dll")]
    static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr GetModuleHandleW(string name);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int CanUnloadFn();

    // DllCanUnloadNow's answer, and whether the DLL is still mapped after a
    // FreeLibrary of the caller's own reference: Explorer must never lose it
    // under a thread still running in it.
    public static string Pinned(string dll) {
      IntPtr module = LoadLibraryW(dll);
      if (module == IntPtr.Zero) throw new Exception("LoadLibrary " + dll + ": " + Marshal.GetLastWin32Error());
      IntPtr entry = GetProcAddress(module, "DllCanUnloadNow");
      if (entry == IntPtr.Zero) throw new Exception("no DllCanUnloadNow");
      int hr = Marshal.GetDelegateForFunctionPointer<CanUnloadFn>(entry)();
      // Every reference this process holds (IconOf's LoadLibraryW calls were
      // never freed): an unpinned DLL is unmapped by the last one, a pinned one
      // ignores them all.
      for (int i = 0; i < 64; i++) FreeLibrary(module);
      bool mapped = GetModuleHandleW(System.IO.Path.GetFileName(dll)) != IntPtr.Zero;
      return "DllCanUnloadNow=" + hr + ";mapped=" + mapped;
    }
  }
}
'@
  }
  $extDir = Join-Path $shellOut 'shellext'
  $extData = Join-Path $shellOut 'localappdata'
  $extList = Join-Path $extData 'Gesher\data\system\apk-icons.list'
  New-Item -ItemType Directory -Force $extDir, (Split-Path $extList) | Out-Null
  Copy-Item "$Dir/gesher_shellext.dll" $extDir -Force
  $recorded = Join-Path $shellOut 'recorded.apk'
  $copy = Join-Path $shellOut 'recorded-copy.apk'
  $empty = Join-Path $shellOut 'empty.apk'
  $appIco = Join-Path $shellOut 'recorded-app.ico'
  [IO.File]::WriteAllBytes($recorded, [byte[]](0x50, 0x4B, 0x05, 0x06) + [byte[]]::new(18))
  [IO.File]::WriteAllBytes($empty, [byte[]]::new(0))
  [IO.File]::WriteAllBytes($appIco, [byte[]](0, 0, 1, 0, 0, 0))
  $when = [DateTime]::new(2026, 1, 2, 3, 4, 5, 678, [DateTimeKind]::Utc)
  [IO.File]::SetLastWriteTimeUtc($recorded, $when)
  Copy-Item $recorded $copy -Force
  [IO.File]::SetLastWriteTimeUtc($copy, $when)
  $millis = ([DateTimeOffset]$when).ToUnixTimeMilliseconds()
  $size = (Get-Item $recorded).Length
  # As AppShell.rememberApkQuietly writes it: size, time, .ico, package.
  [IO.File]::WriteAllText($extList, "$size`t$millis`t$appIco`tcom.example.recorded`n", [Text.UTF8Encoding]::new($false))
  $savedLocal = $env:LOCALAPPDATA
  $env:LOCALAPPDATA = $extData
  try {
    $dll = Join-Path $extDir 'gesher_shellext.dll'
    $flags = [uint32]0
    $icon = [GesherShellExtProbe.Handler]::IconOf($dll, $recorded, [ref]$flags)
    if ($icon -ne $appIco) { $problems += "the recorded APK's icon is '$icon', not $appIco" }
    if (($flags -band 0x2) -eq 0) { $problems += "the recorded APK's icon is not per instance (flags $flags)" }
    $icon = [GesherShellExtProbe.Handler]::IconOf($dll, $copy, [ref]$flags)
    if ($icon -ne $appIco) { $problems += "the copy's icon is '$icon', not $appIco" }
    $icon = [GesherShellExtProbe.Handler]::IconOf($dll, $empty, [ref]$flags)
    if ($icon -ne (Join-Path $extDir 'gesher.ico')) { $problems += "the empty file's icon is '$icon', not gesher.ico next to the DLL" }
    # Never unloaded: an object's destructor lowers the count before its Release
    # returns, so DllCanUnloadNow's S_OK could let COM unmap the DLL under an
    # Explorer thread still in it (it crashed Explorer). Must stay mapped, last.
    $pin = [GesherShellExtProbe.Handler]::Pinned($dll)
    if ($pin -ne 'DllCanUnloadNow=1;mapped=True') { $problems += "the DLL can be unloaded ($pin), not DllCanUnloadNow=1 (S_FALSE) and still mapped" }
  } catch {
    $problems += "$_"
  } finally {
    $env:LOCALAPPDATA = $savedLocal
  }
}
if (-not $problems) {
  Write-Host "PASS  $extName"
} else {
  Write-Host "FAIL  $extName ($($problems -join '; '))"
  $failed++
}
