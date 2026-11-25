/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * Windows' caption style (compat/devices/caption_win.cc:
 * Windows.Media.ClosedCaptioning.ClosedCaptionProperties) for CaptioningBridge. Changes
 * come from ClosedCaptionProperties.PropertiesChanged where Windows has it (a daemon
 * thread waits in nativeAwaitChange; Windows' thread never calls Java) and from
 * WM_SETTINGCHANGE (WindowsHost.EVENT_SETTING_CHANGED).
 */
final class WindowsCaptions implements CaptioningBridge.Backend {
    private static final int AWAIT_MS = 60_000;

    private WindowsCaptions() {
    }

    /** Null without the natives or when Windows cannot answer (no WinRT caption class). */
    static WindowsCaptions create() {
        try {
            int[] probe = new int[CaptioningSettings.VALUES];
            int hr = nativeRead(probe);
            if (hr < 0) {
                SystemServer.log("captioning: Windows caption style unavailable (hr 0x"
                        + Integer.toHexString(hr) + ")");
                return null;
            }
            return new WindowsCaptions();
        } catch (UnsatisfiedLinkError noNatives) {
            return null;
        }
    }

    @Override
    public int[] read() {
        int[] values = new int[CaptioningSettings.VALUES];
        int hr = nativeRead(values);
        if (hr < 0) {
            SystemServer.log("captioning: reading Windows caption style failed (hr 0x"
                    + Integer.toHexString(hr) + ")");
            return null;
        }
        return values;
    }

    @Override
    public Runnable watch(Runnable onChange) {
        WindowsHost.Listener settings = events -> {
            if ((events & WindowsHost.EVENT_SETTING_CHANGED) != 0) onChange.run();
        };
        WindowsHost.addListener(settings);
        int hr = nativeStartWatch();
        Thread waiter = null;
        if (hr >= 0) {
            waiter = new Thread(() -> {
                long seen = 0;
                while (true) {
                    long now = nativeAwaitChange(seen, AWAIT_MS);
                    if (now < 0) return;  // the watch stopped
                    if (now != seen) {
                        seen = now;
                        onChange.run();
                    }
                }
            }, "gesher-captions");
            waiter.setDaemon(true);
            waiter.start();
        } else {
            SystemServer.log("captioning: no PropertiesChanged on this Windows (hr 0x"
                    + Integer.toHexString(hr) + "); WM_SETTINGCHANGE only");
        }
        return () -> {
            WindowsHost.removeListener(settings);
            nativeStopWatch();
        };
    }

    /** Fills CaptioningSettings.VALUES values; returns the HRESULT. */
    private static native int nativeRead(int[] out);
    private static native int nativeStartWatch();
    private static native long nativeAwaitChange(long seen, int timeoutMs);
    private static native void nativeStopWatch();
}
