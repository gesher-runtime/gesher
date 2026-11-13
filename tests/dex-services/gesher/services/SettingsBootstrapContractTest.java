/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.app.ContentProviderHolder;
import android.app.GesherSettingsContext;
import android.app.IActivityManager;
import android.content.AttributionSource;
import android.content.Context;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import com.gesher.app.Main;
import com.gesher.server.Services;
import java.util.Collections;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;

/** Actual framework bootstrap without Binder, then Settings acquisition on a worker. */
public final class SettingsBootstrapContractTest {
    private static int checks;
    private static synchronized void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    public static void main(String[] args) throws Exception {
        if (System.getenv("GESHER_BINDER") != null) throw new AssertionError("Bootstrap fixture must have no Binder broker");
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap("settings-bootstrap", root + "/build.prop", root + "/framework/framework-res.apk");
        check(Looper.myLooper() == Looper.getMainLooper(), "Bootstrap lost the main Looper");
        check(ActivityThread.currentActivityThread() == null, "Preparation attached an application thread");
        Map<String, IBinder> services = Services.createInProcess(Collections.emptyList());
        Services.installInProcess(services);
        IActivityManager activity = IActivityManager.Stub.asInterface(services.get("activity"));
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicReference<Context> context = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                check(Looper.myLooper() == null, "Worker must not have a Looper");
                Context value = GesherSettingsContext.get();
                context.set(value);
                check(value.getMainLooper() == Looper.getMainLooper(), "Context does not retain the main Looper");
                check(value.getContentResolver() != null, "Context has no ContentResolver");
                check("android".equals(value.getPackageName()), "Context lost the system package identity");
                ContentProviderHolder holder = activity.getContentProviderExternal("settings", 0, null, "bootstrap-contract");
                check(holder != null && holder.provider != null, "Settings provider transport is unavailable");
                AttributionSource caller = new AttributionSource.Builder(Process.myUid()).setPackageName("android").build();
                check("1.0".equals(holder.provider.call(caller, "settings", "GET_system", "font_scale", null)
                        .getString("value")), "Settings transport cannot read its ordinary row");
            } catch (Throwable error) { failure.set(error); }
        }, "settings-bootstrap-worker");
        worker.setDaemon(true);
        worker.start();
        worker.join(15000);
        check(!worker.isAlive(), "Settings context waited for the main Looper");
        if (failure.get() != null) throw new AssertionError("Settings worker failed", failure.get());
        check(context.get() == GesherSettingsContext.get(), "Provider context is not a singleton");
        check(ActivityThread.currentActivityThread() == null, "Provider context attached an application thread");
        System.out.println("SettingsBootstrapContractTest all passed: " + checks + " checks; bootstrap without Binder, worker transport");
    }
}
