/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.app.GesherAppDriver;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.os.Handler;
import android.os.Looper;

import java.io.File;

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
 * </pre>
 *
 * <p>The driver runs from the boot class path (it is in {@code android.app} to reach
 * ActivityThread's package-private API, as Ravenwood's drivers are). See app/README.md.
 */
public final class Main {
    static final String TAG = "gesher-app";

    private Main() {
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
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            switch (a) {
                case "--build-prop": buildProp = value(args, ++i, a); break;
                case "--framework-res": frameworkRes = value(args, ++i, a); break;
                case "--activity": activity = value(args, ++i, a); break;
                case "--data-dir": dataDir = value(args, ++i, a); break;
                case "--strict-services": StandIn.sStrict = true; break;
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
            StandIn.sStrict = true;
        }
        String androidRoot = env("ANDROID_ROOT", "/system");
        if (buildProp == null) buildProp = androidRoot + "/build.prop";
        if (frameworkRes == null) frameworkRes = androidRoot + "/framework/framework-res.apk";
        apk = new File(apk).getAbsolutePath();

        // 1. The framework's natives (a builtin library of the runtime on Windows).
        System.loadLibrary("android_runtime");

        // 2. System properties, before anything reads Build.
        int props = BuildProps.load(buildProp);
        log("loaded " + props + " properties from " + buildProp);

        // 3. The main Looper; everything below runs on this thread.
        Looper.prepareMainLooper();

        // 4. The framework's resources. AssetManager's own path is the device's
        //    /system/framework/framework-res.apk; on Windows that is a path on the current
        //    drive, so the system assets are created here from $ANDROID_ROOT's copy first.
        if (!new File(frameworkRes).isFile()) {
            throw new IllegalStateException("framework-res.apk not found: " + frameworkRes);
        }
        AssetManager.createSystemAssetsInZygoteLocked(false, frameworkRes);

        // 5. The APK's manifest.
        AppManifest manifest = AppManifest.read(apk);
        ActivityInfo launch = manifest.launcherActivity(activity);
        ApplicationInfo appInfo = manifest.applicationInfo;
        if (dataDir != null) {
            manifest.setDataDir(new File(dataDir).getAbsolutePath());
        } else {
            manifest.setDataDir(env("ANDROID_DATA", "/data") + "/data/" + appInfo.packageName);
        }
        new File(appInfo.dataDir).mkdirs();
        log("package " + appInfo.packageName + " (targetSdk " + appInfo.targetSdkVersion
                + "), launching " + launch.name);

        // 6. Stand-in system services, through ServiceManager.
        StandInServices.install(manifest);

        // 7. The app and its Activity, on the main Looper.
        final Throwable[] failure = new Throwable[1];
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                GesherAppDriver.launch(manifest.applicationInfo, launch);
            } catch (Throwable t) {
                failure[0] = t;
            } finally {
                Looper.myLooper().quitSafely();
            }
        });
        Looper.loop();
        if (failure[0] != null) {
            throw new RuntimeException("launching " + launch.name + " failed", failure[0]);
        }
        log("done");
        return 0;
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
                + " [--activity CLASS] [--data-dir DIR] [--strict-services] <app.apk>");
    }
}
