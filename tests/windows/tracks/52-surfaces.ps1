# Child surfaces (surfaces track; docs/graphics.md "Surfaces", ADR 0056): SurfaceView,
# TextureView and SurfaceTexture, Bitmap parcels, YuvImage, camera previews into views.
# Dot-sourced by run-tests.ps1 (Check, NoDrive, $Dir, $root, $failed).
#
# surfaceapp.apk (tests/apps/surfaceapp) steps through phases with a hidden window,
# logging "surfaces: phase N ready" when each is drawn; this reads the window back with
# PrintWindow at each and checks where each surface's frame is. Twice: in a single
# process (gesher-app --hidden-window) and on the Gesher device (its own app process),
# both with GESHER_CAMERA_TEST's camera for the preview phases.
. "$PSScriptRoot/../graphics/WindowChecks.ps1"
if (-not ('GesherSurfWin32' -as [type])) {
  Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GesherSurfWin32 {
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hwnd, int cmd);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr OpenFileMappingW(uint access, bool inherit, string name);
  [DllImport("kernel32.dll")] static extern IntPtr MapViewOfFile(IntPtr section, uint access, uint hi, uint lo, UIntPtr size);
  [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr view);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr OpenEventW(uint access, bool inherit, string name);
  [DllImport("kernel32.dll")] static extern bool SetEvent(IntPtr e);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  // SIGQUIT to a Gesher process (gesher-base's signal mailbox, compat/posix/src/process.cc):
  // ART's signal catcher dumps every thread's stack.
  public static bool SigQuit(int pid) {
    IntPtr section = OpenFileMappingW(0xF001F, false, "Local\\gesher-signals-" + pid);
    if (section == IntPtr.Zero) return false;
    IntPtr view = MapViewOfFile(section, 0xF001F, 0, 0, (UIntPtr)8);
    bool ok = false;
    if (view != IntPtr.Zero) {
      long bits = Marshal.ReadInt64(view);
      Marshal.WriteInt64(view, bits | (1L << 2));  // SIGQUIT (3)
      UnmapViewOfFile(view);
      IntPtr e = OpenEventW(0x1F0003, false, "Local\\gesher-signal-" + pid);
      if (e != IntPtr.Zero) { ok = SetEvent(e); CloseHandle(e); }
    }
    CloseHandle(section);
    return ok;
  }
}
'@
}

$bcpSurfaces = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmSurfaces = @("-Xbootclasspath:$bcpSurfaces", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# Pixels of [x0,x1) x [y0,y1) that are not white (a camera's frames are anything but).
function NotWhite($px, [int] $x0, [int] $y0, [int] $x1, [int] $y1) {
  $area = ($x1 - $x0) * ($y1 - $y0)
  return $area - [GesherWin32]::Count($px, $x0, $y0, $x1, $y1, 0xffffff, 12)
}

# One check: PASS when $problems is empty, else FAIL with them.
function SurfaceCheck([string] $Name, $problems) {
  $problems = @($problems | Where-Object { $_ })
  if (-not $problems) { Write-Host "PASS  surfaces: $Name" }
  else { Write-Host "FAIL  surfaces: $Name ($($problems -join '; '))"; $script:failed++ }
}

# Lines of $text that must be there, in order.
function InOrder([string] $text, [string[]] $lines) {
  $at = 0
  $missing = @()
  foreach ($l in $lines) {
    $i = $text.IndexOf($l, $at)
    if ($i -lt 0) { $missing += "missing '$l'" } else { $at = $i + $l.Length }
  }
  return $missing
}

# Runs surfaceapp with $Arguments; returns @{ Text; Shots = @{ phase -> pixels } }.
function SurfaceRun([string[]] $Arguments, [string] $Tag, [string[]] $AppLogs, [string] $Prefix = 'surfaces', [int] $Phases = 7) {
  $log = Join-Path $env:RUNNER_TEMP "surfaces-$Tag-$(Get-Random).log"
  $quoted = (@($dvmSurfaces) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.Environment['GESHER_CAMERA_TEST'] = '1'
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $read = {
    $t = Get-Content -Raw $log -ErrorAction SilentlyContinue
    foreach ($f in $AppLogs) { $t += "`n" + (Get-Content -Raw $f -ErrorAction SilentlyContinue) }
    "$t"
  }.GetNewClosure()
  $shots = @{}
  $hwnd = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds(500)
  $phase = 1
  $minimized = $false
  while ((Get-Date) -lt $deadline -and $phase -le $Phases) {
    $text = & $read
    # Pause and resume: minimized until the Activity stops, then restored (not activated).
    if (-not $minimized -and $hwnd -ne [IntPtr]::Zero -and $text.Contains('surfaces: minimize now')) {
      $minimized = $true
      [void][GesherSurfWin32]::ShowWindowAsync($hwnd, 6)  # SW_MINIMIZE
      $until = (Get-Date).AddSeconds(20)
      while ((Get-Date) -lt $until -and -not (& $read).Contains('surfaces: onStop')) { Start-Sleep -Milliseconds 250 }
      Start-Sleep 2
      [void][GesherSurfWin32]::ShowWindowAsync($hwnd, 4)  # SW_SHOWNOACTIVATE
      continue
    }
    if ($hwnd -eq [IntPtr]::Zero) {
      $m = [regex]::Match($text, 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)')
      if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16) }
    }
    if ($text.Contains("${Prefix}: done") -or $p.HasExited) { break }
    if ($hwnd -ne [IntPtr]::Zero -and $text.Contains("${Prefix}: phase $phase ready")) {
      Start-Sleep -Milliseconds 1500  # the frames after the step land
      $shots[$phase] = [GesherWin32]::Capture($hwnd)
      $phase++
      continue
    }
    Start-Sleep -Milliseconds 250
  }
  $timedOut = -not $p.WaitForExit(120000)
  if ($timedOut) {
    # Its threads' stacks into its log before it goes (ART's SIGQUIT dump).
    foreach ($c in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($p.Id)" -ErrorAction SilentlyContinue)) {
      if ($c.Name -eq 'dalvikvm.exe') { Write-Host "INFO  SIGQUIT to $($c.ProcessId): $([GesherSurfWin32]::SigQuit([int]$c.ProcessId))" }
    }
    Start-Sleep -Seconds 8
    $p.Kill($true); $p.WaitForExit()
  }
  # Its dalvikvm may outlive cmd.exe for a moment (files it maps stay in use).
  Start-Sleep -Seconds 2
  $text = & $read
  if ($timedOut) { $text += "
(killed: still running 120 s after its last phase)" }
  return @{ Text = $text; Shots = $shots; Hwnd = $hwnd; Exit = $p.ExitCode }
}

function SurfaceChecks([string] $Mode, $r) {
  $t = $r.Text
  $s = $r.Shots
  $none = { param($n) if ($null -eq $s[$n]) { "no read-back at phase $n" } }
  # Window-free checks.
  SurfaceCheck "Bitmap parcels ($Mode): a small Bitmap in place, a large one through shared memory (a memfd's descriptor), read back pixel for pixel" @(
    InOrder $t @('surfaces: bitmap parcel 16x8: parcel holds the pixels, read back 16x8 ARGB_8888 mutable true, 0 pixels differ',
                 'surfaces: bitmap parcel 400x300: parcel holds a descriptor, read back 400x300 ARGB_8888 mutable false, 0 pixels differ'))
  $yuv = [regex]::Match($t, 'surfaces: YuvImage NV21 compressToJpeg true, \d+ bytes, starts FFD8 true, decodes to 64x32, left (\d+),(\d+),(\d+), right (\d+),(\d+),(\d+)')
  $yp = @()
  if (-not $yuv.Success) { $yp += 'no YuvImage line: ' + ([regex]::Match($t, 'surfaces: YuvImage[^\r\n]*').Value) }
  else {
    # NV21 Y 90/Cb 170/Cr 110 is a gray blue (about 74,93,169); Y 170/Cb 60/Cr 190 an orange (about 240,165,26).
    $g = $yuv.Groups
    if (-not ([int]$g[3].Value -gt [int]$g[1].Value + 40)) { $yp += "left $($g[1]),$($g[2]),$($g[3]) is not blue" }
    if (-not ([int]$g[4].Value -gt [int]$g[6].Value + 100)) { $yp += "right $($g[4]),$($g[5]),$($g[6]) is not orange" }
  }
  SurfaceCheck "YuvImage ($Mode): NV21 to JPEG (libjpeg-turbo), decoded back with its colors" $yp
  SurfaceCheck "SurfaceTexture ($Mode): a Surface over it, a Canvas frame through lockCanvas, onFrameAvailable, release" @(
    InOrder $t @('surfaces: SurfaceTexture: Surface.lockCanvas 64x32, onFrameAvailable true', 'surfaces: SurfaceTexture: released true'))

  # Phase 1: SurfaceView below its window, a View over it; one on top; a TextureView.
  SurfaceCheck "SurfaceView ($Mode): surfaceCreated/surfaceChanged with the view's size, lockCanvas frames shown where the view is, behind the window (a View over it covers it)" @(
    (& $none 1)
    InOrder $t @('surfaces: A surfaceCreated, valid true', 'surfaces: A surfaceChanged format ', ' 200x200', 'surfaces: A: drew 200x200')
    if ($s[1]) { Regions $s[1] @(@('A red', 10, 10, 140, 140, 0xff0000), @('green View over A', 160, 160, 240, 240, 0x00ff00), @('white beside A', 260, 10, 390, 140, 0xffffff)) })
  SurfaceCheck "SurfaceView ($Mode): setZOrderOnTop draws over the window's views (a later yellow View under it)" @(
    (& $none 1)
    InOrder $t @('surfaces: B surfaceCreated, valid true', 'surfaces: B: drew 200x200')
    if ($s[1]) { Regions $s[1] @(@('B blue', 10, 260, 140, 390, 0x0000ff), @('B over yellow', 160, 410, 195, 445, 0x0000ff), @('yellow below B', 160, 455, 240, 495, 0xffff00)) })
  SurfaceCheck "TextureView ($Mode): onSurfaceTextureAvailable, a Canvas frame into its SurfaceTexture drawn in the view, getBitmap" @(
    (& $none 1)
    InOrder $t @('surfaces: T onSurfaceTextureAvailable 150x150', 'surfaces: T: drew cyan', 'surfaces: T getBitmap 150x150 center ff00ffff')
    if ($s[1]) { Regions $s[1] @(,@('T cyan', 260, 260, 390, 390, 0x00ffff)) })
  # Phase 2: setFixedSize.
  SurfaceCheck "SurfaceView ($Mode): setFixedSize(50, 25): surfaceChanged 50x25, the buffer scaled to the view" @(
    (& $none 2)
    InOrder $t @('surfaces: A surfaceChanged format ', ' 50x25', 'surfaces: A: drew 50x25')
    if ($s[2]) { Regions $s[2] @(@('A left red', 10, 10, 90, 140, 0xff0000), @('A right green', 110, 10, 190, 140, 0x00ff00)) })
  SurfaceCheck "TextureView ($Mode): lockCanvas / unlockCanvasAndPost draw into its SurfaceTexture" @(
    (& $none 2)
    InOrder $t @(,'surfaces: T lockCanvas: drew magenta 150x150')
    if ($s[2]) { Regions $s[2] @(,@('T magenta', 260, 260, 390, 390, 0xff00ff)) })
  # Phase 3: moved and resized by layout.
  SurfaceCheck "SurfaceView ($Mode): moved and resized by its layout: surfaceChanged 300x100, its frames follow the view" @(
    (& $none 3)
    InOrder $t @('surfaces: A surfaceChanged format ', ' 300x100', 'surfaces: A: drew 300x100')
    if ($s[3]) { Regions $s[3] @(@('A red at its new place', 60, 560, 340, 640, 0xff0000), @('white where A was', 10, 10, 140, 140, 0xffffff)) })
  # Phase 4: hidden and shown.
  SurfaceCheck "SurfaceView ($Mode): hidden (surfaceDestroyed) and shown again (surfaceCreated, surfaceChanged, drawn)" @(
    (& $none 4)
    InOrder $t @('surfaces: A surfaceDestroyed', 'surfaces: A surfaceCreated, valid true', 'surfaces: A: drew 300x100', 'surfaces: phase 4 ready')
    if ($s[4]) { Regions $s[4] @(,@('A red again', 60, 560, 340, 640, 0xff0000)) })
  # Paused and resumed (the window minimized and restored).
  SurfaceCheck "SurfaceView ($Mode): the Activity stopped (window minimized) destroys its surface; resumed (restored), it is created again and drawn" @(
    InOrder $t @('surfaces: minimize now', 'surfaces: onStop', 'surfaces: onResume')
    InOrder $t @('surfaces: minimize now', 'surfaces: A surfaceDestroyed')
    InOrder $t @('surfaces: minimize now', 'surfaces: A surfaceCreated, valid true', 'surfaces: A: drew 300x100'))
  # Phases 5-7: cameras.
  $cam = @()
  if ($t -match 'no camera: phases 5 to 7 skipped') { $cam += 'no camera (GESHER_CAMERA_TEST)' }
  SurfaceCheck "camera ($Mode): the legacy Camera's preview into a SurfaceView (setPreviewDisplay)" @(
    $cam; (& $none 5)
    InOrder $t @('surfaces: legacy camera: preview into SurfaceView C started', 'surfaces: phase 5 ready')
    if ($s[5]) { $n = NotWhite $s[5] 10 10 310 230; Write-Host "INFO  C: $n of 66000 pixels not white"; if ($n -lt 40000) { "C shows $n camera pixels of 66000" } })
  SurfaceCheck "camera ($Mode): the legacy Camera's preview into a TextureView (setPreviewTexture), getBitmap" @(
    $cam; (& $none 6)
    InOrder $t @('surfaces: legacy camera: preview into TextureView U started', 'surfaces: U getBitmap 320x240 center ')
    if ($s[6]) { $n = NotWhite $s[6] 10 310 310 530; Write-Host "INFO  U: $n of 66000 pixels not white"; if ($n -lt 40000) { "U shows $n camera pixels of 66000" } })
  SurfaceCheck "camera ($Mode): Camera2's repeating preview into a SurfaceView and a TextureView at once" @(
    $cam; (& $none 7)
    InOrder $t @('surfaces: Camera2: repeating preview into C and U', 'surfaces: phase 7 ready', 'surfaces: done')
    if ($s[7]) { foreach ($q in @(@('C', 10, 10, 310, 230), @('U', 10, 310, 310, 530))) { $n = NotWhite $s[7] $q[1] $q[2] $q[3] $q[4]; if ($n -lt 40000) { "$($q[0]) shows $n camera pixels of 66000" } } })
  if ($script:failed -gt $script:surfacesFailedBefore) {
    ("$t" -split "`n" | Where-Object { $_ -match 'surfaces|Surface|Texture|Exception|FATAL|Error| at ' } | Select-Object -First 150) | Out-Host
  }
}

# surfacegl.apk: EGL/GLES 2 from an NDK library into a SurfaceView, as SDL draws.
function SurfaceGlChecks([string] $Mode, $r) {
  $t = $r.Text
  $s = $r.Shots
  SurfaceCheck "SurfaceView ($Mode): EGL/GLES 2 from native code into its surface (ANativeWindow_fromSurface, eglCreateWindowSurface, eglSwapBuffers): the frames shown where the view is" @(
    if ($null -eq $s[1]) { 'no read-back at phase 1' }
    InOrder $t @('surfacegl: surfaceCreated', 'surfacegl: surfaceChanged 400x400', 'surfacegl: window 400x400',
                 'surfacegl: EGL surface 400x400 after 0 frames', 'surfacegl: 30 frames swapped (GL error 0x0)', 'surfacegl: phase 1 ready')
    if ($s[1]) { Regions $s[1] @(@('GL red half', 10, 10, 190, 390, 0xff0000), @('GL green half', 210, 10, 390, 390, 0x00ff00), @('white below', 10, 410, 390, 790, 0xffffff)) })
  SurfaceCheck "SurfaceView ($Mode): setFixedSize(100, 100) under EGL: the EGL surface follows, scaled to the view; surfaceDestroyed stops the renderer" @(
    if ($null -eq $s[2]) { 'no read-back at phase 2' }
    InOrder $t @('surfacegl: surfaceChanged 100x100', 'surfacegl: EGL surface 100x100 after ', 'surfacegl: phase 2 ready', 'surfacegl: surfaceDestroyed', 'surfacegl: stopped after ')
    if ($s[2]) { Regions $s[2] @(@('GL red half', 10, 10, 190, 390, 0xff0000), @('GL green half', 210, 10, 390, 390, 0x00ff00)) })
  if ($script:failed -gt $script:surfacesFailedBefore) {
    ("$t" -split "`n" | Where-Object { $_ -match 'surfacegl|EGL|Surface|Exception|FATAL| at |killed|gesher-app|^"|^  [|]|  - |tid=' } | Select-Object -Last 400) | Out-Host
  }
}

if (-not (Test-Path "$Dir/surfacegl.apk")) {
  Write-Host 'FAIL  surfaces: surfacegl.apk is missing'
  $failed++
} else {
  $script:surfacesFailedBefore = $failed
  $r = SurfaceRun @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/surfacegl.apk")) 'gl-single' @() 'surfacegl' 2
  SurfaceGlChecks 'single process' $r
}

if (-not (Test-Path "$Dir/surfaceapp.apk")) {
  Write-Host 'FAIL  surfaces: surfaceapp.apk is missing'
  $failed++
} else {
  Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force -ErrorAction SilentlyContinue
  Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force -ErrorAction SilentlyContinue
  # 1. A single process.
  $script:surfacesFailedBefore = $failed
  $r = SurfaceRun @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/surfaceapp.apk")) 'single' @()
  SurfaceChecks 'single process' $r

  # 2. The Gesher device: surfaceapp's own app process, started by the system server.
  $savedData = $env:ANDROID_DATA
  $savedLogs = $env:GESHER_LOGS
  $env:ANDROID_DATA = NoDrive "$root/data-surfaces"
  $env:GESHER_LOGS = NoDrive "$root/logs-surfaces"
  New-Item -ItemType Directory -Force "$root/data-surfaces", "$root/logs-surfaces" | Out-Null
  $env:GESHER_SHORTCUTS = '0'
  $env:GESHER_CAMERA_TEST = '1'  # the device's processes inherit it
  try {
    $script:surfacesFailedBefore = $failed
    $r = SurfaceRun @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/surfaceapp.apk"), '--hidden-window') 'device' @("$root/logs-surfaces/com.gesher.surfaceapp.log")
    SurfaceChecks 'Gesher device' $r
    if (Test-Path "$Dir/surfacegl.apk") {
      $script:surfacesFailedBefore = $failed
      $r = SurfaceRun @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/surfacegl.apk"), '--hidden-window') 'gl-device' @("$root/logs-surfaces/com.gesher.surfacegl.log") 'surfacegl' 2
      SurfaceGlChecks 'Gesher device' $r
    }
  } finally {
    $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmSurfaces) + 'com.gesher.app.Launcher', 'shutdown') `
      -NoNewWindow -PassThru -RedirectStandardOutput "$env:RUNNER_TEMP/surfaces-shutdown.log" -RedirectStandardError "$env:RUNNER_TEMP/surfaces-shutdown.err"
    if (-not $sd.WaitForExit(120000)) { $sd.Kill($true) }
    $env:ANDROID_DATA = $savedData
    if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
    Remove-Item Env:GESHER_SHORTCUTS, Env:GESHER_CAMERA_TEST -ErrorAction SilentlyContinue
  }
}
