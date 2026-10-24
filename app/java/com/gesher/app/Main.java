/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.app.Activity;
import android.app.GesherAppDriver;
import android.app.GesherAppProcess;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.parsing.ApkLite;
import android.content.pm.parsing.ApkLiteParseUtils;
import android.content.pm.parsing.result.ParseResult;
import android.content.pm.parsing.result.ParseTypeImpl;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ServiceManager;

import com.gesher.app.window.WindowMode;
import com.gesher.server.ApkPackage;
import com.gesher.server.Services;
import com.gesher.server.StandIn;

import java.io.File;
import java.util.List;

/**
 * {@code gesher-app [options] <app.apk>}: runs an APK's launcher Activity through attach,
 * create, start and resume on a main Looper, headless, then exits.
 *
 * <p>Options:
 * <pre>
 *   --build-prop FILE     system properties (default $ANDROID_ROOT/build.prop)
 *   --framework-res FILE  framework-res.apk (default $ANDROID_ROOT/framework/framework-res.apk)
 *   --activity CLASS      the Activity to launch (default: the manifest's launcher Activity)
 *   --data-dir DIR        the app's data directory (default $ANDROID_DATA/data/PACKAGE)
 *   --strict-services     a stand-in service method that is not implemented throws
 *                         instead of returning a default value
 *   --system-server       run as an app process attached to a running
 *                         com.gesher.server.SystemServer (given the APK), through
 *                         servicemanager over binderd ($GESHER_BINDER): the server binds
 *                         the app and drives its activity (android.app.GesherAppProcess),
 *                         instead of parsing the APK and serving stand-ins in this process
 *   --ez KEY true|false   a boolean extra in the launch intent (--system-server), as am
 *                         start's
 *   --keep-task           (--system-server) the launcher does not remove the task once the
 *                         activity is idle: the run ends when the app's last activity
 *                         finishes
 *   --window              show the Activity's UI in a Win32 window and run until the
 *                         window is closed (closing it finishes the Activity)
 *   --hidden-window       the same with the window never shown (tests read it back)
 *   --display WxH[@DPI]   the display (and window) size; with a window the default is
 *                         com.gesher.app.window.WindowMode.DEFAULT_DISPLAY
 * </pre>
 *
 * <p>The driver runs from the boot class path (it is in {@code android.app} to reach
 * ActivityThread's package-private API, as Ravenwood's drivers are). See app/README.md.
 */
public final class Main {
    static final String TAG = "gesher-app";

    private Main() {
    }

    /** Ends the main Looper's loop (see run). */
    private static final class EndLoop extends RuntimeException {
        EndLoop() {
            super(null, null, false, false);
        }
    }

    /** Prints a line on stdout, prefixed {@code gesher-app: }. */
    public static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    public static void main(String[] args) {
        int code;
        try {
            code = run(args);
        } catch (Throwable t) {
            System.out.flush();
            System.err.println(TAG + ": failed: " + t);
            t.printStackTrace();
            code = 1;
        }
        System.out.flush();
        System.err.flush();
        // Framework code may have started threads (HandlerThreads, the finalizer watchdog);
        // the process ends here regardless.
        System.exit(code);
    }

    private static int run(String[] args) throws Exception {
        String apk = null;
        String buildProp = null;
        String frameworkRes = null;
        String activity = null;
        String dataDir = null;
        boolean systemServer = false;
        boolean window = false;
        boolean showWindow = true;
        String display = null;
        boolean keepTask = false;
        java.util.Map<String, Boolean> extras = new java.util.LinkedHashMap<>();
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            switch (a) {
                case "--build-prop": buildProp = value(args, ++i, a); break;
                case "--framework-res": frameworkRes = value(args, ++i, a); break;
                case "--activity": activity = value(args, ++i, a); break;
                case "--data-dir": dataDir = value(args, ++i, a); break;
                case "--strict-services": StandIn.setStrict(true); break;
                case "--system-server": systemServer = true; break;
                case "--window": window = true; break;
                case "--hidden-window": window = true; showWindow = false; break;
                case "--display": display = value(args, ++i, a); break;
                case "--keep-task": keepTask = true; break;
                case "--ez": {
                    String key = value(args, ++i, a);
                    extras.put(key, Boolean.parseBoolean(value(args, ++i, a)));
                    break;
                }
                case "-h":
                case "--help":
                    usage();
                    return 0;
                default:
                    if (a.startsWith("-") || apk != null) {
                        usage();
                        return 2;
                    }
                    apk = a;
            }
        }
        if (apk == null) {
            usage();
            return 2;
        }
        if (System.getenv("GESHER_APP_STRICT_SERVICES") != null) {
            StandIn.setStrict(true);
        }
        String androidRoot = env("ANDROID_ROOT", "/system");
        if (buildProp == null) buildProp = androidRoot + "/build.prop";
        if (frameworkRes == null) frameworkRes = androidRoot + "/framework/framework-res.apk";
        apk = new File(apk).getAbsolutePath();

        if (window) {
            WindowMode.setUpDisplay(display);
        } else if (display != null) {
            com.gesher.app.DeviceProfile.parseAndSet(display);
        }

        // 0-4. The runtime: JCA, the framework's natives, the system properties, the main
        //      Looper, the framework's resources.
        bootstrap(TAG, buildProp, frameworkRes);

        // 4b. The system fonts ($ANDROID_ROOT/etc/fonts.xml), which the app process gets
        //     from the system server in bindApplication on a device (docs/fonts.md).
        try {
            String fonts = com.gesher.app.fonts.SystemFontMap.load();
            log("system fonts: " + (fonts != null ? fonts
                    : "none (no " + com.gesher.app.fonts.SystemFontMap.fontsXml() + ")"));
        } catch (Exception e) {
            log("system fonts: not loaded: " + e);
        }

        // 5-6. The package and the system services: parsed and served in this process, or
        //      asked of a SystemServer process over binder.
        ApplicationInfo appInfo;
        ActivityInfo launch;
        if (systemServer) {
            // The app process the framework's way (android.app.GesherAppProcess): attached
            // to the system server, which binds it and drives its activity with client
            // transactions; this process also plays the launcher (am start -W).
            IPackageManager pm = remotePackageManager();
            String packageName = packageName(apk);
            appInfo = pm.getApplicationInfo(packageName, PackageManager.GET_META_DATA
                    | PackageManager.GET_SHARED_LIBRARY_FILES, 0);
            if (appInfo == null) {
                throw new IllegalStateException("the system server does not know "
                        + packageName + " (start com.gesher.server.SystemServer with " + apk
                        + ")");
            }
            log("package " + packageName + " from the system server's package manager: "
                    + appInfo.sourceDir);
            if (dataDir != null) {
                log("--data-dir ignored: the system server's package manager sets the data"
                        + " directory");
            }
            // Android's INTERNET permission for this app process (docs/network.md).
            com.gesher.server.ConnectivityService.applyInternetPermission(
                    pm.getPackageInfo(packageName, PackageManager.GET_PERMISSIONS, 0));
            Intent intent = launchIntent(packageName, activity);
            for (java.util.Map.Entry<String, Boolean> e : extras.entrySet()) {
                intent.putExtra(e.getKey(), (boolean) e.getValue());
            }
            log("starting " + intent.toShortString(false, true, true, false)
                    + " through the system server");
            // With a window the run ends when it is closed (the task stays).
            GesherAppProcess.run(intent, appInfo.targetSdkVersion, !keepTask && !window,
                    !window ? WindowMode.HEADLESS
                            : showWindow ? WindowMode.WINDOW : WindowMode.HIDDEN);
            log("done");
            return 0;
        } else {
            ApkPackage pkg = ApkPackage.parse(apk);
            log(pkg.describe());
            if (dataDir != null) {
                pkg.setDataDir(new File(dataDir).getAbsolutePath());
            }
            appInfo = pkg.applicationInfo(PackageManager.GET_META_DATA
                    | PackageManager.GET_SHARED_LIBRARY_FILES);
            launch = pkg.launcherActivity(activity);
            Services.installInProcess(Services.create(List.of(pkg)));
            com.gesher.server.ConnectivityService.applyInternetPermission(pkg);  // docs/network.md
            log("stand-in services in this process: "
                    + String.join(", ", ServiceManager.listServices()));
        }
        launch.applicationInfo = appInfo;
        if (window) {
            // The app's windows are Win32 windows drawn in this process (WindowSession).
            WindowMode.install(showWindow);
        }
        new File(appInfo.dataDir).mkdirs();
        // The APK's lib/x86_64 into nativeLibraryDir, for System.loadLibrary (NativeLibs).
        int libs = NativeLibs.extract(apk, appInfo.nativeLibraryDir);
        if (libs > 0) {
            log("extracted " + libs + " native " + (libs == 1 ? "library" : "libraries")
                    + " (" + NativeLibs.ABI + ") to " + appInfo.nativeLibraryDir);
        }
        log("package " + appInfo.packageName + " (targetSdk " + appInfo.targetSdkVersion
                + "), launching " + launch.name);

        // 7. The app and its Activity, on the main Looper. The main Looper may not
        //    quit (Looper.prepareMainLooper), so the launch ends the loop the way
        //    quitSafely would: a message posted after it, once the messages it left
        //    behind have run, throws EndLoop out of Looper.loop().
        final ApplicationInfo app = appInfo;
        final ActivityInfo target = launch;
        final Throwable[] failure = new Throwable[1];
        final Handler main = new Handler(Looper.getMainLooper());
        final boolean windowed = window;
        main.post(() -> {
            boolean ended = !windowed;
            try {
                Activity launched = GesherAppDriver.launch(app, target);
                if (windowed) {
                    // Runs until the window is closed: then the loop ends as below.
                    WindowMode.show(launched, () -> main.post(() -> {
                        throw new EndLoop();
                    }));
                }
            } catch (Throwable t) {
                failure[0] = t;
                ended = true;
            } finally {
                if (ended) {
                    main.post(() -> {
                        throw new EndLoop();
                    });
                }
            }
        });
        try {
            Looper.loop();
        } catch (EndLoop expected) {
            // The launch and the messages it left behind ran.
        }
        if (failure[0] != null) {
            throw new RuntimeException("launching " + launch.name + " failed", failure[0]);
        }
        log("done");
        return 0;
    }

    /**
     * Steps 0-4 of starting a process, shared with com.gesher.server.SystemServer: JCA
     * providers and digests, the framework's natives, the system properties, the main
     * Looper and the framework's resources. {@code tag} prefixes the log line.
     */
    public static void bootstrap(String tag, String buildProp, String frameworkRes)
            throws Exception {
        // 0. JCA providers that exist on Gesher's boot class path, and digests
        //    (Digests): before anything uses JCA.
        Digests.install();

        // 1. The framework's natives (a builtin library of the runtime on Windows).
        System.loadLibrary("android_runtime");

        // 2. System properties, before anything reads Build.
        int props = BuildProps.load(buildProp);
        System.out.println(tag + ": loaded " + props + " properties from " + buildProp);
        System.out.flush();

        // 3. The main Looper.
        Looper.prepareMainLooper();

        // 4. The framework's resources. AssetManager's own path is the device's
        //    /system/framework/framework-res.apk; on Windows that is a path on the current
        //    drive, so the system assets are created here from $ANDROID_ROOT's copy first.
        if (!new File(frameworkRes).isFile()) {
            throw new IllegalStateException("framework-res.apk not found: " + frameworkRes);
        }
        AssetManager.createSystemAssetsInZygoteLocked(false, frameworkRes);
    }

    /** The system server's package manager, waited for through servicemanager. */
    private static IPackageManager remotePackageManager() throws InterruptedException {
        if (System.getenv("GESHER_BINDER") == null) {
            throw new IllegalStateException("--system-server needs $GESHER_BINDER (binderd,"
                    + " servicemanager and com.gesher.server.SystemServer serving it)");
        }
        IBinder binder = null;
        for (int i = 0; i < 600 && binder == null; i++) {
            binder = ServiceManager.checkService("package");
            if (binder == null) Thread.sleep(100);
        }
        if (binder == null) {
            throw new IllegalStateException("no package service in servicemanager after 60 s");
        }
        log("system services from servicemanager: "
                + String.join(", ", ServiceManager.listServices())
                + (binder instanceof Binder ? " (in this process)" : " (in another process)"));
        return IPackageManager.Stub.asInterface(binder);
    }

    /** The package name in the APK's manifest (ApkLiteParseUtils, as installers read it). */
    private static String packageName(String apk) {
        ParseResult<ApkLite> lite = ApkLiteParseUtils.parseApkLite(
                ParseTypeImpl.forParsingWithoutPlatformCompat(), new File(apk), 0);
        if (lite.isError()) {
            throw new IllegalStateException("reading " + apk + ": " + lite.getErrorMessage(),
                    lite.getException());
        }
        return lite.getResult().getPackageName();
    }

    /**
     * The launch intent, as the launcher makes it: MAIN/LAUNCHER in the package (as
     * getLaunchIntentForPackage), or {@code requested} explicitly.
     */
    private static Intent launchIntent(String packageName, String requested) {
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (requested == null) {
            return intent.setPackage(packageName);
        }
        String name = requested.startsWith(".") ? packageName + requested
                : requested.indexOf('.') < 0 ? packageName + "." + requested : requested;
        return intent.setComponent(new ComponentName(packageName, name));
    }

    private static String value(String[] args, int i, String option) {
        if (i >= args.length) {
            throw new IllegalArgumentException(option + " needs a value");
        }
        return args[i];
    }

    private static String env(String name, String fallback) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? fallback : v;
    }

    private static void usage() {
        System.err.println("usage: gesher-app [--build-prop FILE] [--framework-res FILE]"
                + " [--activity CLASS] [--data-dir DIR] [--strict-services] [--system-server"
                + " [--ez KEY true|false]... [--keep-task]] [--window | --hidden-window]"
                + " [--display WxH[@DPI]] <app.apk>");
    }
}
