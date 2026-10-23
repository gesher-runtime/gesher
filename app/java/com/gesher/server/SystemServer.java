/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.ServiceManager;

import com.gesher.app.Main;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * {@code SystemServer [options] <app.apk>...}: system_server, lite. Parses the APKs it is
 * given as PackageManagerService parses installed packages ({@link ApkPackage}), creates
 * the system services an app's lifecycle needs ({@link Services}: {@code package},
 * {@code activity}, {@code activity_task}, {@code window}, {@code display}, {@code user}),
 * registers them with servicemanager and serves them on its binder threads until it is
 * killed. App processes find them through ServiceManager ({@code gesher-app
 * --system-server}).
 *
 * <p>Needs binderd and servicemanager running on {@code $GESHER_BINDER}
 * (docs/services.md). Runs from the boot class path, after framework.jar, like gesher-app:
 * <pre>
 *   dalvikvm -Xbootclasspath:...:framework.jar:gesher-app.jar com.gesher.server.SystemServer app.apk
 * </pre>
 *
 * <p>Options: {@code --build-prop FILE}, {@code --framework-res FILE} (defaults under
 * $ANDROID_ROOT, as gesher-app's), {@code --strict-services}.
 */
public final class SystemServer {
    static final String TAG = "system_server";

    private SystemServer() {
    }

    static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    public static void main(String[] args) {
        try {
            run(args);
        } catch (Throwable t) {
            System.out.flush();
            System.err.println(TAG + ": failed: " + t);
            t.printStackTrace();
            System.err.flush();
            System.exit(1);
        }
    }

    private static void run(String[] args) throws Exception {
        String buildProp = null;
        String frameworkRes = null;
        List<String> apks = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            switch (a) {
                case "--build-prop": buildProp = args[++i]; break;
                case "--framework-res": frameworkRes = args[++i]; break;
                case "--strict-services": StandIn.setStrict(true); break;
                default:
                    if (a.startsWith("-")) {
                        throw new IllegalArgumentException("usage: SystemServer [--build-prop FILE]"
                                + " [--framework-res FILE] [--strict-services] <app.apk>...");
                    }
                    apks.add(new File(a).getAbsolutePath());
            }
        }
        if (System.getenv("GESHER_BINDER") == null) {
            throw new IllegalStateException("$GESHER_BINDER is not set: the system server"
                    + " registers its services with servicemanager over binderd");
        }
        StandIn.setTag(TAG);
        String androidRoot = System.getenv("ANDROID_ROOT");
        if (androidRoot == null || androidRoot.isEmpty()) androidRoot = "/system";
        if (buildProp == null) buildProp = androidRoot + "/build.prop";
        if (frameworkRes == null) frameworkRes = androidRoot + "/framework/framework-res.apk";

        Main.bootstrap(TAG, buildProp, frameworkRes);

        // The packages, as PackageManagerService scans them at boot.
        List<ApkPackage> packages = new ArrayList<>();
        for (String apk : apks) {
            ApkPackage p = ApkPackage.parse(apk);
            new File(p.dataDir()).mkdirs();
            packages.add(p);
            log(p.describe());
        }

        // The services, published with servicemanager.
        Map<String, IBinder> services = Services.create(packages);
        for (Map.Entry<String, IBinder> e : services.entrySet()) {
            ServiceManager.addService(e.getKey(), e.getValue());
        }
        log("registered " + String.join(", ", services.keySet()) + " with servicemanager"
                + " (pid " + Process.myPid() + ")");

        // This thread serves binder calls from now on, as system_server's main thread
        // ends in Looper.loop() and its binder threads in joinThreadPool.
        Binder.joinThreadPool();
    }
}
