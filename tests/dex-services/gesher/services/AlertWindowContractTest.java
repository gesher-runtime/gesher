/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.graphics.PixelFormat;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.ServiceManager;
import android.view.IWindow;
import android.view.Surface;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager.LayoutParams;
import android.view.WindowManagerGlobal;

import com.android.internal.os.ApplicationSharedMemory;
import com.android.internal.app.IAppOpsService;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.gesher.app.Main;
import com.gesher.app.input.WindowInput;
import com.gesher.app.window.NativeWindow;
import com.gesher.app.window.WindowSession;
import com.gesher.server.ApkPackage;
import com.gesher.server.AppOpsService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import com.gesher.server.StandInWindowSession;

import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

import static gesher.services.ClipboardFixture.*;

/** Real Windows session and AppOps, with isolated package metadata and a headless desktop fixture. */
public final class AlertWindowContractTest {
    private static final String APP = "fixture.alertwindow";
    private static final int OP = AppOpsManager.OP_SYSTEM_ALERT_WINDOW;
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static <T> T on(Handler handler, Callable<T> action) throws Exception {
        FutureTask<T> task = new FutureTask<>(action);
        if (!handler.post(task)) throw new AssertionError("Window Looper stopped");
        return task.get(10, TimeUnit.SECONDS);
    }

    private static IWindow client() {
        return new IWindow.Default() {
            final IBinder token = new Binder();
            @Override public IBinder asBinder() { return token; }
        };
    }

    private static LayoutParams attrs(int type, IBinder token) {
        LayoutParams lp = new LayoutParams(96, 48, type, LayoutParams.FLAG_NOT_FOCUSABLE, PixelFormat.OPAQUE);
        lp.packageName = APP;
        lp.token = token;
        lp.setTitle("Gesher alert fixture");
        return lp;
    }

    private static int add(WindowSession session, IWindow client, LayoutParams attrs) {
        return session.addToDisplayWithoutInputChannel(client, attrs, View.VISIBLE, 0, null);
    }

    private static void draw(WindowSession session, IWindow client) {
        session.relayout(client, null, 96, 48, View.VISIBLE, 0, 0, 0, null, null);
        session.finishDrawing(client, null, 0);
    }

    private static Object nativeState(WindowSession session, IWindow client) throws Exception {
        return call(session, "window", new Class<?>[]{IWindow.class}, client);
    }

    private static boolean visible(WindowSession session, IWindow client) throws Exception {
        Object window = nativeState(session, client);
        return window != null && (boolean)get(window, "shown");
    }

    private static void awaitVisibility(Handler handler, WindowSession session, IWindow parent, IWindow child,
            boolean expected) throws Exception {
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < until) {
            if (on(handler, () -> visible(session, parent) == expected && visible(session, child) == expected)) {
                checks++;
                return;
            }
            Thread.sleep(20);
        }
        throw new AssertionError("An AppOps mode change did not update both Windows surfaces to " + expected);
    }

    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap("AlertWindowContractTest", root + "/build.prop", root + "/framework/framework-res.apk");
        ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();
        int uid = Process.myUid();
        check(uid >= Process.FIRST_APPLICATION_UID, "Fixture must use an application UID");
        ApkPackage pkg = pkg(APP, uid, (byte)21, Manifest.permission.SYSTEM_ALERT_WINDOW);
        PackageManagerService pm = (PackageManagerService) make(PackageManagerService.class,
                new Class<?>[]{Collection.class, boolean.class}, List.of(pkg), false);
        Object atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
        make(type("ActivityManagerService"), new Class<?>[]{atm.getClass(), PackageManagerService.class,
                type("NotificationManagerService")}, atm, pm, null);
        IBinder policy = (IBinder)make(type("AlertWindowPolicy"),
                new Class<?>[]{PackageManagerService.class, atm.getClass()}, pm, atm);
        AppOpsService ops = pm.appOps();
        ServiceManager.initServiceCache(Map.of("gesher.alert_window", policy,
                "appops", StandIn.of(IAppOpsService.class, ops).asBinder()));
        IApplicationThread app = new IApplicationThread.Default() {
            final IBinder token = new Binder();
            @Override public IBinder asBinder() { return token; }
        };
        call(atm, "attachInProcessApplication", new Class<?>[]{ApkPackage.class, IApplicationThread.class}, pkg, app);
        // Windowed tests draw real surfaces off the desktop to avoid affecting the host session.
        WindowSession session = WindowSession.install(false);
        HandlerThread thread = new HandlerThread("alert-window-fixture");
        HandlerThread otherThread = new HandlerThread("alert-child-fixture");
        thread.start();
        otherThread.start();
        Handler handler = new Handler(thread.getLooper());
        Handler otherHandler = new Handler(otherThread.getLooper());
        IWindow parent = client(), child = client(), otherChild = client();
        LayoutParams overlay = attrs(LayoutParams.TYPE_APPLICATION_OVERLAY, null);
        try {
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_DEFAULT);
            check(on(handler, () -> add(session, parent, overlay)) == WindowManagerGlobal.ADD_PERMISSION_DENIED,
                    "Modern manifest declaration does not supply the default-mode permission");
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_ERRORED);
            check(on(handler, () -> add(session, parent, overlay)) == WindowManagerGlobal.ADD_PERMISSION_DENIED,
                    "Modern app cannot add an errored overlay");
            check(on(handler, () -> new StandInWindowSession().addToDisplayWithoutInputChannel(parent,
                    overlay, View.VISIBLE, 0, null)) == WindowManagerGlobal.ADD_PERMISSION_DENIED,
                    "Headless window entry point also enforces admission");
            check(on(handler, () -> nativeState(session, parent)) == null,
                    "Denied window acquired no session state");
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_ALLOWED);
            check(on(handler, () -> add(session, parent, attrs(LayoutParams.TYPE_SYSTEM_ALERT, null)))
                    == WindowManagerGlobal.ADD_PERMISSION_DENIED, "Modern target cannot use legacy alert type");
            LayoutParams rounded = attrs(LayoutParams.TYPE_APPLICATION_OVERLAY, null);
            rounded.privateFlags |= LayoutParams.PRIVATE_FLAG_IS_ROUNDED_CORNERS_OVERLAY;
            check(on(handler, () -> add(session, parent, rounded)) == WindowManagerGlobal.ADD_PERMISSION_DENIED,
                    "An ordinary overlay grant cannot authorize a system rounded-corner overlay");
            check(on(handler, () -> {
                long token = Binder.clearCallingIdentity();
                try {
                    Binder.restoreCallingIdentity(((long)uid << 32) | 774411);
                    return add(session, parent, overlay);
                } finally {
                    Binder.restoreCallingIdentity(token);
                }
            }) == WindowManagerGlobal.ADD_PERMISSION_DENIED, "Unbound caller cannot borrow the app's grant");
            check(on(handler, () -> {
                long token = Binder.clearCallingIdentity();
                try {
                    Binder.restoreCallingIdentity(((long)Process.SYSTEM_UID << 32) | 774412);
                    return new StandInWindowSession().addToDisplayWithoutInputChannel(parent,
                            overlay, View.VISIBLE, 0, null);
                } finally {
                    Binder.restoreCallingIdentity(token);
                }
            }) >= 0, "The trusted platform UI may add its alert");
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_IGNORED);
            on(handler, () -> {
                check(add(session, parent, overlay) >= 0, "Ignored overlay is admitted");
                draw(session, parent);
                check(add(session, child, attrs(LayoutParams.TYPE_APPLICATION_PANEL, parent.asBinder())) >= 0,
                        "Attached child of ignored overlay is admitted");
                draw(session, child);
                check(((NativeWindow)get(nativeState(session, parent), "nativeWindow")).getHwnd() != 0,
                        "Overlay owns a real Win32 surface");
                check(!visible(session, parent) && !visible(session, child), "Ignored app-op keeps both surfaces hidden");
                return null;
            });
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_ALLOWED);
            awaitVisibility(handler, session, parent, child, true);
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_ERRORED);
            awaitVisibility(handler, session, parent, child, false);
            on(handler, () -> {
                draw(session, parent);
                draw(session, child);
                check(!visible(session, parent) && !visible(session, child),
                        "Relayout and another draw cannot re-show revoked surfaces");
                try {
                    session.relayoutAsync(child, overlay, 96, 48, View.VISIBLE, 0, 0, 0);
                    throw new AssertionError("Relayout changed the child's window type");
                } catch (IllegalArgumentException expected) {
                    checks++;
                }
                Class<?>[] nativeEvent = {IBinder.class, int.class, int.class, int.class};
                call(session, "onWindowEvent", nativeEvent, child.asBinder(), WindowInput.EVENT_FOCUS, 1, 0);
                call(session, "onWindowEvent", nativeEvent, child.asBinder(), WindowInput.EVENT_CHAR, 0xd800, 0);
                return null;
            });
            on(handler, () -> {
                Object window = nativeState(session, child);
                check(!(boolean)get(window, "focused") && (char)get(window, "highSurrogate") == 0,
                        "Queued native focus and character input cannot reach a revoked child");
                return null;
            });
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_ALLOWED);
            awaitVisibility(handler, session, parent, child, true);
            Object otherChildState = on(otherHandler, () -> {
                check(add(session, otherChild, attrs(LayoutParams.TYPE_APPLICATION_PANEL, parent.asBinder())) >= 0,
                        "A child can belong to an alert on another window Looper");
                draw(session, otherChild);
                return nativeState(session, otherChild);
            });
            // Keep the owner's HWND alive until both Loopers release their children.
            // Hiding an owner does not hide its owned popups.
            Surface retained = on(handler, () ->
                    ((NativeWindow)get(nativeState(session, parent), "nativeWindow")).getSurface());
            try {
                Object retiredChild = on(handler, () -> {
                    Object parentState = nativeState(session, parent);
                    Object childState = nativeState(session, child);
                    Object operation = get(parentState, "alertOperation");
                    NativeWindow parentWindow = (NativeWindow)get(parentState, "nativeWindow");
                    long parentHwnd = parentWindow.getHwnd();
                    Class<?>[] nativeEvent = {IBinder.class, int.class, int.class, int.class};
                    call(session, "onWindowEvent", nativeEvent, child.asBinder(), WindowInput.EVENT_FOCUS, 1, 0);
                    call(session, "onWindowEvent", nativeEvent, child.asBinder(), WindowInput.EVENT_CHAR, 0xd800, 0);
                    session.remove(parent.asBinder());
                    check(nativeState(session, parent) == null && nativeState(session, child) == null
                                    && nativeState(session, otherChild) == null,
                            "Parent-first removal also retires its attached alert children");
                    check(WindowInput.postMessage(parentHwnd, 0 /* WM_NULL */, 0, 0),
                            "Retained Surface keeps the owner HWND alive during child retirement");
                    check(!(boolean)get(childState, "shown") && get(childState, "nativeWindow") == null
                                    && get(childState, "surfaceControl") == null && get(childState, "queue") == null,
                            "Parent-first removal hides and releases the child's window resources");
                    check(add(session, client(), attrs(LayoutParams.TYPE_APPLICATION_PANEL, parent.asBinder()))
                                    == WindowManagerGlobal.ADD_BAD_SUBWINDOW_TOKEN,
                            "A new subwindow cannot attach to a removed alert parent");
                    draw(session, child);
                    session.remove(child.asBinder());
                    session.remove(parent.asBinder());
                    check(!ops.isOperationActive(OP, uid, APP), "Removing the window retires its active AppOp");
                    call(operation, "refresh", new Class<?>[0]);
                    check(!ops.isOperationActive(OP, uid, APP), "Late refresh cannot restart the removed operation");
                    return childState;
                });
                on(handler, () -> {
                    check(!(boolean)get(retiredChild, "focused") && (char)get(retiredChild, "highSurrogate") == 0,
                            "Queued native focus and character input cannot reach a removed child");
                    return null;
                });
                on(otherHandler, () -> {
                    check(!(boolean)get(otherChildState, "shown") && get(otherChildState, "nativeWindow") == null
                                    && get(otherChildState, "surfaceControl") == null && get(otherChildState, "queue") == null,
                            "An attached child's resources retire on its own window Looper");
                    return null;
                });
            } finally {
                on(handler, () -> { retained.release(); return null; });
            }
            on(handler, () -> {
                ((PackageImpl)pkg.parsed).setTargetSdkVersion(22);
                return null;
            });
            ops.setMode(OP, uid, APP, AppOpsManager.MODE_ERRORED);
            on(handler, () -> {
                check(add(session, parent, attrs(LayoutParams.TYPE_SYSTEM_ALERT, null)) >= 0,
                        "Pre-M target's errored legacy alert remains admissible");
                draw(session, parent);
                check(!visible(session, parent), "Pre-M errored alert remains hidden");
                return null;
            });
        } finally {
            on(otherHandler, () -> { session.remove(otherChild.asBinder()); return null; });
            on(handler, () -> {
                session.remove(child.asBinder());
                session.remove(parent.asBinder());
                return null;
            });
            thread.quitSafely();
            otherThread.quitSafely();
            thread.join(5000);
            otherThread.join(5000);
        }
        check(!thread.isAlive() && !otherThread.isAlive(), "The fixture's window Loopers retired");
        System.out.println("AlertWindowContractTest all passed: " + checks + " checks");
    }
}
