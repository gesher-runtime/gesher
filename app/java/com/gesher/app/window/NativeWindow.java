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

  public void hide() {
    nHide(ptr());
  }

  public void setTitle(String title) {
    nSetTitle(ptr(), title);
  }

  /** Whether the user may resize (and maximize) the window. */
  public void setResizable(boolean resizable) {
    nSetResizable(ptr(), resizable);
  }

  /** Resizes the window so its client area is width x height pixels. */
  public void setClientSize(int width, int height) {
    nSetClientSize(ptr(), width, height);
  }

  /** The window's icon (title bar, taskbar) from an .ico file; false if it cannot be read. */
  public boolean setIcon(String icoPath) {
    return nSetIcon(ptr(), icoPath);
  }

  /**
   * The System.AppUserModel.ID the window was given when it was created (the process's,
   * which app processes set from $GESHER_APP_USER_MODEL_ID), or null.
   */
  public String getAppUserModelId() {
    return nGetAppUserModelId(ptr());
  }

  /**
   * Makes a stand-in SurfaceControl handle ({@code SurfaceControl.mNativeObject}) stand for
   * this window: its copies, and the BLASTBufferQueue ViewRootImpl updates with it, hand
   * out this window's Surface (BLASTBufferQueue.nativeGetSurface).
   */
  public void tagSurfaceControl(long surfaceControlHandle) {
    nTagSurfaceControl(ptr(), surfaceControlHandle);
  }

  /** An eventfd (gesher-base) that is readable while events are queued. */
  public int getEventFd() {
    return nGetEventFd(ptr());
  }

  /** Event types ({@code gesher::WindowEvent}). */
  public static final int EVENT_MOUSE_DOWN = 1;
  public static final int EVENT_MOUSE_UP = 2;
  public static final int EVENT_MOUSE_MOVE = 3;
  public static final int EVENT_WHEEL = 4;
  public static final int EVENT_KEY_DOWN = 5;
  public static final int EVENT_KEY_UP = 6;
  public static final int EVENT_CHAR = 7;
  public static final int EVENT_RESIZE = 8;
  public static final int EVENT_FOCUS = 9;
  public static final int EVENT_CLOSE = 10;

  /**
   * Takes the next queued event into {@code out} (13 values: type, x, y, button, key,
   * scan, repeat, modifiers, delta, ch, width, height, time in ms); returns its type, or 0
   * when none is queued.
   */
  public int pollEvent(double[] out) {
    return nPollEvent(ptr(), out);
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

  /**
   * Presents what was last drawn into the Surface's buffer without
   * lockCanvas/unlockCanvasAndPost: hwui's CPU renderer (HardwareRenderer)
   * draws into the buffer in place and does not queue it.
   */
  public void present() {
    nPresent(ptr());
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
  private static native void nPresent(long ptr);
  private static native int[] nCapture(long ptr, boolean print);
  private static native void nHide(long ptr);
  private static native void nSetTitle(long ptr, String title);
  private static native void nSetResizable(long ptr, boolean resizable);
  private static native void nSetClientSize(long ptr, int width, int height);
  private static native void nTagSurfaceControl(long ptr, long surfaceControl);
  private static native int nGetEventFd(long ptr);
  private static native int nPollEvent(long ptr, double[] out);
  private static native boolean nSetIcon(long ptr, String icoPath);
  private static native String nGetAppUserModelId(long ptr);
}
