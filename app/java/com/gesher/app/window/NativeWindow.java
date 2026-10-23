package com.gesher.app.window;

import android.view.Surface;

/**
 * A top-level Win32 window and the {@link Surface} that draws into it
 * (docs/graphics.md, "Window"). The window has a thread of its own and starts
 * hidden; what is drawn into the Surface (lockCanvas/unlockCanvasAndPost, or
 * hwui's renderer) shows in the window. Natives: compat/hwui/window/window_jni.cc.
 */
public final class NativeWindow implements AutoCloseable {
  private long mPtr;

  private NativeWindow(long ptr) {
    mPtr = ptr;
  }

  /** A hidden window whose client area is width x height pixels. */
  public static NativeWindow create(String title, int width, int height) {
    long ptr = nCreate(title, width, height);
    if (ptr == 0) {
      throw new IllegalStateException("Windows refused to create a window");
    }
    return new NativeWindow(ptr);
  }

  public void show() {
    nShow(ptr());
  }

  /** A new Surface over the window (Win32Surface); release it when done. */
  public Surface getSurface() {
    return nGetSurface(ptr());
  }

  public int getWidth() {
    return nGetWidth(ptr());
  }

  public int getHeight() {
    return nGetHeight(ptr());
  }

  /** Frames the window's Surface has presented. */
  public long getFramesPresented() {
    return nGetFramesPresented(ptr());
  }

  /** The window's HWND. */
  public long getHwnd() {
    return nGetHwnd(ptr());
  }

  /**
   * What the window shows of its client area, read back with PrintWindow
   * (works without a visible desktop): {width, height, ARGB colors row by
   * row...}, or null if Windows refuses.
   */
  public int[] capture() {
    return nCapture(ptr(), true);
  }

  /** The window's back buffer: {width, height, ARGB colors row by row...}. */
  public int[] readBackBuffer() {
    return nCapture(ptr(), false);
  }

  /** Lets the window go; it closes when its Surfaces are released too. */
  @Override
  public void close() {
    if (mPtr != 0) {
      nDestroy(mPtr);
      mPtr = 0;
    }
  }

  private long ptr() {
    if (mPtr == 0) {
      throw new IllegalStateException("closed");
    }
    return mPtr;
  }

  private static native long nCreate(String title, int width, int height);
  private static native void nDestroy(long ptr);
  private static native void nShow(long ptr);
  private static native Surface nGetSurface(long ptr);
  private static native int nGetWidth(long ptr);
  private static native int nGetHeight(long ptr);
  private static native long nGetFramesPresented(long ptr);
  private static native long nGetHwnd(long ptr);
  private static native int[] nCapture(long ptr, boolean print);
}
