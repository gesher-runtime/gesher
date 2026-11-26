# Shared real-window smoke workload, used by E2E and the isolated performance adapter.
# The caller supplies $app, $OutDir and $env:LOCALAPPDATA, and loads WindowChecks.ps1.
# Default behavior is the existing E2E path. Performance measurement is opt-in;
# its owned Job Object performs abnormal cleanup, never an unverified PID.

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
  public static bool Comparable(int[] a, int[] b) {
    return a != null && b != null && a.Length >= 2 && b.Length >= 2 &&
        a[0] > 0 && a[1] > 0 && (long)a[0] * a[1] + 2 == a.Length &&
        a[0] == b[0] && a[1] == b[1] && a.Length == b.Length;
  }
  public static int Diff(int[] a, int[] b) {
    if (!Comparable(a, b)) return int.MaxValue;
    int n = 0;
    for (int i = 2; i < a.Length; i++) if (a[i] != b[i] && !Near(a[i], b[i], 24)) n++;
    return n;
  }
  // A content rectangle excludes the surrounding native toolbar and controls.
  public static int[] Region(int[] px, int left, int top, int right, int bottom) {
    if (px == null || px.Length < 2 || px[0] <= 0 || px[1] <= 0 ||
        (long)px[0] * px[1] + 2 != px.Length || left < 0 || top < 0 ||
        right <= left || bottom <= top || right > px[0] || bottom > px[1]) return null;
    int width = right - left, height = bottom - top;
    var region = new int[2 + width * height];
    region[0] = width; region[1] = height;
    for (int y = 0; y < height; y++)
      Array.Copy(px, 2 + (top + y) * px[0] + left, region, 2 + y * width, width);
    return region;
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

# The app's own log (its process writes there and, while the launch runs, to the
# launch's log too).
function AppLogPath([string] $pkg) { return "$env:LOCALAPPDATA\Gesher\logs\$pkg.log" }

# How long the launch's and the app's logs are: taken before an action, so that only
# what the logs gained after it counts as the action's result.
function LogMark([string] $log, [string] $pkg) {
  return @((ReadText $log).Length, (ReadText (AppLogPath $pkg)).Length)
}

# What the launch's and the app's logs gained since LogMark (all of a log that shrank).
function LogsSince([string] $log, [string] $pkg, $mark) {
  $files = @($log, (AppLogPath $pkg))
  $out = ''
  for ($i = 0; $i -lt $files.Count; $i++) {
    $text = ReadText $files[$i]
    $from = if ($null -ne $mark -and $mark.Count -gt $i -and $text.Length -ge $mark[$i]) { $mark[$i] } else { 0 }
    $out += $text.Substring($from) + "`n"
  }
  return $out
}

# An input action can require evidence of its own result: a log line the action's
# asynchronous completion writes after it ($since: the logs' mark and the capture before
# the action), text in a content rectangle, and that rectangle changed from what it
# showed before the action. A completion before the action, or content that was already
# there (an editor the action should have replaced), is not the action's result.
function RenderReadiness($expect, $px, $since) {
  $problems = @()
  if ($null -eq $expect) { return @{ Ready = $true; Problems = $problems } }
  if (-not $expect.log -or -not (LogsSince $since.Log $since.Package $since.Mark).Contains([string]$expect.log)) {
    $problems += "the logs gained no '$($expect.log)' after the action"
  }
  $rect = @($expect.region)
  if ($null -eq $px) {
    $problems += 'no capture of the window (PrintWindow failed)'
  } elseif ($rect.Count -ne 4 -or [int]$expect.minTextPixels -le 0 -or [int]$expect.minChangedPixels -le 0) {
    $problems += 'invalid rendered-content rectangle or pixel requirements'
  } else {
    $region = [GesherE2E]::Region([int[]]$px, [int]$rect[0], [int]$rect[1], [int]$rect[2], [int]$rect[3])
    if ($null -eq $region) {
      $problems += "the content rectangle $($rect -join ',') is not within the $($px[0])x$($px[1]) capture"
    } else {
      $an = Analyze $region
      if ($an.Text -lt [int]$expect.minTextPixels) {
        $problems += "the content region has $($an.Text) text pixels, expected at least $($expect.minTextPixels)"
      }
      $earlier = $null
      if ($null -ne $since.Before) {
        $earlier = [GesherE2E]::Region([int[]]$since.Before, [int]$rect[0], [int]$rect[1], [int]$rect[2], [int]$rect[3])
      }
      # Without the same rectangle from before the action nothing shows a change
      # (PixelDiff's "incomparable" maximum would pass the requirement vacuously).
      if ($null -eq $since.Before) {
        $problems += 'no capture from before the action to compare the content region with (PrintWindow failed)'
      } elseif ($null -eq $earlier -or $earlier[0] -ne $region[0] -or $earlier[1] -ne $region[1]) {
        $problems += "the content rectangle $($rect -join ',') is not within the $($since.Before[0])x$($since.Before[1]) capture from before the action"
      } else {
        $changed = PixelDiff $earlier $region
        if ($changed -lt [int]$expect.minChangedPixels) {
          $problems += "the content region changed in $changed pixels since before the action, expected at least $($expect.minChangedPixels)"
        }
      }
    }
  }
  return @{ Ready = ($problems.Count -eq 0); Problems = $problems }
}

# A capture as a base64 PNG (for the job log); empty when there is none.
function PngBase64($px) {
  if ($null -eq $px) { return '' }
  $file = [IO.Path]::GetTempFileName()
  try {
    SavePng $px $file
    return [Convert]::ToBase64String([IO.File]::ReadAllBytes($file))
  } finally { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
}

function SavePng([int[]] $px, [string] $path) {
  if ($null -eq $px) { return }
  $w = $px[0]; $h = $px[1]
  $bmp = [System.Drawing.Bitmap]::new($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
  $data = $bmp.LockBits([System.Drawing.Rectangle]::new(0, 0, $w, $h), [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
                        [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
  for ($y = 0; $y -lt $h; $y++) {
    [Runtime.InteropServices.Marshal]::Copy($px, 2 + $y * $w, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $w)
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

# The owned-job adapter waits for the parent's live membership verification before
# any measured window interaction. Ordinary E2E does not enable this protocol.
function RegisterMeasuredParticipants($Performance, $Participants) {
  $items = @($Participants)
  if ($script:PerfJobOwnsCleanup) {
    if (-not $script:PerfRegisterParticipants) { throw 'Missing owned participant registrar' }
    $acknowledged = & $script:PerfRegisterParticipants -Participants $items
    if ($acknowledged -isnot [bool] -or -not $acknowledged) { throw 'Owned participant registration was not acknowledged' }
    foreach ($item in $items) {
      if ($item.role -eq 'application') {
        $script:PerfVerifiedWindowProcesses[[int]$item.pid] = [long]$item.created_100ns
      }
    }
  }
  $Performance.Participants += $items
}

function AssertMeasuredWindow([IntPtr] $hwnd) {
  if (-not $script:PerfJobOwnsCleanup) { return }
  if (-not [GesherWin32]::IsWindow($hwnd)) { throw 'Measured window no longer exists' }
  $ownerId = [int][GesherWin32]::ProcessId($hwnd)
  if (-not $script:PerfVerifiedWindowProcesses -or -not $script:PerfVerifiedWindowProcesses.ContainsKey($ownerId)) {
    throw 'Measured window owner was not acknowledged in the owned job'
  }
  $process = [Diagnostics.Process]::GetProcessById($ownerId)
  try {
    if ($process.HasExited -or $process.StartTime.ToFileTimeUtc() -ne $script:PerfVerifiedWindowProcesses[$ownerId] -or
        [GesherWin32]::ProcessId($hwnd) -ne $ownerId -or -not [GesherWin32]::IsWindow($hwnd)) {
      throw 'Measured window process generation or owner changed'
    }
  } finally { $process.Dispose() }
}

# Waits for the app's first frame in $log; returns the window's handle or zero.
function WaitFirstFrame($p, [string] $log, [int] $seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    foreach ($m in [regex]::Matches((ReadText $log), 'first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)')) {
      $window = [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16)
      if ([GesherWin32]::IsWindow($window)) { return $window }
    }
    Start-Sleep -Milliseconds 500
  }
  return [IntPtr]::Zero
}

# The app's newest window of the size of $hwnd's that has drawn and still exists: an app
# may replace its activity's window with another activity's (Puzzles' GamePlay goes up to
# its chooser at once; a click there starts a game in GamePlay). Dialogs (other sizes) are
# not taken.
function FrontWindow([string] $log, [IntPtr] $hwnd) {
  $ms = [regex]::Matches((ReadText $log), ' (\d+x\d+): first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)')
  $size = $null
  foreach ($m in $ms) { if ([Convert]::ToInt64($m.Groups[2].Value, 16) -eq $hwnd.ToInt64()) { $size = $m.Groups[1].Value } }
  if ($null -eq $size) { return $hwnd }
  for ($i = $ms.Count - 1; $i -ge 0; $i--) {
    $h = [IntPtr][Convert]::ToInt64($ms[$i].Groups[2].Value, 16)
    if ($ms[$i].Groups[1].Value -eq $size -and [GesherWin32]::IsWindow($h)) { return $h }
  }
  return $hwnd
}

# Captures until two reads a second apart agree on non-blank content (the app settled)
# or $seconds pass. Return one typed buffer so the pipeline does not box each pixel.
function CaptureSettled([IntPtr] $hwnd, [int] $seconds = 15, $render = $null, $since = $null, $renderReady = $null) {
  $deadline = (Get-Date).AddSeconds($seconds)
  if ($null -ne $renderReady) { $renderReady.Value = $false }
  AssertMeasuredWindow $hwnd
  $last = [GesherWin32]::Capture($hwnd)
  do {
    Start-Sleep 1
    AssertMeasuredWindow $hwnd
    $px = [GesherWin32]::Capture($hwnd)
    if ($null -ne $px -and (PixelDiff $last $px) -eq 0) {
      $an = Analyze $px
      if ($an.Other -ge ($an.Width * $an.Height * 0.005) -and (RenderReadiness $render $px $since).Ready) {
        if ($null -ne $renderReady) { $renderReady.Value = $true }
        return ,$px
      }
    }
    $last = $px
  } while ((Get-Date) -lt $deadline)
  return ,$last
}

# Diagnose an already failed blank-frame assertion without changing its result or
# sending input. Each sample records the actual capture, timing and pixel hash.
function BlankFrameDiagnostics([IntPtr] $hwnd, $initial, [string] $pkg, [string] $tag) {
  $info = @()
  $previous = $initial
  $clock = [Diagnostics.Stopwatch]::StartNew()
  $sha = [Security.Cryptography.SHA256]::Create()
  try {
    for ($i = 1; $i -le 15; $i++) {
      $remaining = 1000 * $i - $clock.ElapsedMilliseconds
      if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int]$remaining) }
      AssertMeasuredWindow $hwnd
      $px = [GesherWin32]::Capture($hwnd)
      if ($null -eq $px) {
        $info += "no-input blank diagnostic $i at $($clock.ElapsedMilliseconds) ms: PrintWindow failed"
        continue
      }
      $an = Analyze $px
      $bytes = [byte[]]::new($px.Length * 4)
      [Buffer]::BlockCopy([int[]]$px, 0, $bytes, 0, $bytes.Length)
      $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
      SavePng $px (Join-Path $OutDir "$pkg.$tag.no-input-$i.png")
      $info += "no-input blank diagnostic $i at $($clock.ElapsedMilliseconds) ms UTC $([DateTime]::UtcNow.ToString('o')): " +
        "$($an.Width)x$($an.Height), background #$($an.Background), $($an.Other) non-background, " +
        "$($an.Text) text, initial diff $(PixelDiff $initial $px), previous diff $(PixelDiff $previous $px), sha256 $hash"
      $previous = $px
    }
  } finally { $sha.Dispose() }
  return $info
}

function AppPid([string] $text, [string] $pkg) {
  $m = [regex]::Match($text, "app process for $([regex]::Escape($pkg)): pid (\d+)")
  if ($m.Success) { return [int]$m.Groups[1].Value }
  return 0
}

function KillTree($p) {
  if ($script:PerfJobOwnsCleanup) { return }
  if ($null -ne $p -and -not $p.HasExited) { try { $p.Kill($true) } catch {} }
}

function DismissStartupDialog([string] $log, [string] $pkg, $settings) {
  $text = ReadText $log
  $title = $settings.title
  $m = [regex]::Match($text,
    ('(?m)^gesher-app: window "{0}" \d+x\d+: first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)' -f
      [regex]::Escape($title)))
  if (-not $m.Success) { return $false }
  $dialog = [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16)
  $ownerId = AppPid $text $pkg
  AssertMeasuredWindow $dialog
  if (-not $ownerId -or [GesherWin32]::ProcessId($dialog) -ne $ownerId -or
      [GesherWin32]::Title($dialog) -cne $title) { throw "Cannot identify '$title' as the launched app's dialog" }
  SavePng ([GesherWin32]::Capture($dialog)) ([IO.Path]::ChangeExtension($log, '.startup-dialog.png'))
  $size = [GesherWin32]::ClientSize($dialog)
  if ($size[0] -ne $settings.size[0] -or $size[1] -ne $settings.size[1]) {
    throw "Unexpected geometry for '$title': $($size -join 'x')"
  }
  AssertMeasuredWindow $dialog
  [GesherWin32]::Click($dialog, [int]$settings.cancel[0], [int]$settings.cancel[1])
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  while ([GesherWin32]::IsWindow($dialog) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
  if ([GesherWin32]::IsWindow($dialog)) { throw "'$title' did not close" }
  return $true
}

# Closes the window, waits for the launcher (and the app's process) to end. Returns problems.
function CloseAndWait([IntPtr] $hwnd, $p, [string] $log, [string] $pkg) {
  $problems = @()
  $appPid = AppPid (ReadText $log) $pkg
  if ([GesherWin32]::IsWindow($hwnd)) {
    AssertMeasuredWindow $hwnd
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  # An app may show more than one window (a dialog over its activity): close each one the
  # app process still has, until the launcher ends.
  $deadline = (Get-Date).AddSeconds(120)
  while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep 2
    foreach ($m in [regex]::Matches((ReadText $log), 'hwnd 0x([0-9a-f]+)')) {
      $h = [IntPtr][Convert]::ToInt64($m.Groups[1].Value, 16)
      if ([GesherWin32]::IsWindow($h)) {
        AssertMeasuredWindow $h
        [void][GesherWin32]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
      }
    }
  }
  if (-not $p.HasExited) { KillTree $p; $problems += 'the launcher did not end within 120 s of WM_CLOSE' }
  else {
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { $problems += "the launcher exited with code $($p.ExitCode)" }
  }
  if ($appPid -and (Get-Process -Id $appPid -ErrorAction SilentlyContinue)) {
    $problems += "the app process $appPid is still running"
    if (-not $script:PerfJobOwnsCleanup) { Stop-Process -Id $appPid -Force -ErrorAction SilentlyContinue }
  }
  $text = ReadText $log
  if ($text -notmatch "the process of $([regex]::Escape($pkg)) ended") { $problems += 'no "the process ... ended" line' }
  return $problems
}

# Launches with $commandLine, checks the first frame and the window's pixels, runs the
# app's input actions, closes it. Returns @{ Problems; Shot; Info }.
function RunWindow($a, [string] $commandLine, [string] $tag, [switch] $NoInput, [switch] $Measure) {
  $pkg = $a.package
  $log = Join-Path $OutDir "$pkg.$tag.log"
  $res = @{ Problems = @(); Info = @() }
  if ($Measure) {
    $script:PerfVerifiedWindowProcesses = @{}
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $res.Performance = @{ Events = @(@{ name = 'command_start'; at_ms = 0.0 });
      Participants = @(); FirstFrameVerified = $false; NonblankVerified = $false;
      InputVerified = $false; LifecycleVerified = $false }
  }
  $p = StartLogged $commandLine $log
  $hwnd = WaitFirstFrame $p $log 420
  if ($hwnd -eq [IntPtr]::Zero) {
    $res.Problems += $(if ($p.HasExited) { "no first frame (the launcher exited with code $($p.ExitCode))" } else { 'no first frame within 420 s' })
    KillTree $p
    return $res
  }
  if ($Measure) {
    $res.Performance.Events += @{ name = 'first_frame_observed'; at_ms = $clock.Elapsed.TotalMilliseconds }
    $windowProcess = [Diagnostics.Process]::GetProcessById([GesherWin32]::ProcessId($hwnd))
    try {
      $participants = @(@{ pid = $windowProcess.Id; created_100ns = $windowProcess.StartTime.ToFileTimeUtc(); role = 'application' })
    } finally { $windowProcess.Dispose() }
    if ($script:PerfParticipantCapture) { $participants += @(& $script:PerfParticipantCapture) }
    RegisterMeasuredParticipants $res.Performance $participants
    AssertMeasuredWindow $hwnd
    $res.Performance.FirstFrameVerified = $true
  }
  Start-Sleep 3
  $actions = $a.actions
  if (-not $NoInput -and $a.startupDialog) {
    try {
      if (DismissStartupDialog $log $pkg $a.startupDialog) {
        $hwnd = WaitFirstFrame $p $log 15
        if ($hwnd -eq [IntPtr]::Zero) { throw 'No app window remained after closing the startup dialog' }
        $hwnd = FrontWindow $log $hwnd
        $focus = $a.startupDialog.focus
        AssertMeasuredWindow $hwnd
        [GesherWin32]::Click($hwnd, [int]$focus[0], [int]$focus[1])
        $actions = $a.startupDialog.actions
        $res.Info += "closed '$($a.startupDialog.title)' and focused the editor"
      }
    } catch {
      $res.Problems += $_.Exception.Message
      $res.Problems += @(CloseAndWait $hwnd $p $log $pkg)
      return $res
    }
  }
  $hwnd = FrontWindow $log $hwnd
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
  if ($Measure) { $res.Performance.NonblankVerified = $an.Other -ge $area * 0.005 -and $an.Text -ge 150 }
  if ($a.surfaceTrace -and $an.Other -lt $area * 0.005) {
    $res.Info += @(BlankFrameDiagnostics $hwnd $px $pkg $tag)
  }
  if (-not $NoInput -and $actions) {
    $before = $px
    $i = 0
    foreach ($act in $actions) {
      $i++
      $since = @{ Log = $log; Package = $pkg; Mark = (LogMark $log $pkg); Before = $before }
      AssertMeasuredWindow $hwnd
      if ($Measure -and $i -eq 1) {
        $res.Performance.Events += @{ name = 'first_input'; at_ms = $clock.Elapsed.TotalMilliseconds }
      }
      if ($act.click) { [GesherWin32]::Click($hwnd, [int]$act.click[0], [int]$act.click[1]); $what = "click at $($act.click[0]),$($act.click[1])" }
      elseif ($act.keys) { [GesherWin32]::TypeKeys($hwnd, [string]$act.keys); $what = "keys '$($act.keys)'" }
      Start-Sleep 2
      $hwnd = FrontWindow $log $hwnd
      # The existing 20-second capture deadline also owns readiness: no extra
      # wait after it, and no reset between the completion and pixel predicates.
      $renderReady = $false
      $after = CaptureSettled $hwnd 20 $act.render $since ([ref]$renderReady)
      if ($null -ne $act.render -and -not $renderReady) {
        $detail = (RenderReadiness $act.render $after $since).Problems -join '; '
        if (-not $detail) { $detail = 'the completed content did not stabilize' }
        $res.Problems += "rendered content did not become ready within the existing 20-second capture budget: $detail"
        # Failure-only diagnostic: the job log carries the captures too, for when
        # the uploaded results cannot be downloaded.
        foreach ($shot in @(@('before', $before), @('after', $after))) {
          $b64 = PngBase64 $shot[1]
          if ($b64) { $res.Info += "render diagnostic $what, capture $($shot[0]) the action (PNG, base64): $b64" }
        }
      }
      $comparable = [GesherE2E]::Comparable([int[]]$before, [int[]]$after)
      $changed = if ($comparable) { PixelDiff $before $after } else { $null }
      SavePng $after (Join-Path $OutDir "$pkg.$tag.after-$i.png")
      if (-not $comparable) { $res.Problems += "$what has missing or incomparable before/after captures" }
      else {
        $res.Info += "$what changed $changed pixels"
        if ($changed -lt 30) { $res.Problems += "$what did not change the window ($changed pixels)" }
      }
      if ($Measure -and $i -eq 1 -and $comparable -and $changed -ge 30 -and
          ($null -eq $act.render -or $renderReady)) {
        $res.Performance.Events += @{ name = 'first_input_change_observed'; at_ms = $clock.Elapsed.TotalMilliseconds }
        $res.Performance.InputVerified = $true
      }
      $before = $after
    }
  }
  # "waitLog" (apps.json): a line the app's log must show after the actions, e.g. that
  # a background step they started finished; the window is saved again then.
  if ($a.waitLog -and $tag -eq 'launch') {
    $appLogFile = AppLogPath $pkg
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline -and (ReadText $appLogFile) -notmatch $a.waitLog) { Start-Sleep 1 }
    $m = [regex]::Match((ReadText $appLogFile), $a.waitLog)
    if ($m.Success) {
      $res.Info += "the app's log shows '$($m.Value.Trim())'"
      $hwnd = FrontWindow $log $hwnd
      SavePng (CaptureSettled $hwnd 20) (Join-Path $OutDir "$pkg.$tag.after-log.png")
    } else {
      $res.Problems += "the app's log never showed /$($a.waitLog)/ within 120 s of the actions"
    }
  }
  if ($Measure -and $script:PerfParticipantCapture) {
    RegisterMeasuredParticipants $res.Performance @(& $script:PerfParticipantCapture)
  }
  $closeProblems = @(CloseAndWait $hwnd $p $log $pkg)
  $res.Problems += $closeProblems
  if ($Measure) {
    $res.Performance.LifecycleVerified = $closeProblems.Count -eq 0
    $res.Performance.Events += @{ name = 'completed'; at_ms = $clock.Elapsed.TotalMilliseconds }
  }
  return $res
}
