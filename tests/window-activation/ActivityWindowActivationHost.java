/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.app.Activity;
import android.app.ActivityThread;
import android.app.Application;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.util.ArrayMap;
import android.view.IWindow;
import android.view.WindowManager;
import com.gesher.app.window.NativeWindow;
import com.gesher.app.window.WindowSession;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import sun.misc.Unsafe;

/** Full production Java dispatch; fixture admission, queue and native-output boundaries. */
public final class ActivityWindowActivationHost {
    private static final List<String> events = new ArrayList<>();
    private static Unsafe unsafe;
    private static int checks;
    private static int cases;
    private static long nextPointer;

    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    private static void set(Object value, String name, Object contents) throws Exception {
        field(value.getClass(), name).set(value, contents);
    }

    private static Object get(Object value, String name) throws Exception {
        return field(value.getClass(), name).get(value);
    }

    private static Object construct(Class<?> type, Class<?>[] types, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(types);
        c.setAccessible(true);
        return c.newInstance(args);
    }

    private static void require(boolean condition, String why) {
        if (!condition) throw new AssertionError(why);
        checks++;
    }

    public static void recordNative(long pointer, int operation) {
        events.add((operation == 2 ? "restore:" : operation == 1 ? "show:" : "activate:") + pointer);
    }

    /** Handler.post and Message are real; only queue scheduling is controlled. */
    private static final class QueueHandler extends Handler {
        ArrayDeque<Runnable> pending;
        private QueueHandler() { super((Looper)null); }
        @Override public boolean sendMessageAtTime(Message message, long when) {
            Runnable callback = message.getCallback();
            if (callback == null) throw new AssertionError("Unexpected non-callback message");
            pending.add(callback);
            return true;
        }
        void drain() {
            int remaining = 20;
            while (!pending.isEmpty()) {
                if (--remaining == 0) throw new AssertionError("Unbounded activation queue");
                pending.remove().run();
            }
        }
    }

    private static final class Fixture {
        final IBinder activityToken = new Binder();
        final IBinder windowToken = new Binder();
        final Application application = new Application();
        final Activity activity;
        final ActivityThread thread;
        final WindowSession session;
        final Object window;
        final QueueHandler handler;
        final NativeWindow nativeWindow;
        final long pointer = ++nextPointer;
        final Map<IBinder, Object> windows;
        final Map<IBinder, Object> activities;
        final WindowManager.LayoutParams attrs;

        Fixture() throws Exception { this(true); }

        @SuppressWarnings("unchecked")
        Fixture(boolean show) throws Exception {
            activity = (Activity)unsafe.allocateInstance(Activity.class);
            set(activity, "mToken", activityToken);
            set(activity, "mResumed", true);
            thread = (ActivityThread)unsafe.allocateInstance(ActivityThread.class);
            activities = new ArrayMap<>();
            set(thread, "mActivities", activities);
            set(thread, "mInitialApplication", application);
            field(ActivityThread.class, "sCurrentActivityThread").set(null, thread);
            Class<?> recordType = Class.forName("android.app.ActivityThread$ActivityClientRecord");
            Object record = unsafe.allocateInstance(recordType);
            set(record, "activity", activity);
            set(record, "token", activityToken);
            activities.put(activityToken, record);
            session = (WindowSession)construct(WindowSession.class, new Class<?>[]{boolean.class}, show);
            field(WindowSession.class, "sInstance").set(null, session);
            windows = (Map<IBinder, Object>)get(session, "mWindows");
            Class<?> windowType = Class.forName(WindowSession.class.getName() + "$AppWindow");
            IWindow client = new IWindow.Default() {
                @Override public IBinder asBinder() { return windowToken; }
            };
            window = construct(windowType, new Class<?>[]{IWindow.class}, client);
            attrs = (WindowManager.LayoutParams)get(window, "attrs");
            attrs.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
            attrs.token = activityToken;
            nativeWindow = nativeWindow(pointer);
            set(window, "nativeWindow", nativeWindow);
            set(window, "shown", true);
            set(window, "drawn", true);
            handler = (QueueHandler)unsafe.allocateInstance(QueueHandler.class);
            handler.pending = new ArrayDeque<>();
            field(Handler.class, "mLooper").set(handler, unsafe.allocateInstance(Looper.class));
            set(window, "handler", handler);
            windows.put(windowToken, window);
            events.clear();
        }

        void resume() throws Exception {
            session.activateActivity(activityToken);
            require(events.isEmpty(), "Activation ran before the queued UI work: " + events);
        }

        void register() throws Exception {
            Method watch = WindowSession.class.getDeclaredMethod("watchResumedActivities");
            watch.setAccessible(true);
            watch.invoke(session);
        }

        int registered() throws Exception {
            return ((List<?>)field(Application.class, "mActivityLifecycleCallbacks").get(application)).size();
        }

        void postResume() throws Exception {
            Method dispatch = Application.class.getDeclaredMethod("dispatchActivityPostResumed", Activity.class);
            dispatch.setAccessible(true);
            dispatch.invoke(application, activity);
            require(events.isEmpty(), "Lifecycle callback bypassed the window handler: " + events);
        }

        void expectNoNative() {
            handler.drain();
            require(events.isEmpty(), "Stale/ineligible resume activated a window: " + events);
        }

        void expect(String operation) {
            handler.drain();
            require(events.equals(List.of(operation + ":" + pointer)),
                    "Wrong native operation/order: " + events);
        }
    }

    private static NativeWindow nativeWindow(long pointer) throws Exception {
        return (NativeWindow)construct(NativeWindow.class, new Class<?>[]{long.class}, pointer);
    }

    private interface Scenario { void run(Fixture f) throws Exception; }

    private static void scenario(String selected, String name, Scenario body, List<String> failures)
            throws Exception {
        if (!selected.equals("all") && !selected.equals(name)) return;
        cases++;
        try {
            body.run(new Fixture());
            System.out.println("PASS " + name);
        } catch (AssertionError e) {
            failures.add(name + ": " + e.getMessage());
            System.out.println("FAIL " + name + ": " + e.getMessage());
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        String selected = args[1];
        List<String> failures = new ArrayList<>();
        scenario(selected, "restore-minimized", f -> {
            set(f.window, "minimized", true);
            f.resume();
            f.expect("restore");
            require(Boolean.FALSE.equals(get(f.window, "minimized")), "Restored window retained minimized state");
        }, failures);
        scenario(selected, "activate-unfocused", f -> { f.resume(); f.expect("activate"); }, failures);
        scenario(selected, "focused-noop", f -> {
            set(f.window, "focused", true); f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "null-token", f -> {
            f.session.activateActivity(null); f.expectNoNative();
        }, failures);
        scenario(selected, "unknown-token", f -> {
            f.session.activateActivity(new Binder()); f.expectNoNative();
        }, failures);
        scenario(selected, "popup-noop", f -> {
            set(f.window, "popup", true); f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "nonbase-noop", f -> {
            f.attrs.type = WindowManager.LayoutParams.TYPE_APPLICATION; f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "nonfocusable-noop", f -> {
            f.attrs.flags |= WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
            f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "hidden-noop", f -> {
            set(f.window, "shown", false); f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "missing-handler", f -> {
            set(f.window, "handler", null); f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "visibility-denied", f -> {
            f.session.setActivityVisibility(f.activityToken, false); f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "selective-unapproved", ignored -> {
            Fixture f = new Fixture(false);
            f.resume(); f.expectNoNative();
        }, failures);
        scenario(selected, "selective-approved", ignored -> {
            Fixture f = new Fixture(false);
            f.session.setActivityVisibility(f.activityToken, true);
            f.resume(); f.expect("activate");
        }, failures);
        scenario(selected, "visibility-changed", f -> {
            f.resume(); f.session.setActivityVisibility(f.activityToken, false); f.expectNoNative();
        }, failures);
        scenario(selected, "hidden-after-post", f -> {
            f.resume(); set(f.window, "shown", false); f.expectNoNative();
        }, failures);
        scenario(selected, "record-removed", f -> {
            f.resume(); f.windows.remove(f.windowToken); f.expectNoNative();
        }, failures);
        scenario(selected, "record-replaced", f -> {
            f.resume();
            f.windows.put(f.windowToken, unsafe.allocateInstance(f.window.getClass()));
            f.expectNoNative();
        }, failures);
        scenario(selected, "native-removed", f -> {
            f.resume(); set(f.window, "nativeWindow", null); f.expectNoNative();
        }, failures);
        scenario(selected, "native-replaced", f -> {
            f.resume(); set(f.window, "nativeWindow", nativeWindow(++nextPointer)); f.expectNoNative();
        }, failures);
        scenario(selected, "activity-missing", f -> {
            f.resume(); f.activities.remove(f.activityToken); f.expectNoNative();
        }, failures);
        scenario(selected, "activity-paused", f -> {
            f.resume(); set(f.activity, "mResumed", false); f.expectNoNative();
        }, failures);
        scenario(selected, "activity-finishing", f -> {
            f.resume(); set(f.activity, "mFinished", true); f.expectNoNative();
        }, failures);
        scenario(selected, "activity-destroyed", f -> {
            f.resume(); set(f.activity, "mDestroyed", true); f.expectNoNative();
        }, failures);
        scenario(selected, "post-resume-callback", f -> {
            f.register();
            require(f.registered() == 1, "Session did not register exactly one lifecycle callback");
            f.postResume();
            f.expect("activate");
        }, failures);
        scenario(selected, "post-resume-minimized", f -> {
            f.register();
            set(f.window, "minimized", true);
            f.postResume();
            f.expect("restore");
        }, failures);
        scenario(selected, "registration-once", f -> {
            f.register(); f.register(); f.register();
            require(f.registered() == 1, "Repeated window admission duplicated lifecycle callbacks");
            f.postResume();
            f.expect("activate");
        }, failures);
        scenario(selected, "registration-retry", f -> {
            set(f.thread, "mInitialApplication", null);
            f.register();
            require(f.registered() == 0, "Callback registered without the current Application");
            set(f.thread, "mInitialApplication", f.application);
            f.register();
            require(f.registered() == 1, "Early missing Application permanently disabled activation");
            f.postResume();
            f.expect("activate");
        }, failures);
        require(cases > 0, "Unknown fixture case: " + selected);
        if (!failures.isEmpty()) throw new AssertionError(String.join("\n", failures));
        System.out.println("Activity window activation host passed: " + cases + " cases, " + checks + " checks");
        System.out.println("Full production Application callback/session/native wrapper; explicit admission, queue and JNI observation boundaries. Win32 restoration and ART lifecycle remain device checks.");
    }
}
