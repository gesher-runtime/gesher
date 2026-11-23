# AppContainer isolation, milestone M1 (docs/decisions/0090-appcontainer-app-isolation.md,
# docs/sandbox.md): a Gesher device with GESHER_SANDBOX=appcontainer installs and
# launches an APK; its app process must run in the package's own AppContainer.
#
#   pwsh tests/windows/sandbox/AppContainerPoc.ps1 -Dir <test layout> [-Root <scratch>] [-Apk <apk>]
#   Add -Protected -NativeChild to verify enrolled APK and native process tokens.
#
# -Dir is a test layout as tests/windows/run-tests.ps1 takes it (dalvikvm.exe,
# binderd.exe, servicemanager.exe, the boot jars, framework.jar, gesher-app.jar,
# framework-res.apk, lifecycle.apk). The script reports what it observed; it is a
# proof of concept, not a gate: docs/sandbox.md records its results.
# -Scoped starts a device for this invocation and reads the original APK.
# -AlongsidePersistent verifies an existing device registration remains live.
param(
  [Parameter(Mandatory)][string] $Dir,
  [string] $Root = (Join-Path ([IO.Path]::GetTempPath()) "gesher-sandbox-$PID"),
  [string] $Apk = '',
  [int] $Seconds = 600,
  # The same run without the sandbox: a baseline for the layout under test.
  [switch] $NoSandbox,
  [switch] $Protected,
  [switch] $NativeChild,
  [switch] $Scoped,
  [switch] $AlongsidePersistent
)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path $Dir).Path
$defaultApk = !$Apk
if (-not $Apk) { $Apk = Join-Path $Dir 'lifecycle.apk' }
if (($Protected -and $NoSandbox) -or (($NativeChild -or $Scoped) -and !$Protected)) {
  throw 'Use -NativeChild and -Scoped only with -Protected, without -NoSandbox.'
}
if ($AlongsidePersistent -and !$Scoped) { throw 'Use -AlongsidePersistent only with -Scoped.' }
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }

foreach ($d in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata', 'logs') {
  New-Item -ItemType Directory -Force (Join-Path $Root $d) | Out-Null
}
$Root = (Resolve-Path $Root).Path
Copy-Item "$Dir/icudt*l.dat" (Join-Path $Root 'i18n/etc/icu')
if (Test-Path "$Dir/tzdata") { Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $Root 'tzdata') }
Copy-Item "$Dir/framework-res.apk" (Join-Path $Root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $Root 'system')
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
if ($NoSandbox) {
  Remove-Item Env:GESHER_SANDBOX -ErrorAction SilentlyContinue
} elseif ($Protected) {
  $env:GESHER_SANDBOX = 'appcontainer-trusted'
  $env:GESHER_SHORTCUTS = '0'
  $env:GESHER_MICROG_SPOOF = '0'
} else {
  $env:GESHER_SANDBOX = 'appcontainer'
}

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

$exitCodes = [Collections.Generic.List[int]]::new()
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
  $exitCodes.Add($p.ExitCode)
  $p.Dispose()
}

# The app's process, watched while the launch runs: its token is read while it lives.
$watch = [Collections.Concurrent.ConcurrentDictionary[int, string]]::new()
$watcher = [PowerShell]::Create()
[void]$watcher.AddScript({
  param($watch, $layout, $device, $observeChild)
  $filter = "Name = 'dalvikvm.exe'"
  if ($observeChild) { $filter += " OR Name = 'gesher-exec.exe'" }
  for ($i = 0; $i -lt 6000; $i++) {
    foreach ($p in Get-CimInstance Win32_Process -Filter $filter -ErrorAction SilentlyContinue) {
      if (!$p.CommandLine -or $watch.ContainsKey([int]$p.ProcessId)) { continue }
      $command = $p.CommandLine.Replace('\', '/')
      if (!$command.Contains($layout)) { continue }
      if ($p.Name -eq 'dalvikvm.exe' -and $command -match 'GesherAppProcess' -and
          $command.Contains($device)) {
        $watch[[int]$p.ProcessId] = 'app: ' + [GesherToken]::Describe([int]$p.ProcessId)
      } elseif ($observeChild -and $p.Name -eq 'gesher-exec.exe' -and
          $command -match '--tool sleep' -and $watch.ContainsKey([int]$p.ParentProcessId)) {
        $watch[[int]$p.ProcessId] = 'child: ' + [GesherToken]::Describe([int]$p.ProcessId)
      }
    }
    Start-Sleep -Milliseconds 100
  }
}).AddArgument($watch).AddArgument((NoDrive $Dir)).AddArgument((NoDrive $Root)).AddArgument($NativeChild)
$async = $watcher.BeginInvoke()

try {
  $state = "$Root/data/system/gesher-device"
  if ($AlongsidePersistent) {
    Run ($launcher + 'start') | Write-Host
    $persistentState = (Get-FileHash -LiteralPath $state).Hash
  }
  if (!$Scoped) {
    Write-Host "== install $Apk"
    Run ($launcher + 'install', (NoDrive $Apk)) | Write-Host
  } else {
    $originalAcl = (Get-Acl -LiteralPath $Apk).Sddl
    $originalHash = (Get-FileHash -LiteralPath $Apk).Hash
  }
  Write-Host "== launch"
  $launchArguments = if ($Scoped) { $launcher + 'launch-scoped', '--headless' } else { $launcher + 'launch' }
  if ($NativeChild) {
    $launchArguments += '--ez', 'com.gesher.lifecycle.RUN_NATIVE_CHILD', 'true'
  }
  $launchOutput = Run ($launchArguments + (NoDrive $Apk))
  $launchOutput | Write-Host
  if ($AlongsidePersistent) {
    if ($persistentState -ne (Get-FileHash -LiteralPath $state).Hash) {
      throw 'The scoped launch changed the existing persistent device registration.'
    }
    Run ($launcher + 'status') | Write-Host
  }
  if (!$Scoped -and $Protected -and $defaultApk) {
    Run ($launcher + 'uninstall', 'com.gesher.lifecycle') | Write-Host
  }
} finally {
  if (!$Scoped -or $AlongsidePersistent) {
    Write-Host "== shutdown"
    Run ($launcher + 'shutdown') | Write-Host
  }
  $watcher.Stop()
  $watcher.Dispose()
}

Write-Host "== app processes seen"
foreach ($k in $watch.Keys) { Write-Host "pid $k : $($watch[$k])" }
Write-Host "== logs ($Root/logs)"
foreach ($f in Get-ChildItem (Join-Path $Root 'logs') -File -Filter '*.log' -Recurse -ErrorAction SilentlyContinue) {
  Write-Host "--- $($f.Name)"
  Get-Content $f.FullName -Tail 80 | Write-Host
}
if ($Protected) {
  if ($exitCodes | Where-Object { $_ -ne 0 }) { throw "Protected command failed: $exitCodes" }
  if (($defaultApk -or $NativeChild) -and ($launchOutput -join "`n") -notmatch 'LifecycleActivity onResume') {
    throw 'The contained fixture did not reach Activity.onResume.'
  }
  $apps = @($watch.Values | Where-Object { $_ -match '^app: AppContainer (S-[^,]+)' })
  if (!$apps) { throw 'The Windows app token was not observed.' }
  if ($NativeChild) {
    $children = @($watch.Values | Where-Object { $_ -match '^child: AppContainer (S-[^,]+)' })
    if (!$children -or ($launchOutput -join "`n") -notmatch 'native child UID \d+ and Binder verified') {
      throw 'The contained native child did not complete its process and Binder checks.'
    }
    if ($children | Where-Object { $apps -notcontains ($_ -replace '^child:', 'app:') }) {
      throw 'The native child and its application used different Windows AppContainers.'
    }
  }
  if ($Scoped) {
    if ($originalAcl -ne (Get-Acl -LiteralPath $Apk).Sddl -or
        $originalHash -ne (Get-FileHash -LiteralPath $Apk).Hash) {
      throw 'The scoped launch altered its source APK.'
    }
    if (Test-Path "$Root/data/system/gesher-device") {
      throw 'The scoped launch wrote a persistent device registration.'
    }
  }
}
