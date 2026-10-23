package gesher;

import android.graphics.Matrix;
import android.hardware.input.InputManagerGlobal;
import android.os.HandlerThread;
import android.os.Parcel;
import android.view.InputChannel;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.InputEventReceiver;
import android.view.InputEventSender;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import com.gesher.app.input.VirtualKeyboard;
import com.gesher.app.input.WindowInput;
import com.gesher.app.window.NativeWindow;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Android's input path on Gesher (docs/input.md): the framework's input
 * natives (KeyEvent, MotionEvent, VelocityTracker, KeyCharacterMap with
 * Generic.kcm, InputChannel, InputEventSender and InputEventReceiver on a
 * Looper), then a Gesher Win32 window attached to an InputChannel
 * (com.gesher.app.input.WindowInput): Win32 messages posted to the window,
 * and window events injected with modifiers and touches, arrive in a Java
 * InputEventReceiver as MotionEvents and KeyEvents, which it finishes.
 */
public final class InputTest {
  private static int failures;

  // winuser.h
  private static final int WM_KEYDOWN = 0x0100;
  private static final int WM_KEYUP = 0x0101;
  private static final int WM_MOUSEMOVE = 0x0200;
  private static final int WM_LBUTTONDOWN = 0x0201;
  private static final int WM_LBUTTONUP = 0x0202;
  private static final int WM_RBUTTONDOWN = 0x0204;
  private static final int WM_RBUTTONUP = 0x0205;
  private static final int WM_MOUSEWHEEL = 0x020A;
  private static final int WM_MOUSEHWHEEL = 0x020E;
  private static final long MK_LBUTTON = 1;
  private static final long MK_RBUTTON = 2;

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    try {
      events();
      keyCharacterMap();
      senderAndReceiver();
      window();
    } catch (Throwable t) {
      // Looper threads would keep the VM alive.
      t.printStackTrace();
      System.out.println("InputTest: " + t);
      System.exit(1);
    }
    if (failures == 0) {
      System.out.println("InputTest all passed");
    } else {
      System.out.println("InputTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  /** KeyEvent, MotionEvent and VelocityTracker natives. */
  private static void events() {
    check("KEYCODE_A".equals(KeyEvent.keyCodeToString(KeyEvent.KEYCODE_A))
            && KeyEvent.keyCodeFromString("KEYCODE_ENTER") == KeyEvent.KEYCODE_ENTER,
        "KeyEvent.keyCodeToString/keyCodeFromString (libinput's labels)");

    MotionEvent down = MotionEvent.obtain(100, 100, MotionEvent.ACTION_DOWN, 12.5f, 34f,
        KeyEvent.META_SHIFT_ON);
    down.setSource(InputDevice.SOURCE_TOUCHSCREEN);
    boolean ok = down.getAction() == MotionEvent.ACTION_DOWN && down.getX() == 12.5f
        && down.getY() == 34f && down.getMetaState() == KeyEvent.META_SHIFT_ON
        && down.getEventTime() == 100;
    down.offsetLocation(10, 0);
    ok &= down.getX() == 22.5f;
    Matrix m = new Matrix();
    m.setScale(2, 2);
    down.transform(m);
    ok &= down.getX() == 45f && down.getY() == 68f;
    MotionEvent copy = MotionEvent.obtain(down);
    ok &= copy.getX() == 45f && copy.getSource() == InputDevice.SOURCE_TOUCHSCREEN;
    copy.recycle();
    down.recycle();
    MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[2];
    MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[2];
    for (int i = 0; i < 2; i++) {
      props[i] = new MotionEvent.PointerProperties();
      props[i].id = i;
      props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
      coords[i] = new MotionEvent.PointerCoords();
      coords[i].x = 10 + 100 * i;
      coords[i].y = 20;
    }
    MotionEvent two = MotionEvent.obtain(0, 5, MotionEvent.ACTION_POINTER_DOWN | (1 << 8), 2,
        props, coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
    ok &= two.getPointerCount() == 2 && two.getActionMasked() == MotionEvent.ACTION_POINTER_DOWN
        && two.getActionIndex() == 1 && two.getX(1) == 110f && two.getPointerId(1) == 1;
    Parcel p = Parcel.obtain();
    two.writeToParcel(p, 0);
    p.setDataPosition(0);
    MotionEvent back = MotionEvent.CREATOR.createFromParcel(p);
    p.recycle();
    ok &= back.getPointerCount() == 2 && back.getX(1) == 110f;
    back.recycle();
    two.recycle();
    check(ok, "MotionEvent: obtain, offsetLocation, transform(Matrix), two pointers, Parcel");

    // VelocityTracker asks InputManagerGlobal for its strategy: a device with
    // no input service has none, so the virtual keyboard's stands in.
    InputManagerGlobal.TestSession session =
        InputManagerGlobal.createTestSession(VirtualKeyboard.inputManager());
    VelocityTracker vt = VelocityTracker.obtain();
    for (int i = 0; i <= 6; i++) {
      MotionEvent e = MotionEvent.obtain(0, i * 10, i == 0 ? MotionEvent.ACTION_DOWN
          : MotionEvent.ACTION_MOVE, i * 10f, 50f, 0);
      vt.addMovement(e);
      e.recycle();
    }
    vt.computeCurrentVelocity(1000);
    float vx = vt.getXVelocity();
    float vy = vt.getYVelocity();
    vt.recycle();
    session.close();
    check(Math.abs(vx - 1000f) < 50f && Math.abs(vy) < 1f,
        "VelocityTracker: 10 px every 10 ms is " + vx + " px/s");
  }

  /** Generic.kcm through the KeyCharacterMap natives and InputManagerGlobal. */
  private static void keyCharacterMap() {
    KeyCharacterMap map = VirtualKeyboard.loadKeyCharacterMap(KeyCharacterMap.VIRTUAL_KEYBOARD);
    boolean ok = map.getKeyboardType() == KeyCharacterMap.FULL
        && map.get(KeyEvent.KEYCODE_A, 0) == 'a'
        && map.get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON) == 'A'
        && map.get(KeyEvent.KEYCODE_1, KeyEvent.META_SHIFT_ON) == '!'
        && map.getDisplayLabel(KeyEvent.KEYCODE_Z) == 'Z';
    KeyEvent[] typed = map.getEvents("Hi".toCharArray());
    ok &= typed != null && typed.length >= 4;
    Parcel p = Parcel.obtain();
    map.writeToParcel(p, 0);
    p.setDataPosition(0);
    KeyCharacterMap back = KeyCharacterMap.CREATOR.createFromParcel(p);
    p.recycle();
    ok &= back.get(KeyEvent.KEYCODE_SEMICOLON, KeyEvent.META_SHIFT_ON) == ':';
    check(ok, "KeyCharacterMap (Generic.kcm): FULL; a, A, !, label Z; getEvents(\"Hi\"); Parcel");

    try (InputManagerGlobal.TestSession session =
             InputManagerGlobal.createTestSession(VirtualKeyboard.inputManager())) {
      KeyEvent a = new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A);
      KeyEvent shiftA = new KeyEvent(0, 0, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A, 0,
          KeyEvent.META_SHIFT_ON);
      check(a.getUnicodeChar() == 'a' && shiftA.getUnicodeChar() == 'A'
              && KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD).getKeyboardType()
                  == KeyCharacterMap.FULL,
          "KeyEvent.getUnicodeChar through KeyCharacterMap.load(VIRTUAL_KEYBOARD)");
    }
  }

  /** A receiver that keeps copies of what it gets and finishes each event (handled). */
  private static final class Receiver extends InputEventReceiver {
    final List<InputEvent> events = new ArrayList<>();
    final List<Boolean> focus = new ArrayList<>();
    volatile CountDownLatch done;
    volatile java.util.function.Predicate<InputEvent> last;

    Receiver(InputChannel channel, android.os.Looper looper) {
      super(channel, looper);
    }

    @Override
    public void onInputEvent(InputEvent event) {
      InputEvent copy = event instanceof MotionEvent
          ? MotionEvent.obtain((MotionEvent) event)
          : new KeyEvent((KeyEvent) event);
      synchronized (events) {
        events.add(copy);
      }
      finishInputEvent(event, true);
      if (last != null && last.test(copy) && done != null) {
        done.countDown();
      }
    }

    @Override
    public void onFocusEvent(boolean hasFocus) {
      synchronized (focus) {
        focus.add(hasFocus);
      }
    }

    List<InputEvent> snapshot() {
      synchronized (events) {
        return new ArrayList<>(events);
      }
    }
  }

  /** InputEventSender to InputEventReceiver over an InputChannel pair. */
  private static void senderAndReceiver() throws Exception {
    InputChannel[] pair = InputChannel.openInputChannelPair("gesher sender test");
    check(pair != null && pair.length == 2, "InputChannel.openInputChannelPair");
    HandlerThread thread = new HandlerThread("input sender test");
    thread.start();
    CountDownLatch finished = new CountDownLatch(2);
    List<String> results = new ArrayList<>();
    InputEventSender sender = new InputEventSender(pair[0], thread.getLooper()) {
      @Override
      public void onInputEventFinished(int seq, boolean handled) {
        synchronized (results) {
          results.add(seq + ":" + handled);
        }
        finished.countDown();
      }
    };
    Receiver receiver = new Receiver(pair[1], thread.getLooper());
    KeyEvent key = new KeyEvent(1000, 1000, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_B, 0,
        KeyEvent.META_CTRL_ON | KeyEvent.META_CTRL_LEFT_ON);
    MotionEvent motion = MotionEvent.obtain(2000, 2000, MotionEvent.ACTION_DOWN, 7f, 8f, 0);
    motion.setSource(InputDevice.SOURCE_TOUCHSCREEN);
    boolean sent = sender.sendInputEvent(11, key) && sender.sendInputEvent(12, motion);
    boolean all = finished.await(10, TimeUnit.SECONDS);
    List<InputEvent> got = receiver.snapshot();
    boolean ok = sent && all && got.size() == 2 && got.get(0) instanceof KeyEvent
        && ((KeyEvent) got.get(0)).getKeyCode() == KeyEvent.KEYCODE_B
        && ((KeyEvent) got.get(0)).isCtrlPressed()
        && got.get(1) instanceof MotionEvent && ((MotionEvent) got.get(1)).getX() == 7f
        && results.contains("11:true") && results.contains("12:true");
    check(ok, "InputEventSender to InputEventReceiver on a Looper: a key and a motion, finished"
        + " as handled " + results);
    onThread(thread, () -> {
      receiver.dispose();
      sender.dispose();
    });
    pair[0].dispose();
    pair[1].dispose();
    thread.quitSafely();
  }

  /** Runs r on the thread's Looper (receivers are disposed there) and waits. */
  private static void onThread(HandlerThread thread, Runnable r) throws InterruptedException {
    CountDownLatch ran = new CountDownLatch(1);
    new android.os.Handler(thread.getLooper()).post(() -> {
      r.run();
      ran.countDown();
    });
    ran.await(10, TimeUnit.SECONDS);
  }

  private static long point(int x, int y) {
    return ((long) (y & 0xffff) << 16) | (x & 0xffff);
  }

  private static int masked(InputEvent e) {
    return e instanceof MotionEvent ? ((MotionEvent) e).getActionMasked() : -1;
  }

  private static boolean isHover(InputEvent e) {
    int a = masked(e);
    return a == MotionEvent.ACTION_HOVER_ENTER || a == MotionEvent.ACTION_HOVER_MOVE
        || a == MotionEvent.ACTION_HOVER_EXIT;
  }

  /** A Gesher window's Win32 messages as Android events in a Java InputEventReceiver. */
  private static void window() throws Exception {
    NativeWindow window = NativeWindow.create("Gesher input test", 320, 240);
    long hwnd = window.getHwnd();
    InputChannel[] pair = InputChannel.openInputChannelPair("gesher window input test");
    HandlerThread thread = new HandlerThread("input window test");
    thread.start();
    Receiver receiver = new Receiver(pair[1], thread.getLooper());
    CountDownLatch done = new CountDownLatch(1);
    receiver.done = done;
    // The last event: the injected two-finger gesture's UP.
    receiver.last = e -> e instanceof MotionEvent
        && ((MotionEvent) e).getActionMasked() == MotionEvent.ACTION_UP
        && ((MotionEvent) e).getSource() == InputDevice.SOURCE_TOUCHSCREEN
        && ((MotionEvent) e).getPointerId(0) == 1;
    List<String> windowEvents = new ArrayList<>();
    WindowInput.Options options = new WindowInput.Options();
    options.initialFocus = true;
    options.listener = (type, a, b) -> {
      synchronized (windowEvents) {
        windowEvents.add(type + ":" + a);
      }
    };
    WindowInput input = WindowInput.attach(hwnd, pair[0], options);

    int[] wheel = WindowInput.clientToScreen(hwnd, 30, 40);
    boolean posted = WindowInput.postMessage(hwnd, WM_MOUSEMOVE, 0, point(10, 20))
        && WindowInput.postMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point(40, 50))
        && WindowInput.postMessage(hwnd, WM_MOUSEMOVE, MK_LBUTTON, point(60, 70))
        && WindowInput.postMessage(hwnd, WM_RBUTTONDOWN, MK_LBUTTON | MK_RBUTTON, point(60, 70))
        && WindowInput.postMessage(hwnd, WM_RBUTTONUP, MK_LBUTTON, point(60, 70))
        && WindowInput.postMessage(hwnd, WM_LBUTTONUP, 0, point(60, 70))
        && WindowInput.postMessage(hwnd, WM_MOUSEWHEEL, -120L << 16, point(wheel[0], wheel[1]))
        && WindowInput.postMessage(hwnd, WM_MOUSEHWHEEL, 240L << 16, point(wheel[0], wheel[1]))
        && WindowInput.postMessage(hwnd, WM_KEYDOWN, 'A', 0x001E0001L)
        && WindowInput.postMessage(hwnd, WM_KEYDOWN, 'A', 0x401E0001L)
        && WindowInput.postMessage(hwnd, WM_KEYUP, 'A', 0xC01E0001L)
        && WindowInput.postMessage(hwnd, WM_KEYDOWN, 0x0D, 0x011C0001L)  // keypad Enter
        && WindowInput.postMessage(hwnd, WM_KEYUP, 0x0D, 0xC11C0001L);
    check(posted, "PostMessage: mouse, wheel and key messages to the Gesher window");
    // Posted messages carry no keyboard state, and Windows does not post
    // WM_POINTER*: modifiers and touches are injected as window events (what
    // the window's WM_POINTER handling queues), after the posted ones.
    Thread.sleep(500);
    input.inject(WindowInput.EVENT_TOUCH_DOWN, 100, 110, 0, 0, 0, 0, 0, 0, 0, 4242, 1);
    input.inject(WindowInput.EVENT_TOUCH_MOVE, 120, 130, 0, 0, 0, 0, 0, 0, 0, 4242, 1);
    input.inject(WindowInput.EVENT_TOUCH_UP, 120, 130, 0, 0, 0, 0, 0, 0, 0, 4242, 1);
    input.inject(WindowInput.EVENT_KEY_DOWN, 0, 0, 0, 'A', 0x1E, 0, WindowInput.MODIFIER_SHIFT,
        0, 0, 0, 0);
    input.inject(WindowInput.EVENT_KEY_UP, 0, 0, 0, 'A', 0x1E, 0, WindowInput.MODIFIER_SHIFT,
        0, 0, 0, 0);
    input.inject(WindowInput.EVENT_MOUSE_DOWN, 5, 6, 1, 0, 0, 0, WindowInput.MODIFIER_CTRL, 0, 0,
        0, 0);
    input.inject(WindowInput.EVENT_MOUSE_UP, 5, 6, 1, 0, 0, 0, WindowInput.MODIFIER_CTRL, 0, 0,
        0, 0);
    input.inject(WindowInput.EVENT_TOUCH_DOWN, 50, 60, 0, 0, 0, 0, 0, 0, 0, 501, 1);
    input.inject(WindowInput.EVENT_TOUCH_DOWN, 150, 160, 0, 0, 0, 0, 0, 0, 0, 502, 1);
    input.inject(WindowInput.EVENT_TOUCH_MOVE, 155, 165, 0, 0, 0, 0, 0, 0, 0, 502, 1);
    input.inject(WindowInput.EVENT_TOUCH_UP, 50, 60, 0, 0, 0, 0, 0, 0, 0, 501, 1);
    input.inject(WindowInput.EVENT_TOUCH_UP, 155, 165, 0, 0, 0, 0, 0, 0, 0, 502, 1);
    boolean arrived = done.await(15, TimeUnit.SECONDS);
    check(arrived, "the window's events reached the receiver");

    List<InputEvent> all = receiver.snapshot();
    List<InputEvent> seq = new ArrayList<>();
    boolean hoverEnter = false;
    for (InputEvent e : all) {
      if (masked(e) == MotionEvent.ACTION_HOVER_ENTER && ((MotionEvent) e).getX() == 10f
          && ((MotionEvent) e).getY() == 20f
          && ((MotionEvent) e).getSource() == InputDevice.SOURCE_MOUSE) {
        hoverEnter = true;
      }
      if (!isHover(e)) {
        seq.add(e);
      }
    }
    for (InputEvent e : seq) {
      System.out.println("  " + e);
    }
    synchronized (receiver.focus) {
      check(receiver.focus.contains(Boolean.TRUE), "the initial focus event: onFocusEvent(true)");
    }
    check(hoverEnter, "WM_MOUSEMOVE without buttons: ACTION_HOVER_ENTER at (10, 20), SOURCE_MOUSE");

    int i = 0;
    boolean mouse = seq.size() >= 8;
    if (mouse) {
      MotionEvent d = (MotionEvent) seq.get(0);
      mouse = d.getActionMasked() == MotionEvent.ACTION_DOWN && d.getX() == 40f && d.getY() == 50f
          && d.getRawX() == 40f && d.getSource() == InputDevice.SOURCE_MOUSE
          && d.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE
          && d.getButtonState() == MotionEvent.BUTTON_PRIMARY
          && d.getDeviceId() == 1;
      MotionEvent press = (MotionEvent) seq.get(1);
      mouse &= press.getActionMasked() == MotionEvent.ACTION_BUTTON_PRESS
          && press.getActionButton() == MotionEvent.BUTTON_PRIMARY;
      MotionEvent move = (MotionEvent) seq.get(2);
      mouse &= move.getActionMasked() == MotionEvent.ACTION_MOVE && move.getX() == 60f
          && move.getY() == 70f && move.getDownTime() == d.getDownTime();
      MotionEvent right = (MotionEvent) seq.get(3);
      mouse &= right.getActionMasked() == MotionEvent.ACTION_BUTTON_PRESS
          && right.getActionButton() == MotionEvent.BUTTON_SECONDARY
          && right.isButtonPressed(MotionEvent.BUTTON_PRIMARY)
          && right.isButtonPressed(MotionEvent.BUTTON_SECONDARY);
      MotionEvent rightUp = (MotionEvent) seq.get(4);
      mouse &= rightUp.getActionMasked() == MotionEvent.ACTION_BUTTON_RELEASE
          && rightUp.getActionButton() == MotionEvent.BUTTON_SECONDARY;
      mouse &= masked(seq.get(5)) == MotionEvent.ACTION_BUTTON_RELEASE
          && masked(seq.get(6)) == MotionEvent.ACTION_UP
          && ((MotionEvent) seq.get(6)).getButtonState() == 0;
      i = 7;
    }
    check(mouse, "left and right buttons: DOWN, BUTTON_PRESS, MOVE, BUTTON_PRESS (secondary),"
        + " BUTTON_RELEASE x2, UP (window pixels, SOURCE_MOUSE)");

    boolean wheelOk = seq.size() >= i + 2;
    if (wheelOk) {
      MotionEvent v = (MotionEvent) seq.get(i);
      MotionEvent h = (MotionEvent) seq.get(i + 1);
      wheelOk = v.getActionMasked() == MotionEvent.ACTION_SCROLL && v.getX() == 30f
          && v.getY() == 40f && v.getAxisValue(MotionEvent.AXIS_VSCROLL) == -1f
          && h.getActionMasked() == MotionEvent.ACTION_SCROLL
          && h.getAxisValue(MotionEvent.AXIS_HSCROLL) == 2f;
      i += 2;
    }
    check(wheelOk, "WM_MOUSEWHEEL / WM_MOUSEHWHEEL: ACTION_SCROLL, AXIS_VSCROLL -1, AXIS_HSCROLL 2");

    boolean keys = seq.size() >= i + 5;
    try (InputManagerGlobal.TestSession session =
             InputManagerGlobal.createTestSession(VirtualKeyboard.inputManager())) {
      if (keys) {
        KeyEvent k0 = (KeyEvent) seq.get(i);
        KeyEvent k1 = (KeyEvent) seq.get(i + 1);
        KeyEvent k2 = (KeyEvent) seq.get(i + 2);
        KeyEvent enter = (KeyEvent) seq.get(i + 3);
        keys = k0.getAction() == KeyEvent.ACTION_DOWN && k0.getKeyCode() == KeyEvent.KEYCODE_A
            && k0.getScanCode() == 30 && k0.getRepeatCount() == 0
            && k0.getDeviceId() == KeyCharacterMap.VIRTUAL_KEYBOARD
            && k0.getSource() == InputDevice.SOURCE_KEYBOARD && k0.getUnicodeChar() == 'a'
            && k1.getAction() == KeyEvent.ACTION_DOWN && k1.getRepeatCount() == 1
            && k2.getAction() == KeyEvent.ACTION_UP && k2.getKeyCode() == KeyEvent.KEYCODE_A
            && k2.getDownTime() == k0.getDownTime()
            && enter.getKeyCode() == KeyEvent.KEYCODE_NUMPAD_ENTER && enter.getScanCode() == 96;
        i += 5;
      }
      check(keys, "WM_KEYDOWN/KEYUP: KEYCODE_A (scan 30, repeat 0 then 1, getUnicodeChar 'a'),"
          + " keypad Enter (KEYCODE_NUMPAD_ENTER, scan 96)");

      // A finger, injected after the posted messages were handled.
      boolean touch = seq.size() >= i + 3;
      if (touch) {
        MotionEvent td = (MotionEvent) seq.get(i);
        MotionEvent tm = (MotionEvent) seq.get(i + 1);
        MotionEvent tu = (MotionEvent) seq.get(i + 2);
        touch = td.getActionMasked() == MotionEvent.ACTION_DOWN
            && td.getSource() == InputDevice.SOURCE_TOUCHSCREEN
            && td.getToolType(0) == MotionEvent.TOOL_TYPE_FINGER && td.getX() == 100f
            && td.getY() == 110f && tm.getActionMasked() == MotionEvent.ACTION_MOVE
            && tm.getX() == 120f && tu.getActionMasked() == MotionEvent.ACTION_UP;
      }
      check(touch, "touch: DOWN, MOVE, UP (SOURCE_TOUCHSCREEN, finger)");
      i += 3;

      // Injected: shift+A down/up; ctrl+click DOWN, BUTTON_PRESS, BUTTON_RELEASE, UP.
      boolean modifiers = seq.size() >= i + 6;
      if (modifiers) {
        KeyEvent shiftA = (KeyEvent) seq.get(i);
        MotionEvent ctrlDown = (MotionEvent) seq.get(i + 2);
        modifiers = shiftA.isShiftPressed()
            && (shiftA.getMetaState() & KeyEvent.META_SHIFT_LEFT_ON) != 0
            && shiftA.getUnicodeChar() == 'A'
            && ctrlDown.getActionMasked() == MotionEvent.ACTION_DOWN
            && (ctrlDown.getMetaState() & KeyEvent.META_CTRL_ON) != 0;
      }
      check(modifiers, "meta state: shift+A (isShiftPressed, 'A'), ctrl+click (META_CTRL_ON)");
    }

    // The injected two-finger gesture, found by its POINTER_DOWN.
    int pd = -1;
    for (int k = 0; k < seq.size(); k++) {
      if (masked(seq.get(k)) == MotionEvent.ACTION_POINTER_DOWN) {
        pd = k;
        break;
      }
    }
    boolean fingers = pd > 0 && seq.size() >= pd + 4;
    if (fingers) {
      MotionEvent first = (MotionEvent) seq.get(pd - 1);
      MotionEvent second = (MotionEvent) seq.get(pd);
      MotionEvent move = (MotionEvent) seq.get(pd + 1);
      MotionEvent up0 = (MotionEvent) seq.get(pd + 2);
      MotionEvent up = (MotionEvent) seq.get(pd + 3);
      fingers = first.getActionMasked() == MotionEvent.ACTION_DOWN && first.getX() == 50f
          && second.getActionIndex() == 1 && second.getPointerCount() == 2
          && second.getPointerId(1) == 1 && second.getX(1) == 150f
          && move.getActionMasked() == MotionEvent.ACTION_MOVE && move.getX(1) == 155f
          && up0.getActionMasked() == MotionEvent.ACTION_POINTER_UP && up0.getActionIndex() == 0
          && up.getActionMasked() == MotionEvent.ACTION_UP && up.getPointerCount() == 1
          && up.getPointerId(0) == 1;
    }
    check(fingers, "two fingers: DOWN, POINTER_DOWN (index 1), MOVE, POINTER_UP (index 0), UP");

    // Every event was finished (handled): the connection read the signals.
    long[] stats = input.stats();
    for (int k = 0; k < 100 && (stats[1] < all.size() || stats[3] != 0); k++) {
      Thread.sleep(20);
      stats = input.stats();
    }
    System.out.println("  published " + stats[0] + ", finished " + stats[1] + " (handled "
        + stats[2] + "), in flight " + stats[3] + ", dropped " + stats[4] + "; window events "
        + windowEvents);
    check(stats[1] >= all.size() && stats[2] == stats[1] && stats[4] == 0,
        "finished signals: the connection saw every event finished as handled, none dropped");

    input.close();
    onThread(thread, receiver::dispose);
    pair[0].dispose();
    pair[1].dispose();
    thread.quitSafely();
    window.close();
  }

  private static void check(boolean ok, String what) {
    System.out.println((ok ? "PASS  " : "FAIL  ") + what);
    if (!ok) {
      failures++;
    }
  }
}
