# Graphics (docs/graphics.md): Skia, hwui, the Win32 window and input.
# Dot-sourced by run-tests.ps1: Check, RunDex, NoDrive, $Dir, $root, $bcp and
# $failed are its.

# Skia's CPU raster backend: shapes, antialiasing, FreeType text, codecs.
Check 'skia: CPU raster, FreeType text, PNG/JPEG/WebP codecs' {
  & "$Dir/skia_probe.exe" 'C:/Windows/Fonts/arial.ttf' (Join-Path $env:RUNNER_TEMP 'skia_probe.png')
} @('all passed')

# The Win32 window (compat/hwui/window): Skia draws into its back buffer and
# PrintWindow reads it back off-screen; posted input comes out of its queue;
# Win32Surface (the ANativeWindow over it) presents RGBA frames.
Check 'window: Win32 window, PrintWindow read-back, input queue, Win32Surface' {
  & "$Dir/window_probe.exe" (Join-Path $env:RUNNER_TEMP 'window_probe.png')
} @('all passed')

# Runs a class of graphics-tests.dex (tests/dex-graphics) with framework.jar
# and gesher-app.jar on the boot class path, with a time limit (a render
# thread that goes wrong tends to wait forever); passes when it exits 0 and
# prints "<Main> all passed".
function GraphicsDex([string] $Name, [string] $Main, [string[]] $Arguments = @(), [int] $Seconds = 180) {
  if (-not (Test-Path "$Dir/graphics-tests.dex")) {
    Write-Host "FAIL  dalvikvm: $Name (graphics-tests.dex is missing)"
    $script:failed++
    return
  }
  $bcpGraphics = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @("-Xbootclasspath:$bcpGraphics", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
                   '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/graphics-tests.dex"), "gesher.$Main") + $Arguments) {
    $psi.ArgumentList.Add($a)
  }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) { $p.Kill($true); $p.WaitForExit() }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $code = if ($timedOut) { -1 } else { $p.ExitCode }
  $out = $stdout.Result + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  if ($code -eq 0 -and $out.Contains("$Main all passed")) {
    Write-Host "PASS  dalvikvm: $Name"
    ($out -split "`n" | Select-String -Pattern '^(PASS|FAIL)  ') | Out-Host
  } else {
    Write-Host ("FAIL  dalvikvm: $Name (exit 0x{0:X8})" -f $code)
    if (Test-Path "$Dir/dalvikvm.map") {
      $out | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $out
    }
    $script:failed++
  }
}

# android.graphics on framework.jar with hwui's natives (Bitmap, Canvas, Paint
# text with a font file, BitmapFactory/compress), and a Surface over
# gesher-app's Win32 window (lockCanvas, PrintWindow read-back).
GraphicsDex 'android.graphics (Bitmap, Canvas, Paint text, codecs) and a Surface in a Win32 window' `
  GraphicsTest @((NoDrive 'C:/Windows/Fonts/arial.ttf'), (NoDrive $env:RUNNER_TEMP))

# hwui's renderer: HardwareRenderer, its RenderThread and the CPU pipeline
# drawing a RenderNode tree into the window's Surface.
GraphicsDex 'HardwareRenderer: a RenderNode tree into the Win32 window (CPU pipeline)' HardwareRendererTest
