# End-to-end: the Gesher installer runs real, unmodified open-source Android apps
# (docs/e2e.md). For each app of tests/e2e/apps.json (F-Droid, pinned by version and
# SHA-256): download it, `gesher-app install` it, launch it on the Gesher device with an
# off-desktop window, wait for its first frame, check the window is not blank (PrintWindow:
# non-background and text pixels), save a PNG, post input (a click, keys) and check the
# window changes, close it (WM_CLOSE) and check the app's process ends. One app is also
# started through the .apk association's registered command and the Start menu shortcut's
# command line. Prints one PASS or FAIL line per check; a FAIL carries the first exception
# of the app's log. Apps with `knownFailure` print KNOWN-FAIL and do not fail the run.
#
#   pwsh tests/e2e/run-e2e.ps1 -Setup installer\gesher-setup-*.exe -Out e2e-results
#
# Run by the e2e job of .github/workflows/ci.yml on windows-2022.
param(
  [Parameter(Mandatory)] [string] $Setup,
  [string] $Manifest = (Join-Path $PSScriptRoot 'apps.json'),
  [Alias('Out')] [string] $OutDir = 'e2e-results',
  [string[]] $Only = @(),
  [switch] $SkipInstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'tests\windows\graphics\WindowChecks.ps1')
Add-Type -AssemblyName System.Drawing

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
Set-Content $summary ''
$script:failed = 0
$script:lines = @()

function Report([string] $line) {
  Write-Host $line
  Add-Content $summary $line
  $script:lines += $line
  if ($env:GITHUB_STEP_SUMMARY) { Add-Content $env:GITHUB_STEP_SUMMARY $line }
}

# The first lines that look like the reason an app failed: an uncaught exception, a
# native crash, the runtime aborting, the launcher's own failure.
function FirstProblem([string] $text) {
  if (-not $text) { return '(no log)' }
  $lines = $text -split "`r?`n"
  $patterns = 'FATAL EXCEPTION', 'Uncaught|uncaught', '^\S*(Exception|Error)\b', '(Exception|Error): ', 'Abort message|SIGSEGV|signal \d+|Unhandled exception|access violation|fault at',
              'Runtime aborting|art::Runtime::Abort|Check failed', 'gesher-app: failed'
  foreach ($p in $patterns) {
    for ($i = 0; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match $p -and $lines[$i] -notmatch 'stand-in .* not implemented') {
        $found = @($lines[$i].Trim())
        for ($j = $i + 1; $j -lt [Math]::Min($lines.Count, $i + 4); $j++) {
          if ($lines[$j] -match '^\s+at |Caused by') { $found += $lines[$j].Trim() }
        }
        return ($found -join ' | ')
      }
    }
  }
  $last = @($lines | Where-Object { $_.Trim() } | Select-Object -Last 2)
  return "(no exception in the log; last: $($last -join ' | '))"
}

# Colors and pixels of a capture (C#: a capture is 320000 pixels): the background (the
# most common color), pixels away from it, and "text" pixels (far from it in luminance);
# and how many pixels two captures differ in.
if (-not ('GesherE2E' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public static class GesherE2E {
  static bool Near(int a, int b, int t) {
    return Math.Abs(((a >> 16) & 255) - ((b >> 16) & 255)) <= t && Math.Abs(((a >> 8) & 255) - ((b >> 8) & 255)) <= t &&
        Math.Abs((a & 255) - (b & 255)) <= t;
  }
  static double Lum(int c) { return 0.299 * ((c >> 16) & 255) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255); }
  // {background 0xRRGGBB, non-background pixels, text pixels, distinct colors}
  public static int[] Analyze(int[] px) {
    var counts = new Dictionary<int, int>();
    for (int i = 2; i < px.Length; i++) { int n; counts.TryGetValue(px[i], out n); counts[px[i]] = n + 1; }
    int bg = 0, best = -1;
    foreach (var kv in counts) if (kv.Value > best) { best = kv.Value; bg = kv.Key; }
    double bl = Lum(bg);
    int other = 0, text = 0;
    for (int i = 2; i < px.Length; i++) {
      int c = px[i];
      if (c == bg) continue;
      if (!Near(c, bg, 24)) other++;
      if (Math.Abs(Lum(c) - bl) > 96) text++;
    }
    return new int[] { bg, other, text, counts.Count };
  }
  public static int Diff(int[] a, int[] b) {
    if (a == null || b == null || a[0] != b[0] || a[1] != b[1]) return int.MaxValue;
    int n = 0;
    for (int i = 2; i < a.Length; i++) if (a[i] != b[i] && !Near(a[i], b[i], 24)) n++;
    return n;
  }
}
'@
}
function Analyze($px) {
  $r = [GesherE2E]::Analyze([int[]]$px)
  return [pscustomobject]@{ Width = $px[0]; Height = $px[1]; Background = ('{0:x6}' -f $r[0]); Other = $r[1]; Text = $r[2]; Colors = $r[3] }
}
function PixelDiff($a, $b) {
  if ($null -eq $a -or $null -eq $b) { return [int]::MaxValue }
  return [GesherE2E]::Diff([int[]]$a, [int[]]$b)
}

function SavePng($px, [string] $path) {
  if ($null -eq $px) { return }
  $w = $px[0]; $h = $px[1]
  $bmp = [System.Drawing.Bitmap]::new($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
  $data = $bmp.LockBits([System.Drawing.Rectangle]::new(0, 0, $w, $h), [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
                        [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
  for ($y = 0; $y -lt $h; $y++) {
    [Runtime.InteropServices.Marshal]::Copy([int[]]$px, 2 + $y * $w, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $w)
  }
  $bmp.UnlockBits($data)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
}

# Starts `cmd /c $commandLine` with its output in $log; returns the process.
function StartLogged([string] $commandLine, [string] $log) {
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"$commandLine > `"$log`" 2>&1`"")
  $psi.UseShellExecute = $false
  $psi.WorkingDirectory = $app
  return [Diagnostics.Process]::Start($psi)
}

function ReadText([string] $file) { return "$(Get-Content -Raw $file -ErrorAction SilentlyContinue)" }

# Waits for the app's first frame in $log; returns the window's handle or zero.
function WaitFirstFrame($p, [string] $log, [int] $seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $m = [regex]::Match((ReadText $log), 'first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)')
    if ($m.Success) { return [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16) }
    Start-Sleep -Milliseconds 500
  }
  return [IntPtr]::Zero
}

# Captures until two reads a second apart agree (the app settled) or $seconds pass.
function CaptureSettled([IntPtr] $hwnd, [int] $seconds = 15) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $last = [GesherWin32]::Capture($hwnd)
  do {
    Start-Sleep 1
    $px = [GesherWin32]::Capture($hwnd)
    if ($null -ne $px -and (PixelDiff $last $px) -eq 0) { return $px }
    $last = $px
  } while ((Get-Date) -lt $deadline)
  return $last
}

function AppPid([string] $text, [string] $pkg) {
  $m = [regex]::Match($text, "app process for $([regex]::Escape($pkg)): pid (\d+)")
  if ($m.Success) { return [int]$m.Groups[1].Value }
  return 0
}

function KillTree($p) {
  if ($null -ne $p -and -not $p.HasExited) { try { $p.Kill($true) } catch {} }
}

# Closes the window, waits for the launcher (and the app's process) to end. Returns problems.
function CloseAndWait([IntPtr] $hwnd, $p, [string] $log, [string] $pkg) {
  $problems = @()
  $appPid = AppPid (ReadText $log) $pkg
  if ([GesherWin32]::IsWindow($hwnd)) { [void][GesherWin32]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
  # An app may show more than one window (a dialog over its activity): close each one the
  # app process still has, until the launcher ends.
  $deadline = (Get-Date).AddSeconds(120)
  while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep 2
    foreach ($m in [regex]::Matches((ReadText $log), 'hwnd 0x([0-9a-f]+)')) {
      $h = [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16)
      if ([GesherWin32]::IsWindow($h)) { [void][GesherWin32]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    }
  }
  if (-not $p.HasExited) { KillTree $p; $problems += 'the launcher did not end within 120 s of WM_CLOSE' }
  else {
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { $problems += "the launcher exited with code $($p.ExitCode)" }
  }
  if ($appPid -and (Get-Process -Id $appPid -ErrorAction SilentlyContinue)) {
    $problems += "the app process $appPid is still running"
    Stop-Process -Id $appPid -Force -ErrorAction SilentlyContinue
  }
  $text = ReadText $log
  if ($text -notmatch "the process of $([regex]::Escape($pkg)) ended") { $problems += 'no "the process ... ended" line' }
  return $problems
}

# Launches with $commandLine, checks the first frame and the window's pixels, runs the
# app's input actions, closes it. Returns @{ Problems; Shot; Info }.
function RunWindow($a, [string] $commandLine, [string] $tag, [switch] $NoInput) {
  $pkg = $a.package
  $log = Join-Path $OutDir "$pkg.$tag.log"
  $res = @{ Problems = @(); Info = @() }
  $p = StartLogged $commandLine $log
  $hwnd = WaitFirstFrame $p $log 420
  if ($hwnd -eq [IntPtr]::Zero) {
    $res.Problems += $(if ($p.HasExited) { "no first frame (the launcher exited with code $($p.ExitCode))" } else { 'no first frame within 420 s' })
    KillTree $p
    return $res
  }
  # "waitLog" (apps.json): a line the app's log must show before the window is
  # checked, e.g. that a background step it starts at launch finished.
  if ($a.waitLog -and $tag -eq 'launch') {
    $appLogFile = "$env:LOCALAPPDATA\Gesher\logs\$pkg.log"
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline -and (ReadText $appLogFile) -notmatch $a.waitLog) { Start-Sleep 1 }
    $m = [regex]::Match((ReadText $appLogFile), $a.waitLog)
    if ($m.Success) { $res.Info += "the app's log shows '$($m.Value.Trim())'" }
    else { $res.Problems += "the app's log never showed /$($a.waitLog)/ within 120 s" }
  }
  Start-Sleep 3
  $px = CaptureSettled $hwnd
  if ($null -eq $px) { $res.Problems += 'PrintWindow failed'; KillTree $p; return $res }
  $png = Join-Path $OutDir "$pkg.$tag.png"
  SavePng $px $png
  $an = Analyze $px
  $res.Info += "$($an.Width)x$($an.Height), background #$($an.Background), $($an.Other) non-background and $($an.Text) text pixels, $($an.Colors)+ colors"
  $res.Info += "title '$([GesherWin32]::Title($hwnd))'"
  $area = $an.Width * $an.Height
  if ($an.Other -lt $area * 0.005) { $res.Problems += "the window is blank: $($an.Other) non-background pixels of $area" }
  if ($an.Text -lt 150) { $res.Problems += "no text: $($an.Text) text pixels" }
  if (-not $NoInput -and $a.actions) {
    $before = $px
    $i = 0
    foreach ($act in $a.actions) {
      $i++
      if ($act.click) { [GesherWin32]::Click($hwnd, [int]$act.click[0], [int]$act.click[1]); $what = "click at $($act.click[0]),$($act.click[1])" }
      elseif ($act.keys) { [GesherWin32]::TypeKeys($hwnd, [string]$act.keys); $what = "keys '$($act.keys)'" }
      Start-Sleep 2
      $after = CaptureSettled $hwnd 20
      $changed = PixelDiff $before $after
      SavePng $after (Join-Path $OutDir "$pkg.$tag.after-$i.png")
      $res.Info += "$what changed $changed pixels"
      if ($changed -lt 30) { $res.Problems += "$what did not change the window ($changed pixels)" }
      $before = $after
    }
  }
  $res.Problems += @(CloseAndWait $hwnd $p $log $pkg)
  return $res
}

# --- Install Gesher.
$app = Join-Path $env:LOCALAPPDATA 'Programs\Gesher'
if (-not $SkipInstall) {
  $setupExe = (Resolve-Path $Setup).Path
  Start-Process $setupExe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
}
if (-not (Test-Path "$app\gesher-app.cmd")) { throw "Gesher is not installed in $app" }
Report "INFO  e2e: Gesher installed in $app"

$manifestData = Get-Content -Raw $Manifest | ConvertFrom-Json
$apks = Join-Path $env:TEMP 'gesher-e2e'  # on Gesher's drive
New-Item -ItemType Directory -Force $apks | Out-Null
$env:GESHER_SHORTCUTS = $null
$passed = @()

foreach ($a in $manifestData.apps) {
  $pkg = $a.package
  if ($Only.Count -and $Only -notcontains $pkg) { continue }
  $name = "$pkg $($a.versionName) ($($a.label))"
  $known = $a.knownFailure
  $problems = @()
  $info = @()
  # --- Download and verify.
  $apk = Join-Path $apks "$($pkg)_$($a.versionCode).apk"
  $url = "$($manifestData.repo)/$($pkg)_$($a.versionCode).apk"
  try {
    if (-not (Test-Path $apk)) { Invoke-WebRequest -Uri $url -OutFile $apk -UseBasicParsing -MaximumRetryCount 3 -RetryIntervalSec 10 }
    $sha = (Get-FileHash -Algorithm SHA256 $apk).Hash.ToLowerInvariant()
    if (-not $a.sha256) { Report "INFO  e2e: $pkg $($a.versionCode): sha256 $sha (not pinned yet)" }
    elseif ($sha -ne $a.sha256.ToLowerInvariant()) { throw "SHA-256 $sha, not the pinned $($a.sha256)" }
  } catch {
    $problems += "download $url failed: $_"
  }
  # --- Install.
  if (-not $problems) {
    $installOut = & "$app\gesher-app.cmd" install $apk 2>&1 | Out-String
    Set-Content (Join-Path $OutDir "$pkg.install.log") $installOut
    if ($LASTEXITCODE -ne 0 -or $installOut -notmatch "installed $([regex]::Escape($pkg))|$([regex]::Escape($pkg)) is installed") {
      $problems += "gesher-app install failed (exit $LASTEXITCODE): $(FirstProblem $installOut)"
    } else {
      $parsed = [regex]::Match($installOut, 'parsed by PackageParser2[^\r\n]*')
      if ($parsed.Success) { $info += $parsed.Value }
    }
  }
  # --- Launch with an off-desktop window, check it, use it, close it.
  if (-not $problems) {
    # "trace": true: the app's process streams ART's method trace into the results
    # (GESHER_APP_METHOD_TRACE, docs/services.md), on a device started for it alone.
    $traceDir = $null
    if ($a.trace) {
      & "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null
      $traceDir = Join-Path $env:TEMP "gesher-e2e-trace-$pkg"
      New-Item -ItemType Directory -Force $traceDir | Out-Null
      $env:GESHER_APP_METHOD_TRACE = ($traceDir -replace '^[A-Za-z]:', '') -replace '\\', '/'
    }
    try {
      $r = RunWindow $a "`"$app\gesher-app.cmd`" `"$apk`" --hidden-window" 'launch'
    } finally {
      if ($traceDir) {
        & "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null
        Remove-Item Env:GESHER_APP_METHOD_TRACE -ErrorAction SilentlyContinue
        Get-ChildItem $traceDir -Filter *.trace -ErrorAction SilentlyContinue | Copy-Item -Destination $OutDir
      }
    }
    $problems += $r.Problems
    $info += $r.Info
  }
  foreach ($l in $info) { Report "INFO  e2e: ${pkg}: $l" }
  $log = ReadText (Join-Path $OutDir "$pkg.launch.log")
  $appLog = "$env:LOCALAPPDATA\Gesher\logs\$pkg.log"
  if (Test-Path $appLog) { Copy-Item $appLog (Join-Path $OutDir "$pkg.app.log") -Force }
  $standIns = [regex]::Matches($log + (ReadText $appLog), 'stand-in (\S+) not implemented') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
  if ($standIns) { Report "INFO  e2e: ${pkg}: stand-in methods called but not implemented: $($standIns -join ', ')" }
  if (-not $problems) {
    if ($known) { Report "PASS  e2e: $name (listed as a known failure: $known)" }
    else { Report "PASS  e2e: $name installs, draws its first frame (not blank), reacts to input, closes and its process ends" }
    $passed += $a
  } else {
    $why = FirstProblem ((ReadText $appLog) + "`n" + $log)
    $line = "$name`: $($problems -join '; '). First problem in the logs: $why"
    if ($known) { Report "KNOWN-FAIL  e2e: $line (known failure: $known)" } else { Report "FAIL  e2e: $line"; $script:failed++ }
  }
}

# --- The .apk association and the Start menu shortcut, for the first app that passed.
$first = $passed | Where-Object { -not $_.knownFailure } | Select-Object -First 1
if ($null -eq $first) { $first = $passed | Select-Object -First 1 }
if ($null -ne $first) {
  $pkg = $first.package
  $apk = Join-Path $apks "$($pkg)_$($first.versionCode).apk"
  $env:GESHER_APP_WINDOW = 'hidden-window'
  try {
    $open = (Get-ItemProperty 'HKCU:\Software\Classes\Gesher.Apk\shell\open\command').'(default)'
    $r = RunWindow $first $open.Replace('%1', $apk) 'association' -NoInput
    foreach ($l in $r.Info) { Report "INFO  e2e: $pkg (.apk association): $l" }
    if ($r.Problems) { Report "FAIL  e2e: $pkg through the .apk association's command ($open): $($r.Problems -join '; ')"; $script:failed++ }
    else { Report "PASS  e2e: $pkg starts through the .apk association's registered command, draws, closes" }

    $lnk = Get-ChildItem (Join-Path ([Environment]::GetFolderPath('Programs')) 'Gesher Apps') -Filter *.lnk -ErrorAction SilentlyContinue |
      Where-Object { (New-Object -ComObject WScript.Shell).CreateShortcut($_.FullName).Arguments -eq "launch $pkg" } | Select-Object -First 1
    if ($null -eq $lnk) { Report "FAIL  e2e: $pkg has no Start menu shortcut in Gesher Apps"; $script:failed++ }
    else {
      $link = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk.FullName)
      $r = RunWindow $first "`"$($link.TargetPath)`" $($link.Arguments)" 'shortcut' -NoInput
      foreach ($l in $r.Info) { Report "INFO  e2e: $pkg (Start menu): $l" }
      if ($r.Problems) { Report "FAIL  e2e: $pkg through its Start menu shortcut '$($lnk.Name)': $($r.Problems -join '; ')"; $script:failed++ }
      else { Report "PASS  e2e: $pkg starts through its Start menu shortcut '$($lnk.BaseName)' (`"$($link.TargetPath)`" $($link.Arguments)), draws, closes" }
    }
  } finally {
    Remove-Item Env:GESHER_APP_WINDOW -ErrorAction SilentlyContinue
  }
} else {
  Report 'FAIL  e2e: no app passed, so the .apk association and the Start menu shortcut were not tried'
  $script:failed++
}

$logs = "$env:LOCALAPPDATA\Gesher\logs"
if (Test-Path $logs) { Copy-Item "$logs\system_server*.log" $OutDir -ErrorAction SilentlyContinue }
# What the apps asked the system server's stand-ins for that they do not answer yet.
$serverMisses = [regex]::Matches((ReadText "$logs\system_server.log"), 'stand-in (\S+) not implemented') |
  ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
Report "INFO  e2e: system_server stand-in methods called but not implemented: $(if ($serverMisses) { $serverMisses -join ', ' } else { 'none' })"
& "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null
Report "INFO  e2e: $($script:failed) unexpected failures"
exit $(if ($script:failed) { 1 } else { 0 })
