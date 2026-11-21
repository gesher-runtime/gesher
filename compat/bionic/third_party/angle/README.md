# ANGLE's gl2ext_angle.h

`GLES2/gl2ext_angle.h` and `LICENSE` are unmodified copies from ANGLE at
commit `ab6a82e0ed1425316da0ba9a0b1c715a58a0b725`, the commit Gesher's
`libEGL.dll`/`libGLESv2.dll` are built from (`.github/workflows/angle.yml`,
`ANGLE_COMMIT`; AOSP android-16.0.0_r4's external/angle):

- https://chromium.googlesource.com/angle/angle/+/ab6a82e0ed1425316da0ba9a0b1c715a58a0b725/include/GLES2/gl2ext_angle.h
  (SHA-256 `2c49625b9dfd9d72714315247d6002e45759dc8423d3d366c17d5a41fd5d1380`)
- https://chromium.googlesource.com/angle/angle/+/ab6a82e0ed1425316da0ba9a0b1c715a58a0b725/LICENSE
  (SHA-256 `bf4da21bd20bcfb5b60b7ecc67fa864a79be049e21d6178076887f178dd6c71a`)

ANGLE is BSD-3-licensed (`LICENSE`). Only `compat/bionic/gen_gl_thunks.py`
reads the header: it declares the entry points of the extensions ANGLE adds
to `GL_EXTENSIONS` beyond the Khronos headers (`GL_ANGLE_*`,
`GL_CHROMIUM_*`), and `eglGetProcAddress` must return them for an advertised
extension. Update both files together with `ANGLE_COMMIT`.
