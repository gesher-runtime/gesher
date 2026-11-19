/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.window;

import android.os.IBinder;
import java.util.IdentityHashMap;

/** Explicit host registration boundary; no native HWND or UI is created. */
public final class WindowSession {
    private static final WindowSession SESSION = new WindowSession();
    private final IdentityHashMap<IBinder, Long> windows = new IdentityHashMap<>();
    public static WindowSession get() { return SESSION; }
    public synchronized void bind(IBinder token, long hwnd) { windows.put(token, hwnd); }
    public synchronized void remove(IBinder token) { windows.remove(token); }
    public synchronized long hwndOf(IBinder token) { return windows.getOrDefault(token, 0L); }
}
