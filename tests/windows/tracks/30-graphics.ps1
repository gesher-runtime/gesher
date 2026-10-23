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

# android.graphics on framework.jar with hwui's natives, and a Surface over
# gesher-app's Win32 window (tests/dex-graphics).
if (Test-Path "$Dir/graphics-tests.dex") {
  $bcpGraphics = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpGraphics" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/graphics-tests.dex") gesher.GraphicsTest `
    (NoDrive 'C:/Windows/Fonts/arial.ttf') (NoDrive $env:RUNNER_TEMP) 2>&1 | Out-String
  $code = $LASTEXITCODE
  if ($code -eq 0 -and $out.Contains('GraphicsTest all passed')) {
    Write-Host 'PASS  dalvikvm: android.graphics (Bitmap, Canvas, Paint text, codecs) and a Surface in a Win32 window'
  } else {
    Write-Host ('FAIL  dalvikvm: android.graphics (Bitmap, Canvas, Paint text, codecs) and a Surface in a Win32 window (exit 0x{0:X8})' -f $code)
    if (Test-Path "$Dir/dalvikvm.map") {
      $out | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $out
    }
    $failed++
  }
} else {
  Write-Host 'FAIL  dalvikvm: android.graphics: graphics-tests.dex is missing'
  $failed++
}
