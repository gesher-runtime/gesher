/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.usage.NetworkStats;
import android.app.usage.NetworkStatsManager;
import android.content.Context;
import android.os.RemoteException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.List;

/** Real Context wrapper lookup and the documented unavailable-backend failure channel. */
public final class NetworkStatsUnavailableTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
    }
    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("network-stats-test", root + "/build.prop", root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.create(List.of()));
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        Method create = Class.forName("android.app.ContextImpl").getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        Context context = (Context) create.invoke(null, thread);
        Object named = context.getSystemService(Context.NETWORK_STATS_SERVICE);
        require(named instanceof NetworkStatsManager, "Named accounting lookup lacks its compatibility wrapper");
        NetworkStatsManager manager = context.getSystemService(NetworkStatsManager.class);
        require(manager == named, "Typed and named accounting wrappers differ");
        require(NetworkStatsManager.class.getClassLoader() == Object.class.getClassLoader(),
                "Accounting wrapper is not the production boot class");
        boolean failed = false;
        try { manager.querySummary(1, null, 0, 1); }
        catch (RemoteException expected) { failed = true; }
        require(failed, "Summary query returned invented measurements or an incompatible error");
        failed = false;
        try { manager.querySummaryForDevice(0, "synthetic-subscriber", 0, 1); }
        catch (RemoteException expected) { failed = true; }
        require(failed, "Device summary returned invented measurements or an incompatible error");
        NetworkStats.Bucket empty = new NetworkStats.Bucket();
        require(empty.getUid() == -1 && empty.getRxBytes() == 0 && empty.getTxBytes() == 0,
                "Empty output container has populated measurements");
        require(NetworkStats.class.getConstructors().length == 0, "Unavailable query result is publicly constructible");
        System.out.println("NetworkStatsUnavailableTest all passed: " + checks + " synthetic checks");
    }
}
