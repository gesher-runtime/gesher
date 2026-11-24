# RenderScript

Android's legacy compute API, `android.renderscript` (deprecated since API 31,
still in `framework.jar` and still used by apps, often to blur a Bitmap). Its
class initializer loads `librs_jni.so`; before this work Gesher had none, so
`RenderScript.<clinit>` threw `RSRuntimeException: Error loading RS jni
library` and an app using it crashed (issue #419, Anker Soundcore's
`BlurUtil`).

## What runs

| Piece | Source | Built by |
| --- | --- | --- |
| `librs_jni.so`, a builtin library of dalvikvm | frameworks/base `rs/jni/android_renderscript_RenderScript.cpp`, copied with System V JNI tables and Gesher's edits by `compat/renderscript/gen_jni_source.py`; registered by `compat/renderscript/renderscript_jni.cc` | `cmake/aosp/renderscript.cmake` (`gesher_renderscript`) |
| libRS's CPU runtime: contexts, Elements, Types, Allocations, Samplers, scripts, script groups, the reference driver (`driver/rsd*`) and `cpu_ref`'s intrinsics | frameworks/rs (aosp.lock), built as its `RS_COMPATIBILITY_LIB` variant (support.bp's `libRSSupport`), patched by `patches/frameworks/rs/0001-gesher-cpu-runtime.patch` | `renderscript_runtime` |
| `rsgApi.cpp`, `rsgApiReplay.cpp`, `rsgApiStructs.h`, `rsgApiFuncDecl.h` | generated from `rs.spec` and `rs_compat.spec` by `compat/renderscript/rsg_generator.py`, a transcription of AOSP's `rsg_generator.c` and its flex lexer `spec.l` (the build has no flex and runs on Linux and Windows hosts) | the same |

The runtime is AOSP's own, as Android's support library ships it for devices
without a vendor driver: a context thread with its command FIFO, the
`MessageThread` for messages and errors, worker threads for kernels
(`rsCpuCore.cpp`, over gesher-base's pthreads), and the intrinsics' portable
C++ (no NEON assembly and no SSSE3 path, so `gArchUseSIMD` stays off).

Supported, as on Android:

- `RenderScript.create`/`destroy`/`finish`, process contexts, messages and
  errors to the app (`RSErrorHandler`, `RSMessageHandler`).
- Elements, Types (1D-3D, mipmaps, faces, YUV), Allocations: typed, sized,
  `createFromBitmap` (bitmap-backed for `USAGE_SHARED`), `copyFrom`/`copyTo`
  arrays, Bitmaps and Allocations, ranges, `getByteBuffer`, adapters.
- The intrinsics of `cpu_ref`: Blur (U8, U8_4), ColorMatrix, Convolve3x3,
  Convolve5x5, LUT, 3DLUT, Blend (all modes), Histogram (and Dot), Resize
  (bicubic), YuvToRGB; ScriptGroup and ScriptGroup2 of intrinsics.

## What is not available, and how it fails

Each fails where and as Android fails for a driver that cannot do it; nothing
reports a result that was not computed.

- **ScriptC (custom kernels).** A `ScriptC_<name>` from an app's `.bc` needs
  RenderScript's bitcode compiler (bcc, LLVM and `libclcore.bc`), which Gesher
  does not have. `ScriptC` first checks for the platform's `libRS.so`
  (`System.loadLibrary("RS")`); Gesher has none (see "The support library"),
  so its constructor throws `UnsupportedOperationException("This device does
  not have an ABI that supports ScriptC.")`, the framework's own answer on a
  device without ScriptC. Behind that, `rsCpuScript.cpp` (patch 0001) does not
  load a script either (the compatibility library's precompiled
  `librs.<name>.so` is an Android ELF object a Windows process does not call
  into), so `rsnScriptCCreate` returns 0 and `ScriptC` would report
  `RSRuntimeException("Loading of ScriptC script failed.")`. Apps that ship
  their kernels through the support library (`android.support.v8.renderscript`,
  `androidx.renderscript`) use their own `librsjni.so` and `libRSSupport.so`
  instead.
- **ScriptIntrinsicBLAS** (BLAS and BNNM) needs libblasV8 and gemmlowp, which
  are not built: the CPU driver does not create it, `ScriptIntrinsic::init`
  reports that (patch 0001; AOSP ignores the driver's answer) and
  `ScriptIntrinsicBLAS.create` throws `RSRuntimeException` (the JNI's
  `nScriptIntrinsicCreate`, Gesher's edit).
- **Surface I/O** (`USAGE_IO_INPUT`/`USAGE_IO_OUTPUT`, `getSurface`,
  `setSurface`): such an Allocation is not created (patch 0001,
  `rsAllocation.cpp`, a non-fatal `RS_ERROR_BAD_VALUE` to the app's error
  handler) and `Allocation.createTyped` throws
  `RSRuntimeException("Allocation creation failed.")`.
- **RenderScript graphics** (`RenderScriptGL`, `RSSurfaceView`,
  `RSTextureView`, programs, meshes, fonts, A3D files; deprecated since API 16)
  needs libRS's OpenGL ES driver: `RenderScriptGL` throws
  `RSDriverException("Failed to create RS context.")`, and the other graphics
  natives throw `RSRuntimeException` (`compat/renderscript/rs_jni_unavailable.h`).

## The support library

`android.support.v8.renderscript` and `androidx.renderscript` load the app's
own `librsjni.so`. On API 19+ they first try the platform runtime through
`libRS.so`; Gesher registers no `libRS.so` (the runtime is linked into
dalvikvm with Windows' calling convention, which an app's ELF code cannot
call), so that `dlopen` fails and the support library falls back to the
`libRSSupport.so` it ships, as on a device without the platform library. Its
`Class.forName("android.renderscript.RenderScript")` check for `getMinorID` now
initializes the class instead of failing in `librs_jni.so`'s load.

## Tests

`tests/dex-graphics/gesher/RenderScriptTest.java` (track 30,
`tests/windows/tracks/30-graphics.ps1`): a context on framework.jar with
libandroid_runtime, then each intrinsic on known input compared with an
independent Java computation of its definition: Blur on a Bitmap the way
Soundcore's `BlurUtil` uses it, and on U8; ColorMatrix with an add vector;
Convolve3x3 and 5x5; LUT, Blend source-over, Histogram and YuvToRGB (NV21)
exactly; 3DLUT with an inverting table; Resize of ramps; Allocation copies of
arrays and Allocations. Float results may differ from the Java reference by
one step after truncation, so those compare within 1 (2-3 for the
interpolating 3DLUT and Resize). It also checks that ScriptC, BLAS and
`USAGE_IO_INPUT` fail with the exceptions above, that the context keeps working
after them, that a second context works after the first is destroyed, and that
four threads at once (two with contexts of their own, two sharing the process
context) blur Bitmaps correctly, as apps do from the UI thread and a background
Handler.
