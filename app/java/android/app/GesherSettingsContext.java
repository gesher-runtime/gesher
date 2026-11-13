/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app;

import android.content.Context;
import android.os.Looper;

/** Framework context for the built-in settings provider's normal Binder transport. */
public final class GesherSettingsContext {
    private static volatile Context sContext;
    private GesherSettingsContext() { }

    /** Main.bootstrap calls this after installing the main Looper and framework resources. */
    public static synchronized void prepare() {
        if (sContext != null) return;
        if (Looper.myLooper() != Looper.getMainLooper() || Looper.myLooper() == null) {
            throw new IllegalStateException("Prepare the settings context on the main Looper");
        }
        // Supply ContentResolver's application-thread identity without attaching or
        // replacing an application's ActivityThread.
        sContext = ContextImpl.createSystemContext(new ActivityThread());
    }

    public static Context get() {
        Context context = sContext;
        if (context == null) throw new IllegalStateException("Settings context was not prepared");
        return context;
    }
}
