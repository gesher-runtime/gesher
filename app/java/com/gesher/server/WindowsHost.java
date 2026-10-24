/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * What the device-state services ask of Windows (devsvc track, docs/services.md "Device
 * services", ADR 0038): the clipboard, a timer for alarms that follows the wall clock and
 * Windows' sleep, power status and requests, the network list manager, the time zone,
 * toasts, and the events of a hidden window (clipboard changes, clock and time zone
 * changes, sleep and resume, power status, toast clicks), which a Java thread hands to
 * {@link Listener}s. The natives are {@code compat/devsvc} (libandroid_runtime's builtin
 * library); without them ({@link #available} false: a runtime built before them) the
 * services fall back to keeping their state in the system server only.
 */
public final class WindowsHost {
    private WindowsHost() {
    }

    public static final int EVENT_CLIPBOARD = 1;
    public static final int EVENT_TIME_CHANGED = 1 << 1;
    public static final int EVENT_RESUMED = 1 << 2;
    public static final int EVENT_POWER_STATUS = 1 << 3;
    public static final int EVENT_SUSPENDING = 1 << 4;
    public static final int EVENT_SETTING_CHANGED = 1 << 5;
    public static final int EVENT_TOAST_ACTIVATED = 1 << 6;

    /** AlarmWait's answers. */
    static final int ALARM_TIME = 0;
    static final int ALARM_KICKED = 1;
    static final int ALARM_TIMEOUT = 2;

    /** Gets the events of the hidden window, on the "windows-events" thread. */
    public interface Listener {
        void onWindowsEvents(int events);
    }

    private static final List<Listener> sListeners = new CopyOnWriteArrayList<>();
    private static final Object sFallbackAlarm = new Object();
    private static volatile Boolean sAvailable;
    private static boolean sStarted;  // guarded by WindowsHost.class
    private static boolean sKicked;   // guarded by sFallbackAlarm

    /** Whether the natives are there (the runtime has compat/devsvc). */
    public static boolean available() {
        Boolean a = sAvailable;
        if (a == null) {
            try {
                nativeClipboardSequence();
                a = true;
            } catch (UnsatisfiedLinkError e) {
                a = false;
                SystemServer.log("devsvc: no Windows natives in this runtime (" + e.getMessage()
                        + "): device services keep their state in the system server only");
            }
            sAvailable = a;
        }
        return a;
    }

    /** Adds a listener, starting the event window and its thread on the first. */
    public static void addListener(Listener listener) {
        sListeners.add(listener);
        if (!available()) return;
        synchronized (WindowsHost.class) {
            if (sStarted) return;
            sStarted = true;
        }
        long window = nativeStartEvents();
        // The handle is in the log for the tests: they send the window WM_TIMECHANGE by
        // handle, since FindWindow from another window station or desktop (CI) misses it.
        SystemServer.log("devsvc: Windows events " + (window != 0
                ? "from a hidden window 0x" + Long.toHexString(window)
                : "not available (no window)"));
        Thread t = new Thread(() -> {
            while (true) {
                int events = nativeWaitEvents(60_000);
                if (events == 0) continue;
                for (Listener l : sListeners) {
                    try {
                        l.onWindowsEvents(events);
                    } catch (Throwable e) {
                        SystemServer.log("devsvc: a Windows event listener failed: " + e);
                    }
                }
            }
        }, "windows-events");
        t.setDaemon(true);
        t.start();
    }

    /**
     * Whether this process is a system server (SystemServer sets GESHER_BINDER_UID to the
     * system uid before it creates the services), not an app's own (--single-process).
     */
    static boolean inSystemServer() {
        return String.valueOf(android.os.Process.SYSTEM_UID).equals(
                System.getenv("GESHER_BINDER_UID"));
    }

    /** The oldest toast click not yet taken: app id, tag, group, arguments; or null. */
    public static String[] nextActivation() {
        return available() ? nativeNextActivation() : null;
    }

    // --- clipboard ----------------------------------------------------------------------

    public static String clipboardText() {
        return available() ? nativeClipboardText() : null;
    }

    public static String clipboardHtml() {
        return available() ? nativeClipboardHtml() : null;
    }

    public static boolean setClipboard(String text, String html) {
        return available() && nativeSetClipboard(text, html);
    }

    public static int clipboardSequence() {
        return available() ? nativeClipboardSequence() : 0;
    }

    // --- alarms -------------------------------------------------------------------------

    /**
     * Waits until the wall clock reaches {@code utcMillis} (a Windows waitable timer: it
     * follows clock changes and fires on resume when the time passed during sleep; with
     * {@code wakeup}, it may wake Windows), {@link #alarmKick}, or {@code timeoutMs}.
     */
    static int alarmWait(long utcMillis, boolean wakeup, int timeoutMs) {
        if (available()) return nativeAlarmWait(utcMillis, wakeup, timeoutMs);
        long wait = Math.max(0, Math.min(timeoutMs, utcMillis - System.currentTimeMillis()));
        synchronized (sFallbackAlarm) {
            if (!sKicked && wait > 0) {
                try {
                    sFallbackAlarm.wait(wait);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
            boolean kicked = sKicked;
            sKicked = false;
            if (kicked) return ALARM_KICKED;
        }
        return System.currentTimeMillis() >= utcMillis ? ALARM_TIME : ALARM_TIMEOUT;
    }

    static void alarmKick() {
        if (available()) {
            nativeAlarmKick();
            return;
        }
        synchronized (sFallbackAlarm) {
            sKicked = true;
            sFallbackAlarm.notifyAll();
        }
    }

    // --- power ----------------------------------------------------------------------------

    /**
     * GetSystemPowerStatus: {AC line (0 off, 1 on, 255 unknown), battery flag (8 charging,
     * 128 no battery, 255 unknown), percent (255 unknown), 1 if battery saver is on}; null
     * if unknown.
     */
    public static int[] powerStatus() {
        return available() ? nativePowerStatus() : null;
    }

    /** A power request of the process (powercfg /requests shows {@code reason}). */
    public static boolean keepAwake(String reason, boolean system, boolean display) {
        return available() && nativeKeepAwake(reason, system, display);
    }

    /** Milliseconds since the user's last input; 0 if unknown. */
    public static long idleMillis() {
        return available() ? nativeIdleMillis() : 0;
    }

    /** Bits: 1 connected, 2 internet, 4 metered; -1 if unknown. */
    public static int networkState() {
        return available() ? nativeNetworkState() : -1;
    }

    /** The Windows time zone's key name, or null. */
    public static String timeZoneKeyName() {
        return available() ? nativeTimeZoneKeyName() : null;
    }

    // --- toasts ---------------------------------------------------------------------------

    /** Shows a toast; returns its HRESULT (0: shown). */
    public static int showToast(String appId, String tag, String group, String xml,
            boolean suppressPopup, boolean high) {
        return available() ? nativeShowToast(appId, tag, group, xml, suppressPopup, high)
                : 0x80004001;  // E_NOTIMPL
    }

    public static int removeToast(String appId, String tag, String group) {
        return available() ? nativeRemoveToast(appId, tag, group) : 0x80004001;
    }

    public static int clearToasts(String appId) {
        return available() ? nativeClearToasts(appId) : 0x80004001;
    }

    /** The AppUserModelID's name and icon for its toasts (HKCU\Software\Classes\AppUserModelId). */
    public static int registerAppId(String appId, String displayName, String iconPath) {
        return available() ? nativeRegisterAppId(appId, displayName, iconPath) : 0x80004001;
    }

    public static int unregisterAppId(String appId) {
        return available() ? nativeUnregisterAppId(appId) : 0x80004001;
    }

    /** A drive-less path as Windows names it (with the current drive). */
    public static String fullPath(String path) {
        return available() ? nativeFullPath(path) : path;
    }

    private static native long nativeStartEvents();
    private static native int nativeWaitEvents(int timeoutMs);
    private static native String[] nativeNextActivation();
    private static native String nativeClipboardText();
    private static native String nativeClipboardHtml();
    private static native boolean nativeSetClipboard(String text, String html);
    private static native int nativeClipboardSequence();
    private static native int nativeAlarmWait(long utcMillis, boolean wakeup, int timeoutMs);
    private static native void nativeAlarmKick();
    private static native int[] nativePowerStatus();
    private static native boolean nativeKeepAwake(String reason, boolean system,
            boolean display);
    private static native long nativeIdleMillis();
    private static native int nativeNetworkState();
    private static native String nativeTimeZoneKeyName();
    private static native int nativeShowToast(String appId, String tag, String group,
            String xml, boolean suppressPopup, boolean high);
    private static native int nativeRemoveToast(String appId, String tag, String group);
    private static native int nativeClearToasts(String appId);
    private static native int nativeRegisterAppId(String appId, String displayName,
            String iconPath);
    private static native int nativeUnregisterAppId(String appId);
    private static native String nativeFullPath(String path);
}
