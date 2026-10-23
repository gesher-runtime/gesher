# Graphics (docs/graphics.md): Skia, hwui, the Win32 window and input.
# Dot-sourced by run-tests.ps1: Check, RunDex, NoDrive, $Dir, $root, $bcp and
# $failed are its.

# Skia's CPU raster backend: shapes, antialiasing, FreeType text, codecs.
Check 'skia: CPU raster, FreeType text, PNG/JPEG/WebP codecs' {
  & "$Dir/skia_probe.exe" 'C:/Windows/Fonts/arial.ttf' (Join-Path $env:RUNNER_TEMP 'skia_probe.png')
} @('all passed')
