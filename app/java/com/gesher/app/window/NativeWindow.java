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

  /**
   * A hidden popup window (no frame, no taskbar button) whose client area is width x height
   * pixels, owned by {@code owner} (null: none): Windows keeps it above its owner and hides it
   * with it (docs/graphics.md, "Dialogs, menus and other popups").
   */
  public static NativeWindow createPopup(String title, int width, int height,
      NativeWindow owner) {
    long ptr = nCreatePopup(title, width, height, owner != null ? owner.ptr() : 0);
    if (ptr == 0) {
      throw new IllegalStateException("Windows refused to create a popup window");
    }
    return new NativeWindow(ptr);
  }

  /**
   * Places a popup: its client area at (x, y) of {@code anchor}'s client area (the screen's
   * when null), width x height pixels; it follows the anchor when that moves.
   */
  public void setPopupBounds(NativeWindow anchor, int x, int y, int width, int height) {
    nSetPopupBounds(ptr(), anchor != null ? anchor.ptr() : 0, x, y, width, height);
  }

  /** Shows a popup where setPopupBounds put it, activated or not. */
  public void showPopup(boolean activate) {
    nShowPopup(ptr(), activate);
  }

  /** Shows what is drawn with its alpha (a layered window) over what is behind. */
  public void setTranslucent(boolean translucent) {
    nSetTranslucent(ptr(), translucent);
  }

  /** Never activated by a click; with {@code passthrough} clicks go through it. */
  public void setNoActivate(boolean noActivate, boolean passthrough) {
    nSetNoActivate(ptr(), noActivate, passthrough);
  }

  /** To the top of its owner's popups, not activated. */
  public void raise() {
    nRaise(ptr());
  }

  /** Brings the window to the front and gives it the keyboard. */
  public void activate() {
    nActivate(ptr());
  }

  /** Dims what the window shows by {@code amount} (0..1), as behind a dialog. */
  public void setDim(float amount) {
    nSetDim(ptr(), amount);
  }

  /**
   * Where the window's input goes instead of its own queue (null: its own): pointer events
   * (a gesture stays with the window it started in), keys and characters; {@code outside}:
   * windows told of a press outside them.
   */
  public void setInputRoute(NativeWindow pointer, NativeWindow keys, NativeWindow[] outside) {
    long[] watchers = new long[outside != null ? outside.length : 0];
    for (int i = 0; i < watchers.length; i++) watchers[i] = outside[i].ptr();
    nSetInputRoute(ptr(), pointer != null ? pointer.ptr() : 0, keys != null ? keys.ptr() : 0,
        watchers);
  }

  /** The client area on the screen: {left, top, right, bottom}. */
  public int[] getScreenBounds() {
    return nGetScreenBounds(ptr());
  }

  /** Moves the window so its client area starts at (x, y) on the screen. */
  public void setScreenPosition(int x, int y) {
    nSetScreenPosition(ptr(), x, y);
  }

  public void show() {
    nShow(ptr());
  }

  public void hide() {
    nHide(ptr());
  }

  /** Minimizes the window, not activated (its taskbar button stays; windows3 track). */
  public void minimize() {
    nMinimize(ptr());
  }

  /**
   * Shows the window off the desktop (no taskbar button, not activated): drawn like a shown
   * window, so PrintWindow reads it back, which it does not for a hidden one.
   */
  public void showOffscreen() {
    nShowOffscreen(ptr());
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

  /**
   * The smallest client area (pixels) the user can resize the window to; 0 for no minimum on
   * that axis.
   */
  public void setMinClientSize(int width, int height) {
    nSetMinClientSize(ptr(), width, height);
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
  private static native void nMinimize(long ptr);
  private static native void nShowOffscreen(long ptr);
  private static native void nSetTitle(long ptr, String title);
  private static native void nSetResizable(long ptr, boolean resizable);
  private static native void nSetClientSize(long ptr, int width, int height);
  private static native void nSetMinClientSize(long ptr, int width, int height);
  private static native void nTagSurfaceControl(long ptr, long surfaceControl);
  private static native int nGetEventFd(long ptr);
  private static native int nPollEvent(long ptr, double[] out);
  private static native boolean nSetIcon(long ptr, String icoPath);
  private static native String nGetAppUserModelId(long ptr);
  private static native long nCreatePopup(String title, int width, int height, long owner);
  private static native void nSetPopupBounds(long ptr, long anchor, int x, int y, int width,
      int height);
  private static native void nShowPopup(long ptr, boolean activate);
  private static native void nSetTranslucent(long ptr, boolean translucent);
  private static native void nSetNoActivate(long ptr, boolean noActivate, boolean passthrough);
  private static native void nRaise(long ptr);
  private static native void nActivate(long ptr);
  private static native void nSetDim(long ptr, float amount);
  private static native void nSetInputRoute(long ptr, long pointer, long keys, long[] outside);
  private static native int[] nGetScreenBounds(long ptr);
  private static native void nSetScreenPosition(long ptr, int x, int y);
}
