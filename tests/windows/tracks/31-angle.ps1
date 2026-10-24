# ANGLE (docs/angle.md): EGL and OpenGL ES over Direct3D 11.
# Dot-sourced by run-tests.ps1: Check, $Dir and $failed are its.

# libEGL.dll/libGLESv2.dll from angle.yml, next to the probe in $Dir: a D3D11
# display (WARP when the runner has no usable adapter), a pbuffer, an ES 3
# context, shaders, a colored and a textured draw, glReadPixels.
Check 'angle: EGL on D3D11, GLES 3 shaders, draw, glReadPixels' {
  & "$Dir/angle_probe.exe"
} @('all passed')

# The same on WARP, D3D11's software rasterizer, whatever the adapter.
Check 'angle: the same on WARP' {
  & "$Dir/angle_probe.exe" --warp
} @('all passed (D3D11 WARP)')

# Skia's Ganesh GL backend over ANGLE (probes/graphics/skia_gl_probe.cc, from
# graphics.yml): a pbuffer on the WARP device, GrGLMakeNativeInterface,
# shapes, text and a bitmap drawn on the GPU and compared with Skia's CPU
# raster backend.
Check 'angle: Skia Ganesh GL over ANGLE (WARP), the same pixels as the CPU raster backend' {
  & "$Dir/skia_gl_probe.exe" --warp 'C:/Windows/Fonts/arial.ttf'
} @('all passed (Ganesh GL on ANGLE, D3D11 WARP)')

# hwui's SkiaGL pipeline over ANGLE (docs/angle.md, ADR 0030;
# tests/dex-graphics/gesher/HardwareRendererGlTest.java). The same RenderNode
# tree (a child node, text, a bitmap) is rendered by HardwareRenderer into a
# Win32 window with the CPU pipeline (the default), which saves its frame, and
# with SkiaGL (GESHER_HWUI_RENDERER=skiagl) on WARP: an EGL window surface on
# the window's HWND, eglSwapBuffers; its frame, read back with glReadPixels
# and from what Windows composed (PrintWindow with PW_RENDERFULLCONTENT), must
# match. GraphicsDex comes from 30-graphics.ps1, which also copied the font.
$hwuiFont = NoDrive (Join-Path $root 'arial.ttf')
$hwuiFrame = NoDrive (Join-Path $env:RUNNER_TEMP 'hwui-cpu-frame.bin')
GraphicsDex 'HardwareRenderer: the CPU pipeline''s frame (child node, text, bitmap) for the SkiaGL comparison' `
  HardwareRendererGlTest @($hwuiFont, $hwuiFrame, 'cpu')
$env:GESHER_HWUI_RENDERER = 'skiagl'
$env:GESHER_EGL_DEVICE = 'warp'
GraphicsDex 'HardwareRenderer: SkiaGL over ANGLE (WARP) into the Win32 window, the same frame as the CPU pipeline' `
  HardwareRendererGlTest @($hwuiFont, $hwuiFrame, 'gl')
Remove-Item Env:GESHER_EGL_DEVICE

# Without ANGLE (the files of $Dir, hard-linked, less libEGL.dll and
# libGLESv2.dll), skiagl falls back to the CPU pipeline with a line saying so.
$noAngle = Join-Path $env:RUNNER_TEMP 'dist-no-angle'
New-Item -ItemType Directory -Force $noAngle | Out-Null
foreach ($file in Get-ChildItem -File $Dir) {
  if ($file.Name -in @('libEGL.dll', 'libGLESv2.dll')) { continue }
  $link = Join-Path $noAngle $file.Name
  if (-not (Test-Path $link)) {
    try { New-Item -ItemType HardLink -Path $link -Target $file.FullName -ErrorAction Stop | Out-Null }
    catch { Copy-Item $file.FullName $link }
  }
}
$distDir = $Dir
$Dir = $noAngle
GraphicsDex 'HardwareRenderer: skiagl without ANGLE falls back to the CPU pipeline, the same frame' `
  HardwareRendererGlTest @($hwuiFont, $hwuiFrame, 'fallback')
$Dir = $distDir
Remove-Item Env:GESHER_HWUI_RENDERER
