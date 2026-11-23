/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ActivityManager;
import android.app.usage.ExternalStorageStats;
import android.app.usage.IStorageStatsManager;
import android.app.usage.StorageStats;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.function.Predicate;

/**
 * Android Settings screens (SettingsIntents) through the activity starter, with no window
 * opened: an app's "App info" reaches the Control Center on that app (File Manager+'s
 * app list showed "Application not found" for every app), device screens reach a fixed
 * Windows Settings page, anything else stays unresolved. Also the service answers that
 * Default left wrong: component enabled states, content-provider queries, storage stats,
 * memory info, the SIM state and the profile type.
 */
public final class SettingsIntentsTest {
    private static final String PACKAGE = "com.gesher.components";
    private static int checks;

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private interface Call { void run() throws Exception; }

    private static void throwsType(Class<? extends Throwable> type, Call call, String message)
            throws Exception {
        try {
            call.run();
        } catch (Throwable t) {
            Throwable cause = t instanceof java.lang.reflect.InvocationTargetException
                    ? t.getCause() : t;
            if (type.isInstance(cause)) {
                checks++;
                return;
            }
            throw new AssertionError(message + ": " + cause, cause);
        }
        throw new AssertionError(message + ": nothing thrown");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the components.apk fixture");
        System.loadLibrary("android_runtime");
        // This VM owns the package-cache invalidation (component states invalidate it),
        // as SystemServer does.
        com.android.internal.os.ApplicationSharedMemory.setInstance(
                com.android.internal.os.ApplicationSharedMemory.create());
        // The package parser's base AssetManager, as SystemServer sets it up.
        android.content.res.AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File data = Files.createTempDirectory("gesher-settings-intents-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        File apkFile = new File(data, "base.apk");
        Files.copy(new File(args[0]).toPath(), apkFile.toPath(),
                StandardCopyOption.REPLACE_EXISTING);
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Object apk = apkClass.getMethod("parse", String.class).invoke(null, apkFile.getPath());
        require(PACKAGE.equals(apkClass.getField("packageName").get(apk)), "wrong fixture");

        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Object pm = pmClass.getConstructor(Collection.class)
                .newInstance(Collections.singletonList(apk));

        settingsIntents(pm, pmClass);
        componentStates((IPackageManager) pm);
        contentProviders((IPackageManager) pm);
        storageStats(pm, pmClass, data);
        standInDefaults();
        System.out.println("SettingsIntentsTest all passed: " + checks + " checks");
    }

    private static void settingsIntents(Object pm, Class<?> pmClass) throws Exception {
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Constructor<?> constructor = atmClass.getDeclaredConstructor(pmClass);
        constructor.setAccessible(true);
        Object atm = constructor.newInstance(pm);
        Method start = null;
        for (Method method : atmClass.getDeclaredMethods()) {
            if (method.getName().equals("start") && method.getParameterCount() == 8) start = method;
        }
        require(start != null, "activity starter missing");
        start.setAccessible(true);

        Field page = atmClass.getDeclaredField("sOpenSettingsPage");
        page.setAccessible(true);
        Field notifications = atmClass.getDeclaredField("sOpenNotificationSettings");
        notifications.setAccessible(true);
        Class<?> centerClass = Class.forName("com.gesher.server.ControlCenter");
        Field center = centerClass.getDeclaredField("sOpen");
        center.setAccessible(true);
        Object previousPage = page.get(null);
        Object previousNotifications = notifications.get(null);
        Object previousCenter = center.get(null);
        List<String> pages = new ArrayList<>();
        List<String> centers = new ArrayList<>();
        try {
            page.set(null, (Predicate<String>) uri -> pages.add(uri));
            notifications.set(null, (Predicate<String>) uri -> pages.add("notifications:" + uri));
            center.set(null, (Predicate<String>) pkg -> centers.add(String.valueOf(pkg)));

            // File Manager+'s app list: App info of an installed app.
            Intent details = new Intent("android.settings.APPLICATION_DETAILS_SETTINGS",
                    Uri.fromParts("package", PACKAGE, null));
            require(started(start, atm, details), "App info of an installed app was not handled");
            require(centers.equals(Collections.singletonList(PACKAGE)),
                    "Control Center was not opened on the app: " + centers);
            // As Settings resolves it: addressed to com.android.settings.
            require(started(start, atm, new Intent(details).setPackage("com.android.settings")),
                    "App info addressed to com.android.settings was not handled");
            // A per-app screen that names the app in the APP_PACKAGE extra.
            require(started(start, atm, new Intent("android.settings.APP_LOCALE_SETTINGS")
                    .putExtra("android.provider.extra.APP_PACKAGE", PACKAGE)),
                    "APP_PACKAGE extra was not read");
            // Android 11+'s all-files access for an app: its details too.
            require(started(start, atm, new Intent(
                    "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION",
                    Uri.parse("package:" + PACKAGE))), "all-files access screen not handled");
            require(centers.size() == 4 && centers.stream().allMatch(PACKAGE::equals),
                    "per-app screens did not name the app: " + centers);

            // A list screen without data: the Control Center itself.
            require(started(start, atm, new Intent(
                    "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION")),
                    "all-files access list not handled");
            require(started(start, atm, new Intent("android.settings.SYNC_SETTINGS")),
                    "sync settings (GmsCore) not handled");
            require(centers.size() == 6 && "null".equals(centers.get(4))
                    && "null".equals(centers.get(5)), "list screens named a package: " + centers);

            // Device screens: fixed Windows Settings pages, never the intent's data.
            require(started(start, atm, new Intent("android.settings.WIFI_SETTINGS")
                    .setData(Uri.parse("ms-settings:evil"))), "Wi-Fi settings not handled");
            require(started(start, atm, new Intent("android.settings.LOCATION_SOURCE_SETTINGS")),
                    "location settings not handled");
            require(started(start, atm, new Intent("android.settings.BLUETOOTH_SETTINGS")),
                    "Bluetooth settings not handled");
            require(pages.equals(Arrays.asList("ms-settings:network-wifi",
                    "ms-settings:privacy-location", "ms-settings:bluetooth")),
                    "unexpected Windows pages: " + pages);
            // The notification screens keep their own handler.
            require(started(start, atm, new Intent("android.settings.APP_NOTIFICATION_SETTINGS")),
                    "notification settings no longer handled");
            require(pages.size() == 4 && pages.get(3).equals("notifications:ms-settings:notifications"),
                    "notification settings took another path: " + pages);

            int before = centers.size() + pages.size();
            Intent[] unresolved = {
                    // An app that is not installed: Settings' own "not found".
                    new Intent("android.settings.APPLICATION_DETAILS_SETTINGS",
                            Uri.fromParts("package", "com.example.missing", null)),
                    // Not a package name: never reaches the Control Center's request file.
                    new Intent("android.settings.APPLICATION_DETAILS_SETTINGS",
                            Uri.parse("package:../../evil")),
                    new Intent("android.settings.APPLICATION_DETAILS_SETTINGS",
                            Uri.parse("package:com.gesher.components\nother.app")),
                    // App info without an app.
                    new Intent("android.settings.APPLICATION_DETAILS_SETTINGS"),
                    // Addressed elsewhere: another package, an explicit component.
                    new Intent(details).setPackage("other.application"),
                    new Intent(details).setComponent(new ComponentName("com.android.settings",
                            "com.android.settings.applications.InstalledAppDetails")),
                    // Screens Gesher has no counterpart for.
                    new Intent("android.settings.NFC_SETTINGS"),
                    new Intent("android.settings.NOT_A_SETTINGS_SCREEN"),
            };
            for (Intent intent : unresolved) {
                Object status = start.invoke(atm, null, intent, null, "gesher.services", null,
                        null, -1, null);
                require(Integer.valueOf(ActivityManager.START_INTENT_NOT_RESOLVED).equals(status),
                        "intent should stay unresolved: " + intent + " -> " + status);
            }
            require(centers.size() + pages.size() == before,
                    "an unresolved intent opened something: " + centers + " " + pages);

            // A failed launch is reported, not swallowed.
            center.set(null, (Predicate<String>) pkg -> false);
            page.set(null, (Predicate<String>) uri -> false);
            Object failed = start.invoke(atm, null, details, null, "gesher.services", null,
                    null, -1, null);
            require(Integer.valueOf(ActivityManager.START_INTENT_NOT_RESOLVED).equals(failed),
                    "failed Control Center launch reported success");
            failed = start.invoke(atm, null, new Intent("android.settings.WIFI_SETTINGS"), null,
                    "gesher.services", null, null, -1, null);
            require(Integer.valueOf(ActivityManager.START_INTENT_NOT_RESOLVED).equals(failed),
                    "failed Windows Settings launch reported success");
        } finally {
            page.set(null, previousPage);
            notifications.set(null, previousNotifications);
            center.set(null, previousCenter);
        }

        // The request file: one line, replaced whole.
        Method write = centerClass.getDeclaredMethod("writeRequest", File.class, String.class);
        write.setAccessible(true);
        File dir = Files.createTempDirectory("gesher-control-center-").toFile();
        File request = new File(new File(dir, "system"), "control-panel-request");
        write.invoke(null, request, PACKAGE);
        write.invoke(null, request, "org.example.app");
        require(new String(Files.readAllBytes(request.toPath()), "UTF-8").equals("org.example.app\n"),
                "request file was not replaced whole");
        write.invoke(null, request, null);
        require(new String(Files.readAllBytes(request.toPath()), "UTF-8").equals("\n"),
                "main-view request was not empty");
        System.out.println("SettingsIntentsTest: Settings screens reach the Control Center or Windows Settings");
    }

    private static boolean started(Method start, Object atm, Intent intent) throws Exception {
        Object result = start.invoke(atm, null, intent, null, "gesher.services", null, null, -1,
                null);
        return Integer.valueOf(ActivityManager.START_SUCCESS).equals(result);
    }

    private static List<String> names(List<ResolveInfo> infos) {
        List<String> names = new ArrayList<>();
        for (ResolveInfo ri : infos) {
            names.add(ri.activityInfo != null ? ri.activityInfo.name
                    : ri.serviceInfo != null ? ri.serviceInfo.name : ri.providerInfo.name);
        }
        return names;
    }

    @SuppressWarnings("unchecked")
    private static void componentStates(IPackageManager pm) throws Exception {
        ComponentName main = new ComponentName(PACKAGE, PACKAGE + ".MainActivity");
        ComponentName receiver = new ComponentName(PACKAGE, PACKAGE + ".ManifestReceiver");
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setPackage(PACKAGE);
        require(pm.getComponentEnabledSetting(main, 0)
                == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, "fresh state is not DEFAULT");
        require(names(pm.queryIntentActivities(launcher, null, 0, 0).getList())
                .contains(main.getClassName()), "launcher activity missing before disabling");

        // A launcher icon hidden at run time (an app hiding itself after setup).
        pm.setComponentEnabledSetting(main, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP, 0, PACKAGE);
        require(pm.getComponentEnabledSetting(main, 0)
                == PackageManager.COMPONENT_ENABLED_STATE_DISABLED, "disabled state not kept");
        require(!names(pm.queryIntentActivities(launcher, null, 0, 0).getList())
                .contains(main.getClassName()), "disabled activity still resolves");
        require(names(pm.queryIntentActivities(launcher, null,
                PackageManager.MATCH_DISABLED_COMPONENTS, 0).getList())
                .contains(main.getClassName()), "MATCH_DISABLED_COMPONENTS lost the activity");
        // Explicit lookups (startActivity by component, the launcher) do not find it either.
        require(pm.getActivityInfo(main, 0, 0) == null, "disabled activity still found by name");
        require(pm.getActivityInfo(main, PackageManager.MATCH_DISABLED_COMPONENTS, 0) != null,
                "MATCH_DISABLED_COMPONENTS lost the activity by name");
        require(!pm.getActivityInfo(main, PackageManager.MATCH_DISABLED_COMPONENTS, 0).enabled,
                "disabled activity metadata still says enabled");
        require(!names(pm.queryIntentActivityOptions(null, null, null, launcher, null, 0, 0)
                .getList()).contains(main.getClassName()), "activity options still finds disabled activity");
        require(names(pm.queryIntentActivityOptions(null, null, null, launcher, null,
                PackageManager.MATCH_DISABLED_COMPONENTS, 0).getList()).contains(main.getClassName()),
                "activity options lost MATCH_DISABLED_COMPONENTS");
        // DISABLED_USER is an application state: ignored for a component, as on Android.
        pm.setComponentEnabledSetting(receiver,
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER, 0, 0, PACKAGE);
        require(pm.getComponentEnabledSetting(receiver, 0)
                == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, "DISABLED_USER was applied");
        require(pm.getReceiverInfo(receiver, 0, 0) != null, "ignored state hid the receiver");

        // The batch form checks every entry first: one bad entry changes nothing.
        throwsType(IllegalArgumentException.class, () -> pm.setComponentEnabledSettings(
                Arrays.asList(new PackageManager.ComponentEnabledSetting(receiver,
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED, 0),
                        new PackageManager.ComponentEnabledSetting(
                                new ComponentName(PACKAGE, "NoSuchClass"),
                                PackageManager.COMPONENT_ENABLED_STATE_DISABLED, 0)), 0, PACKAGE),
                "batch with an unknown component was accepted");
        require(pm.getComponentEnabledSetting(receiver, 0)
                == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, "failed batch changed a state");

        pm.setComponentEnabledSetting(main, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, 0, 0,
                PACKAGE);
        require(pm.getComponentEnabledSetting(main, 0)
                == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, "DEFAULT did not reset");
        require(names(pm.queryIntentActivities(launcher, null, 0, 0).getList())
                .contains(main.getClassName()), "re-enabled activity does not resolve");
        require(pm.getActivityInfo(main, 0, 0) != null, "re-enabled activity not found by name");

        throwsType(IllegalArgumentException.class, () -> pm.setComponentEnabledSetting(
                new ComponentName(PACKAGE, "NoSuchClass"),
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED, 0, 0, PACKAGE),
                "unknown component accepted");
        throwsType(IllegalArgumentException.class, () -> pm.setComponentEnabledSetting(main, 42,
                0, 0, PACKAGE), "invalid state accepted");
        throwsType(IllegalArgumentException.class, () -> pm.setComponentEnabledSetting(
                new ComponentName("com.example.missing", "X"),
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED, 0, 0, PACKAGE),
                "unknown package accepted");
        // Reading: as Settings.getComponentEnabledSettingLPr, an unknown class of a known
        // package is DEFAULT, an unknown package throws.
        require(pm.getComponentEnabledSetting(new ComponentName(PACKAGE, "NoSuchClass"), 0)
                == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, "unknown class not DEFAULT");
        throwsType(IllegalArgumentException.class, () -> pm.getComponentEnabledSetting(
                new ComponentName("com.example.missing", "X"), 0), "unknown package read");
        com.gesher.server.PackageManagerService service = (com.gesher.server.PackageManagerService) pm;
        com.gesher.server.ApkPackage replacement = com.gesher.server.ApkPackage.parse(service.get(PACKAGE).apkPath);
        Method add = service.getClass().getDeclaredMethod("add", com.gesher.server.ApkPackage.class);
        Method remove = service.getClass().getDeclaredMethod("remove", String.class);
        add.setAccessible(true);
        remove.setAccessible(true);
        pm.setComponentEnabledSetting(main, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP, 0, PACKAGE);
        add.invoke(service, replacement);
        require(pm.getComponentEnabledSetting(main, 0) == PackageManager.COMPONENT_ENABLED_STATE_DISABLED
                && pm.getActivityInfo(main, 0, 0) == null, "package update lost component state");
        remove.invoke(service, PACKAGE);
        add.invoke(service, replacement);
        require(pm.getComponentEnabledSetting(main, 0) == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
                && pm.getActivityInfo(main, 0, 0) != null, "uninstall retained component state");
        System.out.println("SettingsIntentsTest: component enabled states are kept and applied");
    }

    @SuppressWarnings("unchecked")
    private static void contentProviders(IPackageManager pm) throws Exception {
        ComponentName provider = new ComponentName(PACKAGE, PACKAGE + ".ItemsProvider");
        List<ResolveInfo> explicit = pm.queryIntentContentProviders(
                new Intent().setComponent(provider), null, 0, 0).getList();
        require(explicit.size() == 1 && explicit.get(0).providerInfo != null
                && provider.getClassName().equals(explicit.get(0).providerInfo.name),
                "explicit provider query: " + explicit);
        List<ResolveInfo> none = pm.queryIntentContentProviders(
                new Intent("android.content.action.DOCUMENTS_PROVIDER"), null, 0, 0).getList();
        require(none.isEmpty(), "a provider without that filter matched: " + none);

        // All consumers share the installed state: by name, authority, intent, package
        // metadata and the provider-start path must agree about a disabled provider.
        String authority = "com.gesher.components.items";
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> providersClass = Class.forName("com.gesher.server.ContentProviders");
        Method declared = providersClass.getDeclaredMethod("declaredProvider", pmClass, String.class);
        declared.setAccessible(true);
        pm.setComponentEnabledSetting(provider, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP, 0, PACKAGE);
        require(pm.getProviderInfo(provider, 0, 0) == null, "disabled provider found by name");
        require(pm.resolveContentProvider(authority, 0, 0) == null, "disabled provider found by authority");
        require(pm.queryIntentContentProviders(new Intent().setComponent(provider), null, 0, 0)
                .getList().isEmpty(), "disabled provider found by intent");
        require(declared.invoke(null, pm, authority) == null, "provider acquisition found disabled provider");
        android.content.pm.PackageInfo metadata = pm.getPackageInfo(PACKAGE, PackageManager.GET_PROVIDERS, 0);
        require(metadata.providers == null || metadata.providers.length == 0,
                "package metadata includes disabled provider");
        android.content.pm.ProviderInfo disabled = pm.resolveContentProvider(authority,
                PackageManager.MATCH_DISABLED_COMPONENTS, 0);
        require(disabled != null && !disabled.enabled, "disabled provider flag was lost");

        // An ENABLED override must also override a disabled manifest declaration.
        com.gesher.server.ApkPackage apk = ((com.gesher.server.PackageManagerService) pm).get(PACKAGE);
        com.android.internal.pm.pkg.component.ParsedProviderImpl parsed =
                (com.android.internal.pm.pkg.component.ParsedProviderImpl) apk.parsed.getProviders().get(0);
        boolean manifestEnabled = parsed.isEnabled();
        try {
            parsed.setEnabled(false);
            pm.setComponentEnabledSetting(provider, PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                    PackageManager.DONT_KILL_APP, 0, PACKAGE);
            require(pm.resolveContentProvider(authority, 0, 0).enabled,
                    "ENABLED did not override provider manifest state");
            require(declared.invoke(null, pm, authority) != null,
                    "enabled provider cannot be acquired");
            pm.setComponentEnabledSetting(provider, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                    PackageManager.DONT_KILL_APP, 0, PACKAGE);
            require(pm.resolveContentProvider(authority, 0, 0) == null,
                    "DEFAULT did not restore disabled manifest state");
        } finally {
            parsed.setEnabled(manifestEnabled);
            pm.setComponentEnabledSetting(provider, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                    PackageManager.DONT_KILL_APP, 0, PACKAGE);
        }
        require(pm.resolveContentProvider(authority, 0, 0) != null,
                "restored provider stayed disabled");
        System.out.println("SettingsIntentsTest: queryIntentContentProviders answers");
    }

    private static void storageStats(Object pm, Class<?> pmClass, File data) throws Exception {
        Class<?> statsClass = Class.forName("com.gesher.server.StorageStatsService");
        Constructor<?> constructor = statsClass.getDeclaredConstructor(pmClass, File.class);
        constructor.setAccessible(true);
        IStorageStatsManager stats = (IStorageStatsManager) constructor.newInstance(pm, data);

        // CE: 3000 bytes (1000 cache); DE: 1000 bytes (500 cache/code-cache);
        // external: 500 bytes of cache. Each private area must be counted once.
        File appData = new File(data, "data/" + PACKAGE);
        write(new File(appData, "files/a.bin"), 2000);
        write(new File(appData, "cache/c.bin"), 1000);
        write(new File(data, "media/0/Android/data/" + PACKAGE + "/cache/e.bin"), 500);
        write(new File(data, "media/0/Pictures/p.jpg"), 4000);
        write(new File(data, "media/0/Music/m.mp3"), 300);
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Object apk = pmClass.getMethod("get", String.class).invoke(pm, PACKAGE);
        apkClass.getMethod("setDataDir", String.class).invoke(apk, appData.getPath());
        File deviceData = new File((String) apkClass.getMethod("deviceProtectedDataDir").invoke(apk));
        write(new File(deviceData, "files/a.bin"), 500);
        write(new File(deviceData, "cache/c.bin"), 200);
        write(new File(deviceData, "code_cache/compiled.bin"), 300);

        require(stats.getTotalBytes(null, "gesher.services") > 0, "total bytes 0");
        require(stats.getFreeBytes(null, "gesher.services") > 0, "free bytes 0");
        require(!stats.isQuotaSupported(null, "gesher.services"), "quota claimed");
        StorageStats app = stats.queryStatsForPackage(null, PACKAGE, 0, "gesher.services");
        long apkBytes = new File(data, "base.apk").length();
        require(app.getAppBytes() == apkBytes, "code bytes " + app.getAppBytes() + " != " + apkBytes);
        require(app.getDataBytes() == 4500, "CE+DE data bytes " + app.getDataBytes());
        require(app.getCacheBytes() == 2000, "CE+DE cache bytes " + app.getCacheBytes());
        require(app.getExternalCacheBytes() == 500, "external cache " + app.getExternalCacheBytes());
        require(stats.getCacheBytes(null, "gesher.services") == 1500, "CE+DE volume cache bytes");
        throwsType(android.os.ParcelableException.class, () -> stats.queryStatsForPackage(null,
                "com.example.missing", 0, "gesher.services"), "unknown package answered");
        throwsType(IllegalArgumentException.class, () -> stats.getTotalBytes("no-such-volume",
                "gesher.services"), "unknown volume answered");
        int uid = (int) apkClass.getMethod("uid").invoke(apk);
        require(stats.queryStatsForUid(null, uid, "gesher.services").getDataBytes() == 4500,
                "uid stats differ from the package's");
        ExternalStorageStats shared = stats.queryExternalStatsForUser(null, 0, "gesher.services");
        require(shared.getTotalBytes() == 4800 && shared.getImageBytes() == 4000
                && shared.getAudioBytes() == 300 && shared.getAppBytes() == 500,
                "external stats: total " + shared.getTotalBytes() + " image "
                        + shared.getImageBytes() + " audio " + shared.getAudioBytes()
                        + " app " + shared.getAppBytes());
        // Completed scans must never serve as a stale size cache. This service
        // fixture also runs on Windows/ART with actual package/storage objects.
        File changed = new File(appData, "files/after-query.bin");
        write(changed, 123);
        require(stats.queryStatsForPackage(null, PACKAGE, 0, "gesher.services")
                .getDataBytes() == 4623, "storage stats reused a completed scan after write");
        require(changed.delete(), "cannot remove storage freshness fixture");
        require(stats.queryStatsForPackage(null, PACKAGE, 0, "gesher.services")
                .getDataBytes() == 4500, "storage stats reused a completed scan after delete");
        System.out.println("SettingsIntentsTest: storage stats measure the app's files");
    }

    private static void write(File file, int bytes) throws Exception {
        file.getParentFile().mkdirs();
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(new byte[bytes]);
        }
    }

    private static void standInDefaults() throws Exception {
        Class<?> am = Class.forName("com.gesher.server.ActivityManagerService");
        Method read = am.getDeclaredMethod("readMemInfo", File.class);
        read.setAccessible(true);
        File meminfo = File.createTempFile("meminfo", ".txt");
        Files.write(meminfo.toPath(), ("MemTotal:       16000000 kB\nMemFree:         2000000 kB\n"
                + "MemAvailable:    6000000 kB\nCached:          3000000 kB\n").getBytes("UTF-8"));
        long[] memory = (long[]) read.invoke(null, meminfo);
        require(memory[0] == 16000000L * 1024 && memory[1] == 6000000L * 1024
                && memory[2] == 5000000L * 1024, "meminfo parse: " + Arrays.toString(memory));
        long[] missing = (long[]) read.invoke(null, new File(meminfo.getPath() + ".missing"));
        require(missing[0] == 0 && missing[1] == 0, "missing meminfo invented sizes");

        Class<?> telephony = Class.forName("com.gesher.server.DeviceServices$TelephonyService");
        Constructor<?> tc = telephony.getDeclaredConstructor();
        tc.setAccessible(true);
        com.android.internal.telephony.ITelephony phone =
                (com.android.internal.telephony.ITelephony) tc.newInstance();
        require(phone.getSimStateForSlotIndex(0)
                == android.telephony.TelephonyManager.SIM_STATE_ABSENT, "SIM state not ABSENT");
        require("".equals(phone.getNetworkCountryIsoForPhone(0)),
                "network country ISO not empty string");
        require("".equals(phone.getNetworkCountryIsoForPhone(
                android.telephony.SubscriptionManager.DEFAULT_PHONE_INDEX)),
                "default-phone network country ISO not empty string");

        android.os.IUserManager users = new com.gesher.server.Services.UserManager();
        require("".equals(users.getProfileType(0)) && users.getProfileType(10) == null,
                "profile type not as UserManagerService answers");
        require(!users.isDemoUser(0), "demo user");
        System.out.println("SettingsIntentsTest: memory, SIM state and profile type answer as Android");
    }
}
