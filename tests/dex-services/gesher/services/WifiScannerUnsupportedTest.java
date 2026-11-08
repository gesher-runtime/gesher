/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.content.pm.SystemFeaturesCache;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiScanner;
import android.os.Build;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;

/** No-scanner capability checks through the actual boot classes and ContextImpl. */
public final class WifiScannerUnsupportedTest {
    private static int checks;
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    // The public bytecode predicate in original microG252432034's
    // WifiScannerSource.Companion.isSupported, with its private failed flag
    // supplied as a scalar. No Google APK code or service is executed here.
    private static boolean supported(Context context, boolean failed) {
        if (Build.VERSION.SDK_INT < 26 || failed) return false;
        Object service = context.getSystemService("wifiscanner");
        WifiScanner scanner = service instanceof WifiScanner ? (WifiScanner) service : null;
        return scanner != null && context.checkSelfPermission("android.permission.LOCATION_HARDWARE")
                == PackageManager.PERMISSION_GRANTED;
    }

    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("scanner-test", root + "/build.prop", root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.create(List.of()));
        Field features = Class.forName("com.gesher.server.PackageManagerService").getDeclaredField("FEATURES");
        features.setAccessible(true);
        SystemFeaturesCache.setInstance(new SystemFeaturesCache(
                (android.util.ArrayMap<String, FeatureInfo>) features.get(null)));
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        Method create = Class.forName("android.app.ContextImpl").getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        Context context = (Context) create.invoke(null, thread);

        // This exact instanceof-null path threw NoClassDefFoundError on A487.
        check(!supported(context, false), "Absent scanner reported supported");
        check(!supported(context, true), "Previously failed scanner reported supported");
        check(context.getSystemService("wifiscanner") == null, "Named lookup fabricated a hardware scanner");
        check(context.getSystemService(WifiScanner.class) == null, "Typed lookup fabricated a hardware scanner");
        check(!context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI), "Scanner type declared Wi-Fi hardware");
        check(WifiScanner.class.getClassLoader() == Object.class.getClassLoader(), "Scanner type is not the production boot class");
        WifiManager wifi = context.getSystemService(WifiManager.class);
        check(wifi != null && !wifi.isWifiEnabled(), "Legacy fallback wrapper is missing or enabled");
        check(!wifi.startScan() && wifi.getScanResults().isEmpty(), "Unavailable fallback scan reported success or data");
        System.out.println("WifiScannerUnsupportedTest all passed: " + checks);
    }
}
