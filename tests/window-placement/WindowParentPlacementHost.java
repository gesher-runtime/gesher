/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.app.Activity;
import android.app.ActivityThread;
import android.content.res.TypedArray;
import android.graphics.Rect;
import android.os.Binder;
import android.os.IBinder;
import android.util.ArrayMap;
import android.util.TypedValue;
import android.view.IWindow;
import android.view.Surface;
import android.view.Window;
import android.view.WindowManager;
import com.gesher.app.window.NativeWindow;
import com.gesher.app.window.WindowSession;
import com.android.internal.R;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import sun.misc.Unsafe;

/** Production Java selection/placement; synthetic admission and native geometry boundaries. */
public final class WindowParentPlacementHost {
    private static final Map<Long, int[]> bounds = new HashMap<>();
    private static final List<String> events = new ArrayList<>();
    private static Unsafe unsafe;
    private static Class<?> windowType;
    private static int checks, cases;
    private static long nextPointer;

    private static Field field(Class<?> type, String name) throws Exception {
        for (Class<?> current = type; current != null; current = current.getSuperclass()) {
            try {
                Field value = current.getDeclaredField(name);
                value.setAccessible(true);
                return value;
            } catch (NoSuchFieldException ignored) { }
        }
        throw new NoSuchFieldException(name);
    }
    private static Object get(Object value, String name) throws Exception {
        return field(value.getClass(), name).get(value);
    }
    private static void set(Object value, String name, Object contents) throws Exception {
        field(value.getClass(), name).set(value, contents);
    }
    private static Object construct(Class<?> type, Class<?>[] types, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(types);
        constructor.setAccessible(true);
        return constructor.newInstance(values);
    }
    private static Object invoke(Object target, String name, Class<?>[] types, Object... values)
            throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, types);
        method.setAccessible(true);
        return method.invoke(target, values);
    }
    private static void require(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    public static int[] screenBounds(long pointer) { return bounds.get(pointer).clone(); }
    public static void position(long pointer, int x, int y) { events.add("position:" + pointer + ":" + x + ":" + y); }
    public static void size(long pointer, int width, int height) { events.add("size:" + pointer + ":" + width + ":" + height); }
    public static void popup(long pointer, long anchor, int x, int y, int width, int height) {
        events.add("popup:" + pointer + ":" + anchor + ":" + x + ":" + y + ":" + width + ":" + height);
    }

    private static final class Record {
        final IBinder token = new Binder(), clientToken = new Binder();
        final Activity activity = (Activity)unsafe.allocateInstance(Activity.class);
        final Object window;
        final WindowManager.LayoutParams attrs;
        final long pointer = ++nextPointer;

        Record(int task, long sequence) throws Exception {
            IWindow client = new IWindow.Default() {
                @Override public IBinder asBinder() { return clientToken; }
            };
            window = construct(windowType, new Class<?>[]{IWindow.class}, client);
            attrs = (WindowManager.LayoutParams)get(window, "attrs");
            attrs.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
            attrs.token = token;
            set(activity, "mToken", token);
            set(window, "taskId", task);
            set(window, "seq", sequence);
            set(window, "shown", true);
            set(window, "nativeWindow", construct(NativeWindow.class, new Class<?>[]{long.class}, pointer));
            bounds.put(pointer, new int[]{0, 0, 400, 800});
        }

        void theme(int attribute) throws Exception {
            // Admit a real TypedArray/PhoneWindow containing one Boolean theme attribute.
            TypedArray style = (TypedArray)unsafe.allocateInstance(TypedArray.class);
            int entries = field(TypedArray.class, "STYLE_NUM_ENTRIES").getInt(null);
            int[] data = new int[R.styleable.Window.length * entries];
            data[attribute * entries + field(TypedArray.class, "STYLE_TYPE").getInt(null)] = TypedValue.TYPE_INT_BOOLEAN;
            data[attribute * entries + field(TypedArray.class, "STYLE_DATA").getInt(null)] = 1;
            set(style, "mData", data);
            set(style, "mLength", R.styleable.Window.length);
            Window androidWindow = (Window)unsafe.allocateInstance(Class.forName("com.android.internal.policy.PhoneWindow"));
            set(androidWindow, "mWindowStyle", style);
            set(activity, "mWindow", androidWindow);
        }
    }

    private static final class Fixture {
        final WindowSession session;
        final Map<IBinder, Object> windows;
        final Map<IBinder, Object> activities = new ArrayMap<>();
        final Record target;

        @SuppressWarnings("unchecked")
        Fixture() throws Exception {
            ActivityThread thread = (ActivityThread)unsafe.allocateInstance(ActivityThread.class);
            set(thread, "mActivities", activities);
            field(ActivityThread.class, "sCurrentActivityThread").set(null, thread);
            session = (WindowSession)construct(WindowSession.class, new Class<?>[]{boolean.class}, true);
            windows = (Map<IBinder, Object>)get(session, "mWindows");
            target = add(7, 100);
            // addToDisplayAsUser selects the new window's parent before admitting it.
            windows.remove(target.clientToken);
            events.clear();
        }

        Record add(int task, long sequence) throws Exception {
            Record record = new Record(task, sequence);
            Class<?> clientType = Class.forName("android.app.ActivityThread$ActivityClientRecord");
            Object client = unsafe.allocateInstance(clientType);
            set(client, "activity", record.activity);
            set(client, "token", record.token);
            activities.put(record.token, client);
            windows.put(record.clientToken, record.window);
            return record;
        }

        Object parent(boolean floating) throws Exception {
            return invoke(session, "findParent", new Class<?>[]{windowType, boolean.class}, target.window, floating);
        }
        void place() throws Exception {
            invoke(session, "placeInTask", new Class<?>[]{windowType}, target.window);
        }
        void expect(String... expected) {
            require(events.equals(Arrays.asList(expected)), "Unexpected geometry calls: " + events);
        }
        void expectPosition(int x, int y) { expect("position:" + target.pointer + ":" + x + ":" + y); }
        void shared(Rect position) throws Exception {
            set(session, "mOneWindow", true);
            set(session, "mAppScreen", position);
        }
    }

    private interface Scenario { void run(Fixture fixture) throws Exception; }
    private static void scenario(String selected, String name, Scenario body, List<String> failures)
            throws Exception {
        if (!selected.equals("all") && !selected.equals(name)) return;
        cases++;
        try {
            body.run(new Fixture());
            System.out.println("PASS " + name);
        } catch (AssertionError failure) {
            failures.add(name + ": " + failure.getMessage());
            System.out.println("FAIL " + name + ": " + failure.getMessage());
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        windowType = Class.forName(WindowSession.class.getName() + "$AppWindow");
        String selected = args[1];
        List<String> failures = new ArrayList<>();
        scenario(selected, "floating-root", f -> require(f.parent(true) == null, "Root became a popup"), failures);
        scenario(selected, "opaque-root", f -> {
            f.add(7, 1); require(f.parent(false) == null, "Opaque activity became a popup");
        }, failures);
        scenario(selected, "live-same-task", f -> {
            f.add(7, 1); Record newest = f.add(7, 2); f.add(8, 3);
            require(f.parent(true) == newest.window, "Did not choose newest live activity of the same task");
        }, failures);
        scenario(selected, "finishing-splash", f -> {
            Record splash = f.add(7, 1); set(splash.activity, "mFinished", true);
            require(f.parent(true) == null, "Finishing splash owns the replacement HWND");
        }, failures);
        scenario(selected, "destroyed-splash", f -> {
            Record splash = f.add(7, 1); set(splash.activity, "mDestroyed", true);
            require(f.parent(true) == null, "Destroyed splash owns the replacement HWND");
        }, failures);
        scenario(selected, "dead-candidates-skipped", f -> {
            Record live = f.add(7, 1), finishing = f.add(7, 2), destroyed = f.add(7, 3);
            set(finishing.activity, "mFinished", true); set(destroyed.activity, "mDestroyed", true);
            require(f.parent(true) == live.window, "Dead activity hides the eligible underlying activity");
        }, failures);
        scenario(selected, "missing-activity", f -> {
            Record stale = f.add(7, 1); f.activities.remove(stale.token);
            require(f.parent(true) == null, "Removed Activity record owns a new window");
        }, failures);
        scenario(selected, "hidden-candidate", f -> {
            Record hidden = f.add(7, 1); set(hidden.window, "shown", false);
            require(f.parent(true) == null, "Hidden activity selected as underlying window");
        }, failures);
        scenario(selected, "unrelated-active-task", f -> {
            Record other = f.add(8, 1); set(f.session, "mActiveRoot", other.window);
            require(f.parent(true) == null, "Foreign active task owns the activity root");
        }, failures);
        scenario(selected, "unresolved-target-task", f -> {
            set(f.target.window, "taskId", -1); f.add(8, 1);
            require(f.parent(true) == null, "Unknown task attached to arbitrary activity");
        }, failures);
        scenario(selected, "unresolved-candidate-task", f -> {
            f.add(-1, 1); require(f.parent(true) == null, "Unknown candidate task owns activity root");
        }, failures);
        scenario(selected, "dialog-token-parent", f -> {
            Record owner = f.add(7, 1); f.target.attrs.type = WindowManager.LayoutParams.TYPE_APPLICATION;
            f.target.attrs.token = owner.token; set(owner.activity, "mFinished", true);
            require(f.parent(false) == owner.window, "Dialog token ownership changed during teardown");
        }, failures);
        scenario(selected, "subwindow-token-parent", f -> {
            Record owner = f.add(7, 1); f.target.attrs.type = WindowManager.LayoutParams.TYPE_APPLICATION_PANEL;
            f.target.attrs.token = owner.clientToken;
            require(f.parent(false) == owner.window, "Subwindow no longer belongs to its IWindow token");
        }, failures);
        scenario(selected, "floating-shared-position", f -> {
            f.target.theme(R.styleable.Window_windowIsFloating); f.shared(new Rect(210, 330, 610, 1130));
            require((Boolean)invoke(f.session, "isTranslucentOrFloating", new Class<?>[]{IBinder.class}, f.target.token),
                    "Fixture did not admit a floating theme");
            f.place(); f.expectPosition(210, 330);
        }, failures);
        scenario(selected, "translucent-shared-position", f -> {
            f.target.theme(R.styleable.Window_windowIsTranslucent); f.shared(new Rect(210, 330, 610, 1130));
            f.place(); f.expectPosition(210, 330);
        }, failures);
        scenario(selected, "live-other-task-position", f -> {
            f.target.theme(R.styleable.Window_windowIsFloating); f.shared(new Rect(10, 20, 410, 820));
            Record other = f.add(8, 1); bounds.put(other.pointer, new int[]{300, 450, 700, 1250});
            f.place(); f.expectPosition(300, 450);
        }, failures);
        scenario(selected, "same-task-placement-first", f -> {
            f.shared(new Rect(10, 20, 410, 820)); Record same = f.add(7, 1);
            bounds.put(same.pointer, new int[]{500, 600, 900, 1400});
            f.place(); f.expectPosition(500, 600);
        }, failures);
        scenario(selected, "separate-task-isolation", f -> {
            f.target.theme(R.styleable.Window_windowIsFloating); f.shared(new Rect(10, 20, 410, 820));
            set(f.session, "mOneWindow", false); f.add(8, 1); f.place(); f.expect();
        }, failures);
        scenario(selected, "separate-task-history", f -> {
            f.shared(new Rect(10, 20, 410, 820)); set(f.session, "mOneWindow", false);
            @SuppressWarnings("unchecked") android.util.SparseArray<Rect> history =
                    (android.util.SparseArray<Rect>)get(f.session, "mTaskScreen");
            history.put(7, new Rect(800, 900, 1200, 1700)); f.place(); f.expectPosition(800, 900);
        }, failures);
        scenario(selected, "resizable-shared-size", f -> {
            f.shared(new Rect(10, 20, 610, 920)); set(f.target.window, "resizable", true);
            f.place(); f.expect("size:" + f.target.pointer + ":600:900", "position:" + f.target.pointer + ":10:20");
        }, failures);
        scenario(selected, "fixed-size-preserved", f -> {
            f.shared(new Rect(10, 20, 610, 920)); f.place(); f.expectPosition(10, 20);
        }, failures);
        scenario(selected, "orientation-preserved", f -> {
            f.shared(new Rect(10, 20, 610, 920)); set(f.target.window, "resizable", true);
            set(f.target.window, "rotation", Surface.ROTATION_90); f.place(); f.expectPosition(10, 20);
        }, failures);
        scenario(selected, "hidden-root-not-positioned", f -> {
            f.shared(new Rect(10, 20, 410, 820));
            f.session.setActivityVisibility(f.target.token, false); f.place(); f.expect();
        }, failures);
        scenario(selected, "popup-keeps-own-anchor", f -> {
            Record own = f.add(7, 1), foreign = f.add(8, 2);
            set(f.target.window, "parent", own.window); set(f.session, "mActiveRoot", foreign.window);
            ((Rect)get(f.target.window, "frame")).set(12, 34, 112, 234);
            invoke(f.session, "place", new Class<?>[]{windowType}, f.target.window);
            f.expect("popup:" + f.target.pointer + ":" + own.pointer + ":12:34:100:200");
        }, failures);
        scenario(selected, "parentless-popup-no-foreign-anchor", f -> {
            Record foreign = f.add(8, 1); set(f.session, "mActiveRoot", foreign.window);
            ((Rect)get(f.target.window, "frame")).set(12, 34, 112, 234);
            invoke(f.session, "place", new Class<?>[]{windowType}, f.target.window);
            f.expect("popup:" + f.target.pointer + ":0:12:34:100:200");
        }, failures);
        require(cases > 0, "Unknown case: " + selected);
        if (!failures.isEmpty()) throw new AssertionError(String.join("; ", failures));
        System.out.println("WindowParentPlacementHost all passed: " + cases + " cases, " + checks + " checks");
    }
}
