# ANGLE/D3D11 hardware rendering: automatic GPU selection and safety gates

Gesher already contains the SkiaGL -> ANGLE -> D3D11 renderer. Explicit
`GESHER_HWUI_RENDERER=auto` selects it on recognized hardware, including
Intel UHD/Iris Xe/Arc, after qualification. SkiaCpu remains the default
for all processes until the physical acceptance below is recorded.
No WSA proprietary code is copied.

## Selection

- Default, windowed/hidden/headless/services/CLI: stay on SkiaCpu with
  **zero GPU probes**. Window presence alone never enables GPU rendering.
- GESHER_HWUI_RENDERER=skiacpu: always SkiaCpu, even if an iGPU is present.
- GESHER_HWUI_RENDERER=skiagl: explicit GL, including D3D11 WARP for tests.
- GESHER_HWUI_RENDERER=auto: qualify hardware, otherwise fallback to SkiaCpu.
- debug.hwui.renderer accepts the same choices if the environment did not
  override them. The EGL software override GESHER_EGL_DEVICE=warp forces CPU
  in auto mode (explicit skiagl can still be used to test WARP).

The adapter whitelist accepts D3D11 hardware renderer strings from Intel,
AMD/ATI, NVIDIA, Qualcomm, ARM and Imagination, rejecting Microsoft Basic Render
Driver, SwiftShader, llvmpipe and WARP. Unknown devices default to CPU unless
the user explicitly requests skiagl. Hardware selection is conservative.

## Full Ganesh qualification (review P1 fix)

Prior to this change, Gesher considered GL usable after assembling and
validating GrGLInterface. That was insufficient: RenderThread later calls
GrDirectContexts::MakeGL, whose failure is fatal.

The qualification now creates a real temporary **GrDirectContext** on a
small EGL pbuffer, using the same context options as RenderThread:
fPreferExternalImagesOverES3, fDisableDistanceFieldPaths, and the
PROPERTY_REDUCE_OPS_TASK_SPLITTING system property. Both callers obtain
their options from a single header. If MakeGL returns null, the selector
chooses SkiaCpu *before* the render pipeline is fixed.

The probe releases the temporary Ganesh context **while its EGL context is
current**, restores the previously bound EGL draw/read surfaces, display and
context, and destroys the probe surface/context. Missing ANGLE, failed EGL
setup, invalid GL interface, rejected software GPU, and failed MakeGL all
return an explicit diagnostic and use CPU. An auto mode run on WARP rejects
the adapter before allocating a Ganesh context; explicit skiagl still tests
WARP. EGL initialization is process-scoped and is deliberately not terminated.

This prevents the specific reproduced startup failure described in PR #55.
It is not a guarantee of recovery from a **later GPU device loss**: the
pipeline cannot safely change after HWUI windows and surfaces are created.

## Regression evidence

Run `python3 tools/test_hwui_renderer_policy.py --sanitize` on a host with
C++20, ASan and UBSan. Tests include:

1. Production hardware/vendor matching, zero-probe defaults for windowed,
   hidden and headless processes, explicit auto selection and CPU override.
2. Production selector and exact production ANGLE qualification function
   extracted from angle_gl.cc with deterministic EGL/Ganesh adapters.
3. Successful Intel qualification; failed GrDirectContexts::MakeGL; invalid
   GL interface; failed EGL display, config, context, pbuffer or make-current.
4. No leaks or stale current context: every test asserts exact destroyed
   EGL object counts, Ganesh resource release and preservation of an existing
   EGL context with *different draw and read surfaces*.
5. Software WARP auto skip; WARP explicit override; shared context options
   with renderthread.skia.reduceopstasksplitting both enabled and disabled.

These isolated controls do not test actual Intel drivers. Windows track 31
also exercises SkiaGL over WARP, the software auto/CPU fallback, window
composition and GLES rendering. Full AOSP/ART/Windows CI is mandatory before
merge, and both canonical build suppliers must be verified. Physical acceptance
is required before changing the default; it is not established by these fixtures.

## Physical acceptance needed before default use

On physical Windows machines with Intel UHD and Iris Xe, a discrete GPU,
and Basic Render Driver/WARP:

- Run the same diverse APKs with auto, skiacpu and explicit skiagl. Confirm
  first frame correctness, startup time and correct adapter selection.
- Test resize, DPI changes, translucent popups, TextureView/SurfaceView/
  ImageReader, device loss, repeated open/close and concurrent rendering.
- Report warm p50/p95 frame time, dropped/late frames, CPU/GPU utilization,
  private committed/resident memory, peak GPU memory and memory after
  trimming/closing. Explicit trim signals now purge the real Ganesh context:
  UI_HIDDEN temporarily reduces cache budgets and restores them; BACKGROUND
  and above synchronize, release resources and retire the rendering context.
  Automatic idle cache budgeting remains separate work.
- Compare boot-to-first-frame and per-app startup cost; the preflight creates
  and tears down a short-lived Ganesh context and may cost startup latency.
- Do not enable GPU by default based only on successful host tests or declare
  an FPS speedup without this evidence. Explicit auto remains an opt-in path.

Limitations: some TextureView/HardwareBitmap paths are still stand-ins;
translucent popups and child surfaces may perform glReadPixels; and
recovering from device loss is a separate architectural task.

## Local physical acceptance, 2026-11-17

The local AOSP build ran on Intel UHD Graphics 770, driver 32.0.101.6556,
using ANGLE 2.1.1 (ab6a82e0ed14). The native Ganesh probe passed its existing
CPU comparison tolerance. The actual ART HardwareRenderer selected SkiaGL
for explicit auto; both glReadPixels and the Windows-composed HWND frame
matched the CPU frame outside text with zero differing pixels, and the text
had 473 dark pixels in both pipelines.

Physical resize and redraw passed for 32 alternating 240x160/320x200 frames.
The committed trim/resize fixture separately passed levels 20, 40, 60 and 80
on hardware and WARP, including context recreation and subsequent drawing.
An isolated diagnostic build querying actual Ganesh cache usage measured
4,278,272 bytes before a background trim, 49,152 bytes after freeing unlocked
resources, then no rendering context after retirement. The diagnostic logging
was removed and the ordinary binary rebuilt and retested.

These are local acceptance checks, not a general performance benchmark or
qualification of other GPUs, device loss, hardware bitmaps or diverse APKs.
CPU remains the default. The exact-method host regression fails on the old
no-op implementation and runs in CI alongside the qualification tests.
