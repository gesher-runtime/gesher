package com.gesher.app.graphics;

/**
 * Which of hwui's pipelines renders in this process, and ways to read back what
 * the OpenGL one presented (docs/angle.md). The CPU pipeline is the default;
 * {@code GESHER_HWUI_RENDERER=skiagl} or the system property
 * {@code debug.hwui.renderer=skiagl} selects SkiaGL (Skia's Ganesh over ANGLE)
 * when OpenGL ES works. Natives: compat/hwui/gl/graphics_debug_jni.cc.
 */
public final class GraphicsDebug {
  private GraphicsDebug() {}

  /** "skiagl" or "skiacpu". */
  public static String renderer() {
    return nRenderer();
  }

  /**
   * What asked for SkiaGL (GESHER_HWUI_RENDERER or debug.hwui.renderer) and, for SkiaGL,
   * ANGLE's GL_RENDERER and versions, otherwise why the CPU pipeline renders; "the default"
   * when nothing asked.
   */
  public static String rendererDetail() {
    return nRendererDetail();
  }

  /**
   * What Windows composed of a window's client area (PrintWindow with
   * PW_RENDERFULLCONTENT, which sees a GL swap chain's frames):
   * {width, height, ARGB colors row by row...}, or null.
   */
  public static int[] captureComposed(long hwnd) {
    return nCaptureComposed(hwnd);
  }

  /**
   * Keeps a copy of each GL frame (glReadPixels before eglSwapBuffers) for
   * {@link #lastGlFrame}; also on with GESHER_HWUI_GL_READBACK=1.
   */
  public static void setGlReadback(boolean on) {
    nSetGlReadback(on);
  }

  /** The last GL frame presented to the window: {width, height, ARGB...}, or null. */
  public static int[] lastGlFrame(long hwnd) {
    return nLastGlFrame(hwnd);
  }

  private static native String nRenderer();
  private static native String nRendererDetail();
  private static native int[] nCaptureComposed(long hwnd);
  private static native void nSetGlReadback(boolean on);
  private static native int[] nLastGlFrame(long hwnd);
}
