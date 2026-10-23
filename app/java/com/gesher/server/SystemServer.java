/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.ServiceManager;
import android.os.SystemClock;

import com.gesher.app.Main;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * {@code SystemServer [options] [<app.apk>...]}: system_server, lite. Parses the APKs it is
 * given as PackageManagerService parses installed packages ({@link ApkPackage}), creates
 * the system services an app's lifecycle needs ({@link Services}: {@code package},
 * {@code activity}, {@code activity_task}, {@code window}, {@code display}, {@code user}...),
 * registers them with servicemanager and serves them on its binder threads. App processes
 * find them through ServiceManager; the server starts an app process for a package when it
 * needs one ({@link Zygote}, {@link ActivityTaskManagerService#startProcess}), and also
 * takes processes that attach by themselves ({@code gesher-app --system-server}).
 *
 * <p>With {@code --persistent} it is the Gesher device's ({@link Device}, ADR 0026): it
 * also serves the installed packages ({@link Installer}) and the {@code gesher} service
 * ({@link GesherService}: install, uninstall, status, shutdown), puts itself, binderd and
 * servicemanager ({@code --adopt PID,...}) and every app process in a job that ends them all
 * when it ends, and runs until {@code gesher-app shutdown} (an event or the gesher service)
 * or until no app process ran for {@code --idle-timeout} seconds (default 600; 0: never).
 * Without it, it serves until it is killed.
 *
 * <p>Needs binderd and servicemanager running on {@code $GESHER_BINDER}
 * (docs/services.md). Runs from the boot class path, after framework.jar, like gesher-app:
 * <pre>
 *   dalvikvm -Xbootclasspath:...:framework.jar:gesher-app.jar com.gesher.server.SystemServer app.apk
 * </pre>
 *
 * <p>Options: {@code --build-prop FILE}, {@code --framework-res FILE} (defaults under
 * $ANDROID_ROOT, as gesher-app's), {@code --strict-services}, {@code --persistent},
 * {@code --adopt PID[,PID...]}, {@code --idle-timeout SECONDS}.
 */
public final class SystemServer {
    static final String TAG = "system_server";
    /** The app processes' main class (android.app.GesherAppProcess.main). */
    private static final String APP_PROCESS = "android.app.GesherAppProcess";

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
        boolean persistent = false;
        String adopt = null;
        long idleTimeoutS = 600;
        List<String> apks = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            switch (a) {
                case "--build-prop": buildProp = args[++i]; break;
                case "--framework-res": frameworkRes = args[++i]; break;
                case "--strict-services": StandIn.setStrict(true); break;
                case "--persistent": persistent = true; break;
                case "--adopt": adopt = args[++i]; break;
                case "--idle-timeout": idleTimeoutS = Long.parseLong(args[++i]); break;
                default:
                    if (a.startsWith("-")) {
                        throw new IllegalArgumentException("usage: SystemServer [--build-prop FILE]"
                                + " [--framework-res FILE] [--strict-services] [--persistent"
                                + " [--adopt PID,...] [--idle-timeout SECONDS]] [<app.apk>...]");
                    }
                    apks.add(new File(a).getAbsolutePath());
            }
        }
        if (System.getenv("GESHER_BINDER") == null) {
            throw new IllegalStateException("$GESHER_BINDER is not set: the system server"
                    + " registers its services with servicemanager over binderd");
        }
        StandIn.setTag(TAG);
        // Binder calls from this process carry the system uid, as system_server's do
        // (gesher-base's /dev/binder reads it when the process opens the driver, below):
        // ActivityThread's ApplicationThread expects its callers to be the system.
        android.system.Os.setenv("GESHER_BINDER_UID", String.valueOf(Process.SYSTEM_UID), true);
        String androidRoot = System.getenv("ANDROID_ROOT");
        if (androidRoot == null || androidRoot.isEmpty()) androidRoot = "/system";
        if (buildProp == null) buildProp = androidRoot + "/build.prop";
        if (frameworkRes == null) frameworkRes = androidRoot + "/framework/framework-res.apk";

        Main.bootstrap(TAG, buildProp, frameworkRes);

        if (persistent) {
            // The device goes as a whole: binderd, servicemanager and the app processes
            // end with this process (a kill-on-close job; app processes join it as its
            // children).
            boolean job = Zygote.joinJob(0);
            if (adopt != null) {
                for (String pid : adopt.split(",")) {
                    if (!pid.isEmpty()) job &= Zygote.joinJob(Integer.parseInt(pid.trim()));
                }
            }
            log("the Gesher device " + System.getenv("GESHER_BINDER") + ": system server pid "
                    + Process.myPid() + (job ? ", its processes in one job"
                            : ", no job (processes may outlive it)"));
        }

        // The packages, as PackageManagerService scans them at boot: the installed ones
        // (a persistent device's), then the APKs given here.
        List<ApkPackage> packages = new ArrayList<>();
        if (persistent) {
            for (ApkPackage p : Installer.loadInstalled()) {
                packages.add(p);
                log("installed: " + p.describe());
            }
        }
        for (String apk : apks) {
            ApkPackage p = ApkPackage.parse(apk);
            new File(p.dataDir()).mkdirs();
            packages.add(p);
            log(p.describe());
        }

        // The services, published with servicemanager.
        Map<String, IBinder> services = Services.create(packages);
        Services.Created created = Services.sLast;
        // Providers answering other processes ask the permission checker.
        services.put("permission_checker", StandIn.of(
                android.permission.IPermissionChecker.class,
                new Services.PermissionChecker()).asBinder());
        startAppProcessesOnDemand(created.atm);
        PersistentDevice device = null;
        if (persistent) {
            device = new PersistentDevice(created, Installer.create(created.pm, created.atm),
                    idleTimeoutS);
            services.put(GesherService.NAME, new GesherService(device));
        }
        for (Map.Entry<String, IBinder> e : services.entrySet()) {
            ServiceManager.addService(e.getKey(), e.getValue());
        }
        log("registered " + String.join(", ", services.keySet()) + " with servicemanager"
                + " (pid " + Process.myPid() + ")");

        // Binder threads: the server calls into app processes (bindApplication,
        // scheduleTransaction) while their main threads call it (attachApplication,
        // activityPaused), and startActivityAndWait and getContentProvider hold a thread
        // until an activity is idle or a provider is published. system_server runs up to
        // 31 (BinderInternal.setMaxThreads); here a few.
        startBinderThreads(TAG, 7);
        if (device != null) device.start();

        // This thread serves binder calls from now on, as system_server's main thread
        // ends in Looper.loop() and its binder threads in joinThreadPool.
        Binder.joinThreadPool();
    }

    /**
     * App processes on demand: a dalvikvm like this one (its runtime options) running
     * android.app.GesherAppProcess for the package, its output appended to the package's
     * log.
     */
    private static void startAppProcessesOnDemand(ActivityTaskManagerService atm) {
        final String vm;
        try {
            vm = Zygote.vmCommandLine(SystemServer.class.getName());
        } catch (Throwable t) {
            log("app processes are not started on demand (" + t + "): they attach by"
                    + " themselves");
            return;
        }
        final String binder = System.getenv("GESHER_BINDER");
        atm.setProcessStarter((packageName, startSeq, targetSdk) -> {
            File logFile = Device.packageLog(packageName);
            logFile.getParentFile().mkdirs();
            return Zygote.spawn(Zygote.commandLine(vm, APP_PROCESS, List.of("--package",
                    packageName, "--start-seq", String.valueOf(startSeq), "--target-sdk",
                    String.valueOf(targetSdk))), List.of("GESHER_BINDER=" + binder,
                    // The package's AppUserModelID, its Start menu shortcut's (shell track).
                    com.gesher.app.shell.AppShell.ENV_APP_USER_MODEL_ID + "="
                            + com.gesher.app.shell.AppShell.appUserModelId(packageName)),
                    logFile.getPath(), false);
        });
    }

    /**
     * Starts {@code count} Java threads that join this process's binder thread pool, as
     * ProcessState.startThreadPool's pooled threads do (named {@code binder:<name>_N}).
     */
    public static void startBinderThreads(String name, int count) {
        for (int i = 1; i <= count; i++) {
            Thread t = new Thread(Binder::joinThreadPool, "binder:" + name + "_" + i);
            t.setDaemon(true);
            t.start();
        }
    }

    /** The persistent device's lifetime: ready, idle, shutdown. */
    private static final class PersistentDevice implements GesherService.Device {
        private final Services.Created mServices;
        private final Installer mInstaller;
        private final long mIdleTimeoutMs;
        private final long mStarted = System.currentTimeMillis();
        private volatile long mLastCall = SystemClock.uptimeMillis();
        private boolean mShuttingDown;

        PersistentDevice(Services.Created services, Installer installer, long idleTimeoutS) {
            mServices = services;
            mInstaller = installer;
            mIdleTimeoutMs = idleTimeoutS * 1000;
        }

        void start() throws Exception {
            // The launcher waits for this file: the services are registered.
            File ready = Device.readyFile();
            ready.getParentFile().mkdirs();
            try (OutputStream out = new FileOutputStream(ready)) {
                out.write(String.valueOf(Process.myPid()).getBytes(StandardCharsets.UTF_8));
            }
            long event = Zygote.createEvent(Device.shutdownEventName());
            Thread events = new Thread(() -> {
                while (!Zygote.waitEvent(event, 60_000)) {
                    // waiting
                }
                shutdown("the shutdown event was set (gesher-app shutdown)");
            }, "shutdown-event");
            events.setDaemon(true);
            events.start();
            if (mIdleTimeoutMs > 0) {
                Thread idle = new Thread(() -> {
                    while (true) {
                        SystemClock.sleep(Math.min(5000, mIdleTimeoutMs));
                        long since = mServices.atm.idleSince();
                        long now = SystemClock.uptimeMillis();
                        if (since != 0 && now - since >= mIdleTimeoutMs
                                && now - mLastCall >= mIdleTimeoutMs) {
                            shutdown("idle: no app process for " + (mIdleTimeoutMs / 1000)
                                    + " s");
                            return;
                        }
                    }
                }, "idle");
                idle.setDaemon(true);
                idle.start();
            }
            log("the Gesher device is ready (idle timeout "
                    + (mIdleTimeoutMs > 0 ? (mIdleTimeoutMs / 1000) + " s" : "none") + ")");
        }

        @Override
        public void touch() {
            mLastCall = SystemClock.uptimeMillis();
        }

        @Override
        public String[] install(String apk) throws Exception {
            return mInstaller.install(apk);
        }

        @Override
        public boolean uninstall(String packageName) throws Exception {
            return mInstaller.uninstall(packageName);
        }

        @Override
        public int lastExitCode(String packageName) {
            Integer code = mServices.atm.lastExitCode(packageName);
            return code == null ? Integer.MIN_VALUE : code;
        }

        @Override
        public String status() {
            StringBuilder sb = new StringBuilder();
            sb.append("device ").append(System.getenv("GESHER_BINDER"))
                    .append(", system server pid ").append(Process.myPid())
                    .append(", up ").append((System.currentTimeMillis() - mStarted) / 1000)
                    .append(" s, ").append(mServices.pm.packages().size())
                    .append(" packages");
            for (ActivityTaskManagerService.ProcessRecord p : mServices.atm.processes()) {
                sb.append("\n  ").append(p).append(p.activities.isEmpty() ? ""
                        : ", " + p.activities.size() + " activities");
            }
            return sb.toString();
        }

        /** Ends the app processes, then this process (and with it the job). */
        @Override
        public void shutdown(String reason) {
            synchronized (this) {
                if (mShuttingDown) return;
                mShuttingDown = true;
            }
            // After the reply to a gesher-app shutdown call has gone.
            Thread t = new Thread(() -> {
                SystemClock.sleep(200);
                log("shutting down: " + reason);
                mServices.atm.endAllProcesses(5000);
                java.util.Properties state = Device.read(Device.stateFile());
                if (state != null && Device.pid(state, Device.SYSTEM_SERVER) == Process.myPid()) {
                    Device.stateFile().delete();
                }
                Device.readyFile().delete();
                log("the Gesher device stopped");
                System.out.flush();
                // binderd and servicemanager are in this process's job: they end with it.
                System.exit(0);
            }, "shutdown");
            t.start();
        }
    }
}
