package com.gesher.app.input;

import android.view.InputChannel;

/**
 * A Gesher window's input, published on an InputChannel (docs/input.md):
 * the window's mouse, keyboard, wheel, touch and pen messages become
 * MotionEvents and KeyEvents on the channel's server end, for an
 * {@code InputEventReceiver} (ViewRootImpl's) on the client end. A window
 * session does this in {@code addToDisplay}:
 *
 * <pre>
 *   InputChannel[] pair = InputChannel.openInputChannelPair(name);
 *   pair[1].copyTo(outInputChannel);   // the app's end
 *   WindowInput input = WindowInput.attach(nativeWindow.getHwnd(), pair[0]);
 *   ...
 *   input.close();                     // in remove(), before the window goes
 * </pre>
 *
 * The connection reads the window's event queue on a thread of its own;
 * window events that are not input go to {@link Listener#onWindowEvent}.
 * Natives: compat/input/input_jni.cc (gesher_input.h is the same for C++).
 */
public final class WindowInput implements AutoCloseable {
  /** gesher::WindowEvent's types (compat/hwui/window/gesher_window.h). */
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
  public static final int EVENT_TOUCH_DOWN = 11;
  public static final int EVENT_TOUCH_MOVE = 12;
  public static final int EVENT_TOUCH_UP = 13;
  public static final int EVENT_TOUCH_CANCEL = 14;
  public static final int EVENT_MOUSE_LEAVE = 15;
  /** The window was minimized (a = 0) or restored (a = 1): a listener event. */
  public static final int EVENT_VISIBILITY = 16;
  /** A press outside a window watching outside touches: published as ACTION_OUTSIDE. */
  public static final int EVENT_OUTSIDE = 17;

  /** gesher::WindowEvent's modifiers. */
  public static final int MODIFIER_SHIFT = 1;
  public static final int MODIFIER_CTRL = 2;
  public static final int MODIFIER_ALT = 4;
  public static final int MODIFIER_CAPS_LOCK = 8;

  /** Window events that are not input, and focus changes. */
  public interface Listener {
    /**
     * On the connection's thread. {@code type} is an EVENT_* constant; for
     * EVENT_RESIZE a and b are the client area's width and height in pixels,
     * for EVENT_FOCUS a is 1 when the window gains focus, for EVENT_CHAR a is
     * the UTF-16 code unit and b the modifiers, for EVENT_VISIBILITY a is 0
     * when the window was minimized and 1 when it was restored.
     */
    void onWindowEvent(int type, int a, int b);
  }

  /** Where the client area's pixels land in the Android window, and more. */
  public static final class Options {
    public float scale = 1f;
    public float offsetX;
    public float offsetY;
    public int displayId;
    /** Unfinished events at most (0: 32). */
    public int maxInFlight;
    /** Sends a focus event as soon as the connection starts. */
    public boolean initialFocus;
    public Listener listener;
  }

  private long mPtr;
  private final Listener mListener;

  private WindowInput(Listener listener) {
    mListener = listener;
  }

  /** Attaches the window whose HWND is {@code hwnd} (NativeWindow.getHwnd()). */
  public static WindowInput attach(long hwnd, InputChannel serverChannel) {
    return attach(hwnd, serverChannel, new Options());
  }

  public static WindowInput attach(long hwnd, InputChannel serverChannel, Options options) {
    WindowInput input = new WindowInput(options.listener);
    input.mPtr = nAttach(input, hwnd, serverChannel, options.scale, options.offsetX,
        options.offsetY, options.displayId, options.maxInFlight, options.initialFocus);
    return input;
  }

  /**
   * Handles a window event as if the window had queued it (an EVENT_* type
   * and the fields of gesher::WindowEvent): for sessions that read the
   * window's queue themselves, and for tests.
   */
  public void inject(int type, float x, float y, int button, int key, int scan, int repeat,
      int modifiers, float delta, float deltaX, int pointer, int tool) {
    nInject(ptr(), type, x, y, button, key, scan, repeat, modifiers, delta, deltaX, pointer,
        tool);
  }

  /** {published, finished, finished as handled, in flight, dropped}. */
  public long[] stats() {
    return nStats(ptr());
  }

  /** Stops the connection; the window's queue is left to others. */
  @Override
  public void close() {
    if (mPtr != 0) {
      nDetach(mPtr);
      mPtr = 0;
    }
  }

  /** PostMessage to a window: tests drive a Gesher window with Win32 messages. */
  public static boolean postMessage(long hwnd, int msg, long wParam, long lParam) {
    return nPostMessage(hwnd, msg, wParam, lParam);
  }

  /** A client point in screen coordinates: {x, y}. */
  public static int[] clientToScreen(long hwnd, int x, int y) {
    long p = nClientToScreen(hwnd, x, y);
    return new int[] {(int) p, (int) (p >> 32)};
  }

  // Called from native code, on the connection's thread.
  private void onWindowEvent(int type, int a, int b) {
    if (mListener != null) {
      mListener.onWindowEvent(type, a, b);
    }
  }

  private long ptr() {
    if (mPtr == 0) {
      throw new IllegalStateException("closed");
    }
    return mPtr;
  }

  private static native long nAttach(WindowInput self, long hwnd, InputChannel channel,
      float scale, float offsetX, float offsetY, int displayId, int maxInFlight,
      boolean initialFocus);
  private static native void nDetach(long ptr);
  private static native void nInject(long ptr, int type, float x, float y, int button, int key,
      int scan, int repeat, int modifiers, float delta, float deltaX, int pointer, int tool);
  private static native long[] nStats(long ptr);
  private static native boolean nPostMessage(long hwnd, int msg, long wParam, long lParam);
  private static native long nClientToScreen(long hwnd, int x, int y);
}
