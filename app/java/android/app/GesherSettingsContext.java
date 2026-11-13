/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app;

import android.content.Context;
import android.os.Binder;
import android.os.Looper;

/** Framework context for the built-in settings provider's normal Binder transport. */
public final class GesherSettingsContext {
    private static ActivityThread sMainThread;
    private static volatile Context sContext;
    private GesherSettingsContext() { }

    /** Main.bootstrap calls this after installing the main Looper and framework resources. */
    public static synchronized void prepare() {
        if (sMainThread != null) return;
        if (Looper.myLooper() != Looper.getMainLooper() || Looper.myLooper() == null) {
            throw new IllegalStateException("Prepare the settings context on the main Looper");
        }
        // Capture the main-Looper identity without attaching/replacing an application's
        // ActivityThread. ContextImpl construction queries the display service, which
        // does not exist yet in launcher/bootstrap-only or in-process startup paths.
        sMainThread = new ActivityThread();
    }

    public static Context get() {
        Context context = sContext;
        if (context != null) return context;
        synchronized (GesherSettingsContext.class) {
            if (sContext == null) {
                if (sMainThread == null) throw new IllegalStateException("Settings context was not prepared");
                // The provider is acquired after services are available, often on a
                // Binder worker without a Looper. Use the captured main-thread identity
                // synchronously: system_server's main thread serves Binder, not a Looper.
                long identity = Binder.clearCallingIdentity();
                try {
                    sContext = ContextImpl.createSystemContext(sMainThread);
                } finally {
                    Binder.restoreCallingIdentity(identity);
                }
            }
            return sContext;
        }
    }
}
