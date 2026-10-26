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

  /** {@link TextListener}'s kinds (compat/input/text_input.h's TextEvent). */
  public static final int TEXT_COMMIT = 1;
  public static final int TEXT_COMPOSE = 2;
  public static final int TEXT_COMPOSE_END = 3;
  public static final int TEXT_EDITOR_ACTION = 4;
  public static final int DOCUMENT_REPLACE = 5, DOCUMENT_SELECT = 6;
  public static final int DOCUMENT_COMPOSITION_RANGE = 7, DOCUMENT_COMPOSITION_END = 8;
  public static final int DOCUMENT_COMPOSITION_TEXT = 9;

  /**
   * Text for the window's focused editor (docs/input.md, "Text input"), in order with the
   * window's input events: what follows waits until {@link #finishTextEvent} (or a second).
   */
  public interface TextListener {
    /**
     * On the connection's thread. TEXT_COMMIT: {@code text} is typed. TEXT_COMPOSE: {@code
     * text} is the IME's composition ("" when it is cancelled), {@code cursor} its caret,
     * {@code targetStart}/{@code targetEnd} the clause being converted (-1: none).
     * TEXT_COMPOSE_END: the composition ended. TEXT_EDITOR_ACTION: Enter, {@code cursor}
     * the editor's IME action.
     */
    void onTextEvent(WindowInput input, int seq, int kind, String text, int cursor,
        int targetStart, int targetEnd);
    default void onDocumentEdit(WindowInput input, int seq, int kind, String text,
        int epoch, int revision, int start, int end) { input.finishTextEvent(seq); }
  }

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

  private volatile long mPtr;
  private final Listener mListener;
  private volatile TextListener mTextListener;

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

  /**
   * Where text for the window's editor goes; null: typed text reaches the {@link Listener}
   * as EVENT_CHARs, compositions are dropped.
   */
  public void setTextListener(TextListener listener) {
    mTextListener = listener;
  }

  /** The text event {@code seq} is applied: the window's next events may go. */
  public synchronized void finishTextEvent(int seq) {
    long ptr = mPtr;
    if (ptr != 0) nFinishTextEvent(ptr, seq);
  }

  /**
   * What the window's focused editor takes: {@code active}, an editor with an
   * InputConnection has the focus (keys whose character the key map would not type become
   * text, IMEs may compose when {@code ime}); {@code enterAction}, the EditorInfo IME action
   * Enter performs (0: Enter is a key).
   */
  public synchronized void setTextInput(boolean active, boolean ime, int enterAction) {
    setTextInput(active, ime, enterAction, 0);
  }

  public synchronized void setTextInput(boolean active, boolean ime, int enterAction, int epoch) {
    long ptr = mPtr;
    if (ptr != 0) nSetTextInput(ptr, active, ime, enterAction, epoch);
  }

  /** A complete extracted UTF-16 snapshot; start/end are relative to its offset. */
  public synchronized void setDocument(String text, int start, int end, int offset,
      int epoch, int revision, boolean available) {
    long ptr = mPtr;
    if (ptr != 0) nSetDocument(ptr, text, start, end, offset, epoch, revision, available);
  }

  /** Explicit fixture only: loads the CI-specified native Windows text service. */
  public int runTextServiceTest() {
    long ptr;
    synchronized (this) { ptr = mPtr; }
    return ptr != 0 ? nRunTextServiceTest(ptr) : -1;
  }

  /** The editor's caret in client pixels: the system caret and the IME's windows follow it. */
  public synchronized void setCaret(int x, int y, int width, int height) {
    long ptr = mPtr;
    if (ptr != 0) nSetCaret(ptr, x, y, width, height);
  }

  /** Ends the IME's composition: committed, or cancelled. */
  public synchronized void completeComposition(boolean cancel) {
    long ptr = mPtr;
    if (ptr != 0) nCompleteComposition(ptr, cancel);
  }

  /**
   * Shows or hides Windows' touch keyboard for the window (InputPane.TryShow/TryHide): 1
   * when it did, 0 when Windows declined, else a failed HRESULT.
   */
  public int showTouchKeyboard(boolean show) {
    long ptr;
    synchronized (this) {
      ptr = mPtr;
    }
    return ptr != 0 ? nShowTouchKeyboard(ptr, show) : -1;
  }

  /** The session's keyboard layouts: {hkl (hex), BCP 47 locale, display name} each. */
  public static String[][] keyboardLayouts() {
    String[] flat = nKeyboardLayouts();
    if (flat == null) return new String[0][];
    String[][] out = new String[flat.length / 3][];
    for (int i = 0; i < out.length; i++) {
      out[i] = new String[] {flat[i * 3], flat[i * 3 + 1], flat[i * 3 + 2]};
    }
    return out;
  }

  /** The keyboard layout (HKL) of the thread of window {@code hwnd} (0: this thread). */
  public static long keyboardLayout(long hwnd) {
    return nKeyboardLayout(hwnd);
  }

  /** {published, finished, finished as handled, in flight, dropped}. */
  public long[] stats() {
    return nStats(ptr());
  }

  /** Stops the connection; the window's queue is left to others. */
  @Override
  public void close() {
    long ptr;
    synchronized (this) {
      // The input method calls below see it closed; the connection's thread may still be
      // in a listener (nDetach joins it), so the lock is not held for that.
      ptr = mPtr;
      mPtr = 0;
    }
    if (ptr != 0) nDetach(ptr);
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

  // Called from native code, on the connection's thread.
  private void onTextEvent(int seq, int kind, String text, int cursor, int targetStart,
      int targetEnd) {
    TextListener listener = mTextListener;
    if (listener != null) {
      listener.onTextEvent(this, seq, kind, text, cursor, targetStart, targetEnd);
      return;
    }
    if (kind == TEXT_COMMIT && mListener != null && text != null) {
      for (int i = 0; i < text.length(); i++) {
        mListener.onWindowEvent(EVENT_CHAR, text.charAt(i), 0);
      }
    }
    finishTextEvent(seq);
  }

  private void onDocumentEdit(int seq, int kind, String text, int epoch, int revision,
      int start, int end) {
    TextListener listener = mTextListener;
    if (listener != null) listener.onDocumentEdit(this, seq, kind, text, epoch, revision, start, end);
    else finishTextEvent(seq);
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
  private static native void nSetTextInput(long ptr, boolean active, boolean ime,
      int enterAction, int epoch);
  private static native void nSetDocument(long ptr, String text, int start, int end,
      int offset, int epoch, int revision, boolean available);
  private static native int nRunTextServiceTest(long ptr);
  private static native void nSetCaret(long ptr, int x, int y, int width, int height);
  private static native void nCompleteComposition(long ptr, boolean cancel);
  private static native void nFinishTextEvent(long ptr, int seq);
  private static native int nShowTouchKeyboard(long ptr, boolean show);
  private static native String[] nKeyboardLayouts();
  private static native long nKeyboardLayout(long hwnd);
}
