/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app;

import android.content.Context;
import android.content.pm.IPackageManager;
import android.content.pm.SystemFeaturesCache;
import android.os.Binder;
import android.os.Looper;
import android.os.RemoteException;

/** Framework context for built-in providers' normal Binder transports. */
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
                    // Built-in providers can need framework managers before any
                    // application is bound. SystemServiceRegistry consults this
                    // cache during manager construction; obtain the same actual
                    // package-manager features that bindApplication supplies.
                    // An already bound application's cache keeps its identity.
                    if (!SystemFeaturesCache.hasInstance()) {
                        IPackageManager packages = ActivityThread.getPackageManager();
                        if (packages == null) {
                            throw new IllegalStateException("Package manager is not available");
                        }
                        SystemFeaturesCache.setInstance(new SystemFeaturesCache(
                                packages.getSystemAvailableFeatures().getList()));
                    }
                    sContext = ContextImpl.createSystemContext(sMainThread);
                } catch (RemoteException failure) {
                    throw failure.rethrowFromSystemServer();
                } finally {
                    Binder.restoreCallingIdentity(identity);
                }
            }
            return sContext;
        }
    }
}
