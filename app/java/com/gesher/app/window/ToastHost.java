/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.app.ActivityThread;
import android.app.INotificationManager;
import android.app.ITransientNotification;
import android.app.ITransientNotificationCallback;
import android.content.Context;
import android.graphics.PixelFormat;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;
import android.widget.ToastPresenter;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.Iterator;

/**
 * Toasts in an app process with windows (docs/graphics.md, "Dialogs, menus and other
 * popups"; ADR 0040). On a device {@code Toast.show()} asks NotificationManagerService,
 * which queues the toasts of every app, one at a time: a text toast is drawn by SystemUI
 * (ToastUI and its ToastPresenter), a custom one by the app ({@code TN.show}). Gesher has no
 * SystemUI and draws each process's windows in that process (ADR 0029), so the process's
 * {@code Toast} talks to this host instead (its {@code sService}, wrapping the system's
 * {@code notification} service for everything else): it queues the toasts, shows a text
 * toast as SystemUI does (ToastPresenter's text view in a {@code TYPE_TOAST} window: a
 * popup of the app's active window, at the bottom, not focusable, not touchable) or has a
 * custom toast's TN show itself, and hides each after its duration (NotificationManager
 * Service's 2 s and 3.5 s).
 */
final class ToastHost implements InvocationHandler {
    private static final String TAG = "gesher-app";
    /** NotificationManagerService.SHORT_DELAY and LONG_DELAY. */
    private static final int SHORT_DELAY = 2000;
    private static final int LONG_DELAY = 3500;

    private static boolean sInstalled;

    private final INotificationManager mSystem;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final ArrayDeque<Record> mQueue = new ArrayDeque<>();
    private Record mCurrent;

    /** A toast: its text, or its TN. */
    private static final class Record {
        String pkg;
        IBinder token;
        CharSequence text;
        ITransientNotification tn;
        ITransientNotificationCallback callback;
        int duration;
        View view;
        IBinder windowToken;

        String describe() {
            return text != null ? "\"" + text + "\"" : "(custom view)";
        }
    }

    private ToastHost(INotificationManager system) {
        mSystem = system;
    }

    /** Makes this process's toasts go through a ToastHost. */
    static synchronized void install() {
        if (sInstalled) return;
        try {
            INotificationManager system = INotificationManager.Stub.asInterface(
                    ServiceManager.getService(Context.NOTIFICATION_SERVICE));
            Object proxy = Proxy.newProxyInstance(INotificationManager.class.getClassLoader(),
                    new Class<?>[] {INotificationManager.class}, new ToastHost(system));
            Field f = Toast.class.getDeclaredField("sService");
            f.setAccessible(true);
            f.set(null, proxy);
            sInstalled = true;
        } catch (ReflectiveOperationException | RuntimeException e) {
            log("toasts: not hosted (" + e + ")");
        }
    }

    private static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        switch (method.getName()) {
            case "enqueueTextToast": {
                // (pkg, token, text, duration, isUiContext, displayId, callback)
                Record r = new Record();
                r.pkg = (String) args[0];
                r.token = (IBinder) args[1];
                r.text = (CharSequence) args[2];
                r.duration = (Integer) args[3];
                r.callback = (ITransientNotificationCallback) args[args.length - 1];
                enqueue(r);
                return true;
            }
            case "enqueueToast": {
                // (pkg, token, callback, duration, isUiContext, displayId)
                Record r = new Record();
                r.pkg = (String) args[0];
                r.token = (IBinder) args[1];
                r.tn = (ITransientNotification) args[2];
                r.duration = (Integer) args[3];
                enqueue(r);
                return true;
            }
            case "cancelToast":
                cancel((IBinder) args[1]);
                return null;
            case "finishToken":
                return null;  // the toast's window token is the host's
            case "asBinder":
                return mSystem != null ? mSystem.asBinder() : new Binder();
            case "hashCode":
                return System.identityHashCode(proxy);
            case "equals":
                return proxy == args[0];
            case "toString":
                return "ToastHost(" + mSystem + ")";
            default:
                break;
        }
        if (mSystem == null) return defaultValue(method.getReturnType());
        try {
            return method.invoke(mSystem, args);
        } catch (InvocationTargetException e) {
            throw e.getCause();
        }
    }

    private static Object defaultValue(Class<?> type) {
        if (type == boolean.class) return false;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0f;
        if (type == double.class) return 0d;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == char.class) return (char) 0;
        return null;
    }

    private void enqueue(Record r) {
        mHandler.post(() -> {
            mQueue.add(r);
            showNext();
        });
    }

    private void cancel(IBinder token) {
        mHandler.post(() -> {
            for (Iterator<Record> it = mQueue.iterator(); it.hasNext(); ) {
                if (it.next().token == token) it.remove();
            }
            if (mCurrent != null && mCurrent.token == token) hideCurrent(mCurrent);
        });
    }

    /** Shows the next toast, if none is shown (the main thread). */
    private void showNext() {
        if (mCurrent != null) return;
        Record r = mQueue.poll();
        if (r == null) return;
        mCurrent = r;
        r.windowToken = new Binder();
        int ms = r.duration == Toast.LENGTH_LONG ? LONG_DELAY : SHORT_DELAY;
        try {
            if (r.tn != null) {
                r.tn.show(r.windowToken);
            } else {
                showText(r);
            }
            log("toast " + r.describe() + " shown for " + ms + " ms");
        } catch (RuntimeException | RemoteException e) {
            log("toast " + r.describe() + " not shown: " + e);
            mCurrent = null;
            mHandler.post(this::showNext);
            return;
        }
        mHandler.postDelayed(() -> hideCurrent(r), ms);
    }

    /** A text toast as SystemUI's ToastUI shows it (ToastPresenter's view and parameters). */
    private void showText(Record r) throws RemoteException {
        Context context = ActivityThread.currentApplication();
        if (context == null) throw new IllegalStateException("no application");
        View view = ToastPresenter.getTextToastView(context, r.text);
        WindowManager.LayoutParams p = new WindowManager.LayoutParams();
        p.width = WindowManager.LayoutParams.WRAP_CONTENT;
        p.height = WindowManager.LayoutParams.WRAP_CONTENT;
        p.format = PixelFormat.TRANSLUCENT;
        p.type = WindowManager.LayoutParams.TYPE_TOAST;
        p.setFitInsetsIgnoringVisibility(true);
        p.setTitle("Toast");
        p.flags = WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        android.content.res.Resources res = context.getResources();
        p.gravity = res.getInteger(com.android.internal.R.integer.config_toastDefaultGravity);
        p.y = res.getDimensionPixelSize(com.android.internal.R.dimen.toast_y_offset);
        p.packageName = context.getPackageName();
        p.token = r.windowToken;
        context.getSystemService(WindowManager.class).addView(view, p);
        r.view = view;
        if (r.callback != null) r.callback.onToastShown();
    }

    private void hideCurrent(Record r) {
        if (mCurrent != r) return;
        try {
            if (r.tn != null) {
                r.tn.hide();
            } else if (r.view != null) {
                Context context = ActivityThread.currentApplication();
                WindowManager wm = context != null
                        ? context.getSystemService(WindowManager.class) : null;
                if (wm != null && r.view.getParent() != null) wm.removeViewImmediate(r.view);
                if (r.callback != null) r.callback.onToastHidden();
            }
            log("toast " + r.describe() + " hidden");
        } catch (RuntimeException | RemoteException e) {
            log("toast " + r.describe() + ": hiding: " + e);
        }
        mCurrent = null;
        showNext();
    }
}
