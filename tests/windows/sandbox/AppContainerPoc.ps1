# AppContainer isolation, milestone M1 (docs/decisions/0090-appcontainer-app-isolation.md,
# docs/sandbox.md): a Gesher device with GESHER_SANDBOX=appcontainer installs and
# launches an APK; its app process must run in the package's own AppContainer.
#
#   pwsh tests/windows/sandbox/AppContainerPoc.ps1 -Dir <test layout> [-Root <scratch>] [-Apk <apk>]
#
# -Dir is a test layout as tests/windows/run-tests.ps1 takes it (dalvikvm.exe,
# binderd.exe, servicemanager.exe, the boot jars, framework.jar, gesher-app.jar,
# framework-res.apk, lifecycle.apk). The script reports what it observed; it is a
# proof of concept, not a gate: docs/sandbox.md records its results.
param(
  [Parameter(Mandatory)][string] $Dir,
  [string] $Root = (Join-Path ([IO.Path]::GetTempPath()) "gesher-sandbox-$PID"),
  [string] $Apk = '',
  [int] $Seconds = 600,
  # The same run without the sandbox: a baseline for the layout under test.
  [switch] $NoSandbox,
  # Supply an HWND for APK fixtures that initialize the real WebView2 controller.
  [switch] $HiddenWindow,
  [string[]] $LaunchArguments = @()
)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path $Dir).Path
$Root = [IO.Path]::GetFullPath($Root)
if ([IO.Path]::GetPathRoot($Dir) -ne [IO.Path]::GetPathRoot($Root)) {
  throw 'Use a scratch profile on the same drive as the runtime for drive-less ART paths.'
}
if (-not $Apk) { $Apk = Join-Path $Dir 'lifecycle.apk' }
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }

foreach ($d in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata', 'logs') {
  New-Item -ItemType Directory -Force (Join-Path $Root $d) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $Root 'i18n/etc/icu')
if (Test-Path "$Dir/tzdata") { Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $Root 'tzdata') }
Copy-Item "$Dir/framework-res.apk" (Join-Path $Root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $Root 'system')
if (Test-Path "$Dir/fonts/fonts.xml") {
  foreach ($d in 'system/etc','system/fonts') {
    New-Item -ItemType Directory -Force (Join-Path $Root $d) | Out-Null
  }
  Copy-Item "$Dir/fonts/*.xml" (Join-Path $Root 'system/etc')
  Copy-Item "$Dir/fonts/*.ttf" (Join-Path $Root 'system/fonts')
}
if (Test-Path "$Dir/framework/x86_64/boot.art") {
  New-Item -ItemType Directory -Force (Join-Path $Root 'system/framework/x86_64') | Out-Null
  Copy-Item "$Dir/framework/x86_64/*" (Join-Path $Root 'system/framework/x86_64')
}

$env:ANDROID_ROOT = NoDrive "$Root/system"
$env:ANDROID_ART_ROOT = NoDrive "$Root/art"
$env:ANDROID_DATA = NoDrive "$Root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$Root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$Root/tzdata"
$env:GESHER_LOGS = NoDrive "$Root/logs"
$env:GESHER_BINDER = "sandbox-$PID"
$env:GESHER_LOCALES = 'en-US'
$env:GESHER_APP_WINDOW = 'headless'
$env:GESHER_SHORTCUTS = '0'
if ($NoSandbox) { Remove-Item Env:GESHER_SANDBOX -ErrorAction SilentlyContinue } else { $env:GESHER_SANDBOX = 'appcontainer' }

$bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$launcher = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$Root/system")/framework/boot.art",
  '-Xint', '-Xusejit:false', 'com.gesher.app.Launcher')

# The token of a running process: whether it is an AppContainer, and its SID.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherToken {
  [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(int a, bool i, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, int a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr t, int c, IntPtr b, int n, out int r);
  [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool ConvertSidToStringSidW(IntPtr sid, out IntPtr s);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
  public static string Describe(int pid) {
    IntPtr p = OpenProcess(0x1000, false, pid);  // PROCESS_QUERY_LIMITED_INFORMATION
    if (p == IntPtr.Zero) return "open failed: " + Marshal.GetLastWin32Error();
    IntPtr t;
    if (!OpenProcessToken(p, 8, out t)) { CloseHandle(p); return "token failed: " + Marshal.GetLastWin32Error(); }
    IntPtr b = Marshal.AllocHGlobal(4096);
    try {
      int r;
      if (!GetTokenInformation(t, 29, b, 4096, out r)) return "TokenIsAppContainer failed";  // TokenIsAppContainer
      if (Marshal.ReadInt32(b) == 0) return "not an AppContainer";
      if (!GetTokenInformation(t, 31, b, 4096, out r)) return "TokenAppContainerSid failed";  // TokenAppContainerSid
      IntPtr s;
      ConvertSidToStringSidW(Marshal.ReadIntPtr(b), out s);
      string sid = Marshal.PtrToStringUni(s);
      LocalFree(s);
      if (!GetTokenInformation(t, 25, b, 4096, out r)) return "AppContainer " + sid;  // TokenIntegrityLevel
      IntPtr label = Marshal.ReadIntPtr(b);
      int count = Marshal.ReadByte(label, 1);
      int rid = Marshal.ReadInt32(label, 8 + 4 * (count - 1));
      return "AppContainer " + sid + ", integrity 0x" + rid.ToString("x");
    } finally { Marshal.FreeHGlobal(b); CloseHandle(t); CloseHandle(p); }
  }
}
'@

function Run([string[]] $Arguments) {
  # Windows PowerShell 5.1 has no ArgumentList: quote each argument for CommandLineToArgvW.
  $psi = New-Object Diagnostics.ProcessStartInfo "$Dir/dalvikvm.exe"
  $psi.Arguments = ($Arguments | ForEach-Object {
    if ($_ -match '[\s"]') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ }
  }) -join ' '
  # Drive-less paths (ADR 0010) are on the current drive: the layout's.
  $psi.WorkingDirectory = $Dir
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $out = $p.StandardOutput.ReadToEndAsync()
  $err = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); "(timed out)" }
  $out.Result + $err.Result
  "(exit $($p.ExitCode))"
}

# The app's process, watched while the launch runs: its token is read while it lives.
$watch = [Collections.Concurrent.ConcurrentDictionary[int, string]]::new()
$watcher = [PowerShell]::Create()
[void]$watcher.AddScript({
  param($watch, $layout)
  for ($i = 0; $i -lt 6000; $i++) {
    foreach ($p in Get-CimInstance Win32_Process -Filter "Name = 'dalvikvm.exe'" -ErrorAction SilentlyContinue) {
      # This layout's app processes only: other Gesher installations may run too.
      if ($p.CommandLine -match 'GesherAppProcess' -and $p.CommandLine.Contains($layout) -and
          -not $watch.ContainsKey([int]$p.ProcessId)) {
        $watch[[int]$p.ProcessId] = [GesherToken]::Describe([int]$p.ProcessId)
      }
    }
    Start-Sleep -Milliseconds 100
  }
}).AddArgument($watch).AddArgument((NoDrive $Dir))
$async = $watcher.BeginInvoke()

$failure = $null
try {
  Write-Host "== install $Apk"
  $installOutput = @(Run ($launcher + 'install', (NoDrive $Apk)))
  $installOutput | Write-Host
  if ($HiddenWindow -and $installOutput[-1] -ne '(exit 0)') {
    $failure = 'APK install failed'
  } else {
    Write-Host "== launch"
    $launch = $launcher + 'launch', (NoDrive $Apk)
    if ($HiddenWindow) { $launch += '--hidden-window', '--wait' }
    $launch += $LaunchArguments
    $launchOutput = @(Run $launch)
    $launchOutput | Write-Host
    if ($HiddenWindow -and $launchOutput[-1] -ne '(exit 0)') { $failure = 'APK launch failed' }
  }
} finally {
  Write-Host "== shutdown"
  Run ($launcher + 'shutdown') | Write-Host
  $watcher.Stop()
  $watcher.Dispose()
}

Write-Host "== app processes seen"
foreach ($k in $watch.Keys) { Write-Host "pid $k : $($watch[$k])" }
Write-Host "== logs ($Root/logs)"
foreach ($f in Get-ChildItem (Join-Path $Root 'logs') -File -ErrorAction SilentlyContinue) {
  Write-Host "--- $($f.Name)"
  Get-Content $f.FullName -Tail 80 | Write-Host
}
if ($failure) { throw $failure }
