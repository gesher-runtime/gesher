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
