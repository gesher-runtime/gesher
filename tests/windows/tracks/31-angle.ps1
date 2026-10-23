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
