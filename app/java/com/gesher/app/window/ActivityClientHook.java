/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.app.ActivityClient;
import android.app.ActivityTaskManager;
import android.app.IActivityClientController;
import android.os.IBinder;
import android.os.RemoteException;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/**
 * What an app process's Activities ask of the activity task manager about themselves
 * (ActivityClient's IActivityClientController), seen on the way (windows3 track,
 * docs/windows3.md): the calls go to the system server as before, and
 * <ul>
 *   <li>{@code moveActivityTaskToBack} that moved the task also minimizes its windows here
 *       (the process draws them: WindowSession.minimizeTask), as a desktop's window manager
 *       does with a task sent to the back, instead of their being hidden as stopped
 *       Activities' windows are;
 *   <li>{@code onBackPressed} (oneway) on a task root the home screen launched moves the task
 *       to the back in the system server (Android's rule): its windows are minimized here
 *       when its Activity then stops (WindowSession.backPressed);
 *   <li>gesher-app's single process, which has no system server for its one Activity
 *       (its token is its own), answers for that Activity itself ({@link Local}):
 *       {@code setRequestedOrientation} turns its window, {@code getRequestedOrientation},
 *       {@code moveActivityTaskToBack} minimizes its window.
 * </ul>
 */
public final class ActivityClientHook implements InvocationHandler {
    /** What a single process does for its own Activity. */
    public interface Local {
        void setRequestedOrientation(IBinder token, int requestedOrientation);

        int getRequestedOrientation(IBinder token);
    }

    private static boolean sInstalled;
    private static IActivityClientController sHook;
    private static volatile IBinder sLocalToken;
    private static volatile Local sLocal;

    private volatile IActivityClientController mServer;

    private ActivityClientHook() {
    }

    /** Puts the hook between ActivityClient and the system server's controller. */
    public static synchronized void install() {
        if (sInstalled) return;
        sInstalled = true;
        IActivityClientController hook = (IActivityClientController) Proxy.newProxyInstance(
                IActivityClientController.class.getClassLoader(),
                new Class<?>[] {IActivityClientController.class}, new ActivityClientHook());
        sHook = hook;
        ActivityClient.setActivityClientController(hook);
    }

    /**
     * Puts the hook back: each LaunchActivityItem sets ActivityClient's controller to the one
     * it carries (the system server's). The window session calls it as windows are added and
     * laid out, so the hook is there when their Activities call.
     */
    public static void ensure() {
        IActivityClientController hook = sHook;
        if (hook != null) ActivityClient.setActivityClientController(hook);
    }

    /** The single process's Activity: its token, and what answers for it. */
    public static void setLocal(IBinder token, Local local) {
        sLocal = local;
        sLocalToken = token;
    }

    /** Gesher's extension call on the server's controller (com.gesher.server.StandIn). */
    public static final String WINDOWS_DRAWN_DESCRIPTOR = "com.gesher.IActivityWindowsDrawn";
    public static final int WINDOWS_DRAWN = com.gesher.server.StandIn.FIRST_EXTENSION_TRANSACTION;

    private static volatile IBinder sServerBinder;

    /**
     * Tells the system server that the first frame of an Activity's window ({@code token}:
     * the Activity's) was drawn, as WindowManagerService learns it in Android
     * (ActivityRecord.onWindowsDrawn): what completes a launch the server waits for
     * (startActivityAndWait), even if the Activity's main thread is never idle. One way, as
     * activityIdle is; gesher-app's single-process Activity has no server to tell.
     */
    public static void reportWindowsDrawn(IBinder token) {
        if (token == null || token == sLocalToken) return;
        android.os.Parcel data = android.os.Parcel.obtain();
        try {
            IBinder server = sServerBinder;
            if (server == null) {
                android.app.IActivityTaskManager atm = ActivityTaskManager.getService();
                IActivityClientController controller =
                        atm != null ? atm.getActivityClientController() : null;
                if (controller == null) return;
                server = controller.asBinder();
                sServerBinder = server;
            }
            data.writeInterfaceToken(WINDOWS_DRAWN_DESCRIPTOR);
            data.writeStrongBinder(token);
            server.transact(WINDOWS_DRAWN, data, null, IBinder.FLAG_ONEWAY);
        } catch (RemoteException | RuntimeException e) {
            System.err.println("gesher-app: reporting the first frame of activity " + token
                    + " to the system server: " + e);
        } finally {
            data.recycle();
        }
    }

    private IActivityClientController server() throws RemoteException {
        IActivityClientController s = mServer;
        if (s == null) {
            s = ActivityTaskManager.getService().getActivityClientController();
            mServer = s;
        }
        return s;
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        String name = method.getName();
        if (method.getDeclaringClass() == Object.class) {
            switch (name) {
                case "equals":
                    return proxy == args[0];
                case "hashCode":
                    return System.identityHashCode(proxy);
                default:
                    return "ActivityClientHook{" + mServer + "}";
            }
        }
        if (name.equals("asBinder")) return server().asBinder();
        IBinder token = args != null && args.length > 0 && args[0] instanceof IBinder
                ? (IBinder) args[0] : null;
        Local local = sLocal;
        if (token != null && token == sLocalToken && local != null) {
            switch (name) {
                case "setRequestedOrientation":
                    local.setRequestedOrientation(token, (Integer) args[1]);
                    return null;
                case "getRequestedOrientation":
                    return local.getRequestedOrientation(token);
                case "moveActivityTaskToBack": {
                    WindowSession session = WindowSession.get();
                    return session != null && session.minimizeTask(token);
                }
                default:
                    break;
            }
        }
        if (name.equals("onBackPressed") && token != null) {
            // The server answers later (a oneway call): finishing the Activity, or moving its
            // task to the back, which stops it; the stop then minimizes its windows.
            WindowSession session = WindowSession.get();
            if (session != null) session.backPressed(token);
        }
        Object result;
        try {
            result = method.invoke(server(), args);
        } catch (InvocationTargetException e) {
            throw e.getCause();
        }
        if (name.equals("moveActivityTaskToBack") && Boolean.TRUE.equals(result)
                && token != null) {
            // The system server sent the task to the back: its windows are minimized.
            WindowSession session = WindowSession.get();
            if (session != null) session.minimizeTask(token);
        }
        return result;
    }
}
