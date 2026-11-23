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
import static com.gesher.app.locale.UiText.text;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

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

    /**
     * Whether each package has an enabled launcher activity, for the Control Center
     * (AppShell's no-launcher-activity record): kept current on installs and enabled-state
     * changes (PackageManagerService calls the listener outside its monitor),
     * and brought up to date now from the installed packages, in the background, for packages
     * installed before the record existed. Returns that background thread, started.
     */
    static Thread trackLauncherRecords(PackageManagerService pm) {
        Object recordsLock = new Object();
        java.util.function.Consumer<ApkPackage> update = p -> {
            synchronized (recordsLock) {
                if (pm.get(p.packageName) == p && !pm.isRemovalPending(p.packageName)) {
                    com.gesher.app.shell.AppShell.updateLauncherRecordQuietly(p.packageName, p);
                }
            }
        };
        pm.setDesktopEnabledStateListener(update);
        Thread records = new Thread(() -> {
            List<ApkPackage> installed;
            synchronized (pm) {
                installed = new ArrayList<>(pm.packages());
            }
            // Use the same lock and current generation check as package events.
            for (ApkPackage p : installed) update.accept(p);
        }, "launcher-records");
        records.setDaemon(true);
        records.start();
        return records;
    }

    /** Select an explicit APK while the previous record still supplies its system trust policy. */
    static void selectCommandLinePackage(List<ApkPackage> selected, ApkPackage current) {
        for (ApkPackage previous : selected) {
            if (previous.packageName.equals(current.packageName) && previous.isSystemAddon()) {
                if (!GmsSignatureSpoofing.verifyAddonApk(current.packageName, current.apkPath)) {
                    throw new SecurityException("Untrusted microG addon override: " + current.packageName);
                }
                current.setSystemAddon(true);
            }
        }
        selected.removeIf(p -> p.packageName.equals(current.packageName));
        selected.add(current);
    }

    /**
     * The package for the staged microG addon slot {@code name}: the staged addon, or a
     * valid microG-signed installed update at least as new (both as system addons), or
     * null when an installed package of that name is verifiably signed by another
     * signer. Such a package is an ordinary installed app (Google's own Play Store, say):
     * it keeps its name, data and lack of privileges, the staged addon is not published,
     * and nothing else of the device changes. (AOSP's scan instead deletes a user app
     * whose signer differs from a new system package's, with its data; this optional
     * addon does not justify that.) Any other verification failure, such as changed
     * content of a microG update, still fails startup: it is not a different signer.
     */
    static ApkPackage selectStagedAddon(List<ApkPackage> installed, String name, ApkPackage staged) {
        List<ApkPackage> updates = new ArrayList<>();
        for (ApkPackage current : installed) {
            if (!name.equals(current.packageName)) continue;
            if (GmsSignatureSpoofing.verifyAddonApk(name, current.apkPath)) {
                updates.add(current);
            } else if (current.hasVerifiedSigningIdentity() && !current.hasVerifiedMicrogSigner()) {
                return null;
            } else {
                throw new SecurityException("Untrusted installed microG addon override: " + name);
            }
        }
        ApkPackage selected = staged;
        for (ApkPackage current : updates) {
            // Even when selecting the bundled fallback, the old committed
            // manifest is the authority for any legacy storage migration.
            current.setSystemAddon(true);
            // As an updated system app: preserve a valid newer user-installed
            // release and its privileges, with the staged APK as fallback.
            if (current.versionCode() >= selected.versionCode()) selected = current;
        }
        return selected;
    }

    /** Prepare only the final selection, retaining the committed records' migration authority. */
    static void preparePackageStorage(List<ApkPackage> committed, List<ApkPackage> selected)
            throws java.io.IOException {
        Set<ApkPackage> prepared = new HashSet<>();
        for (ApkPackage previous : committed) {
            try {
                previous.prepareInstalledDataDirectories();
                prepared.add(previous);
            } catch (Exception failure) {
                // Neither a newer APK nor a bundled fallback may classify a failed
                // committed layout on the old manifest's behalf.
                selected.removeIf(p -> p.packageName.equals(previous.packageName));
                log("installed package " + previous.packageName
                        + " storage could not be prepared: " + failure);
            }
        }
        for (ApkPackage current : selected) {
            if (!prepared.contains(current)) current.prepareDataDirectories();
        }
        // Keep the documented parser diagnostic, but emit it only after the
        // complete final selection is ready to publish. A later preparation
        // failure must not leave a misleading success summary for this boot.
        for (ApkPackage current : selected) log(current.describe());
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

    private static volatile boolean protectedDevice;

    static boolean isProtectedDevice() {
        return protectedDevice;
    }

    private static void run(String[] args) throws Exception {
        String buildProp = null;
        String frameworkRes = null;
        boolean persistent = false;
        boolean cohostOwner = false;
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
                case "--protected-launch": protectedDevice = true; break;
                case "--cohost-owner": cohostOwner = true; break;
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
        if (cohostOwner && (!persistent || !protectedDevice || !apks.isEmpty())) {
            throw new IllegalArgumentException("A cohost policy owner must be an empty protected host launch");
        }
        if (protectedDevice) {
            System.loadLibrary("android_runtime");
            if (adopt != null || !Zygote.nativeHasProtectedLaunchAuthority(!persistent)) {
                throw new IllegalStateException("Protected system server requires a live launch authority");
            }
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
        // The Windows runtime does not supply display vsync yet. Publish the
        // frame scheduling setting before contained apps read system properties.
        android.os.SystemProperties.set("debug.choreographer.vsync", "false");

        if (persistent) {
            // The device goes as a whole: binderd, servicemanager and the app processes
            // end with this process (a kill-on-close job; app processes join it as its
            // children).
            boolean job = protectedDevice || Zygote.joinJob(0);
            if (!protectedDevice && adopt != null) {
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
        List<ApkPackage> committed = persistent ? Installer.loadInstalled() : List.of();
        List<ApkPackage> packages = new ArrayList<>(committed);
        if (GmsSignatureSpoofing.isEnabled()) {
            // Deliberately only these addon slots, not arbitrary files in priv-app.
            for (String name : MicrogAddons.PACKAGES) {
                File apk = new File(androidRoot, "priv-app/" + name + "/base.apk");
                if (!apk.isFile()) continue;
                ApkPackage staged = MicrogAddons.load(apk, name);
                ApkPackage p = selectStagedAddon(packages, name, staged);
                if (p == null) {
                    log("microg addon " + name + ": not loaded, the installed " + name
                            + " has another signer and stays an ordinary app (uninstall it"
                            + " to use the microG addon)");
                    continue;
                }
                if (p == staged) {
                    // As the package manager's scan of a system package: its libraries
                    // on disk for every process that loads its code (Dynamite modules).
                    try {
                        int libs = MicrogAddons.prepareNativeLibraries(p);
                        if (libs > 0) {
                            log("microg addon " + name + ": " + libs + " native "
                                    + (libs == 1 ? "library" : "libraries") + " in "
                                    + p.nativeLibraryDir());
                        }
                    } catch (IOException | java.io.UncheckedIOException
                            | java.nio.file.InvalidPathException e) {
                        // Not published: the package keeps its default library path,
                        // which a device run does not fill, so it runs without native
                        // libraries until a later startup prepares them (fail closed).
                        log("microg addon " + name + ": native libraries not prepared: "
                                + e.getMessage());
                    }
                }
                packages.removeIf(old -> name.equals(old.packageName));
                packages.add(p);
                log("microg addon: " + p.describe());
            }
        }
        Map<ApkPackage, Zygote.StandaloneInput> pinnedInputs =
                selectExplicitPackages(packages, apks, protectedDevice);
        try (PackageUids.Admission identity = PackageUids.prepare(packages, committed)) {
            identity.commit();
            if (!cohostOwner) preparePackageStorage(committed, packages);
            identity.accept();
        }

        // Keep one PackageManager and policy writer for this root. An owner-only
        // process starts without publishing an Android device. On request it
        // publishes the usual services on these same live objects.
        PackageManagerService ownerPm = cohostOwner ? new PackageManagerService(packages) : null;
        PermissionRequests ownerPermissions = cohostOwner ? new PermissionRequests(ownerPm) : null;
        CohostOwnerService cohostControl = null;
        if (cohostOwner) {
            cohostControl = new CohostOwnerService(ownerPm, ownerPermissions,
                    Zygote.vmCommandLine(SystemServer.class.getName()));
            cohostControl.publish();
            startBinderThreads(TAG, 31);
            DeviceControl.writeReady(Device.cohostOwnerReadyFile());
            while (!cohostControl.awaitPromotion()) {
                if (idleTimeoutS > 0
                        && SystemClock.uptimeMillis() - cohostControl.lastUse() >= idleTimeoutS * 1000
                        && cohostControl.closeIfIdle()) {
                    return;
                }
            }
            preparePackageStorage(committed, packages);
        }

        // The services, published with servicemanager.
        Map<String, IBinder> services = cohostOwner ? Services.create(ownerPm, true, ownerPermissions).services
                : Services.create(packages);
        Services.Created created = Services.sLast;
        if (persistent && protectedDevice && cohostControl == null) {
            cohostControl = new CohostOwnerService(created.pm, created.atm.permissionRequests(),
                    Zygote.vmCommandLine(SystemServer.class.getName()));
            cohostControl.publish();
        }
        // Host installation can precede the first application bind. Initialize the
        // server's writable region and real package-cache nonce before publishing APIs.
        created.am.invalidatePermissionCaches();
        startAppProcessesOnDemand(created.atm, created.pm, pinnedInputs);
        Installer installer = persistent ? Installer.create(created.pm, created.atm) : null;
        if (!persistent) {
            Installer.attachMetadata(created.pm, created.atm);
        }
        DeviceControl device = persistent || protectedDevice
                ? new DeviceControl(created, installer, cohostControl, idleTimeoutS) : null;
        if (device != null) services.put(GesherService.NAME, new GesherService(device));
        for (Map.Entry<String, IBinder> e : services.entrySet()) {
            ServiceManager.addService(e.getKey(), e.getValue());
        }
        log("registered " + String.join(", ", services.keySet()) + " with servicemanager"
                + " (pid " + Process.myPid() + ")");

        // Binder threads: the server calls into app processes (bindApplication,
        // scheduleTransaction) while their main threads call it (attachApplication,
        // activityPaused), and startActivityAndWait and getContentProvider hold a thread
        // until an activity is idle or a provider is published. system_server runs up to
        // 31 (BinderInternal.setMaxThreads).
        if (!cohostOwner) startBinderThreads(TAG, 31);
        if (persistent) device.startPersistent();
        else if (protectedDevice) DeviceControl.writeReady(new File(Device.logsDir(), "ready"));
        if (persistent && GmsSignatureSpoofing.isEnabled()) {
            ApkPackage gms = created.pm.get(GmsPackageSupport.PACKAGE_GMS);
            if (gms != null && gms.hasVerifiedMicrogSigner()) {
                // Let microG restore its own enabled services after a device restart.
                // Its settings decide whether Check-in / Cloud Messaging is enabled.
                android.content.Intent boot = new android.content.Intent(
                        android.content.Intent.ACTION_BOOT_COMPLETED).setPackage(gms.packageName);
                created.am.broadcastIntentWithFeature(null, null, boot, null, null, 0, null,
                        null, null, null, null, -1, null, false, false, 0);
            }
        }

        // This thread serves binder calls from now on, as system_server's main thread
        // ends in Looper.loop() and its binder threads in joinThreadPool.
        Binder.joinThreadPool();
    }

    private static Map<ApkPackage, Zygote.StandaloneInput> selectExplicitPackages(
            List<ApkPackage> packages, List<String> apks, boolean pinCode) throws Exception {
        // Keep the actual selected code pinned for this temporary device's lifetime,
        // including subsequent process starts from the same package publication.
        Map<ApkPackage, Zygote.StandaloneInput> pinned = new java.util.IdentityHashMap<>();
        try {
            for (String apk : apks) {
                if (!pinCode) {
                    selectCommandLinePackage(packages, ApkPackage.parse(apk));
                    continue;
                }
                Zygote.StandaloneInput input = new Zygote.StandaloneInput(apk);
                try {
                    ApkPackage pkg = ApkPackage.parseStandaloneInput(input);
                    selectCommandLinePackage(packages, pkg);
                    pinned.put(pkg, input);
                } catch (Exception failure) {
                    input.close();
                    throw failure;
                }
            }
            pinned.entrySet().removeIf(entry -> {
                if (packages.contains(entry.getKey())) return false;
                entry.getValue().close();
                return true;
            });
            return Map.copyOf(pinned);
        } catch (Exception failure) {
            pinned.values().forEach(Zygote.StandaloneInput::close);
            throw failure;
        }
    }

    /**
     * App processes on demand: a dalvikvm like this one (its runtime options) running
     * android.app.GesherAppProcess for the package, its output appended to the package's
     * log.
     */
    private static void startAppProcessesOnDemand(ActivityTaskManagerService atm,
            PackageManagerService pm, Map<ApkPackage, Zygote.StandaloneInput> pinnedInputs) {
        final String vm;
        try {
            // With the JIT and its profile saver, as Android's app processes (ADR 0054).
            vm = Dexopt.appVmOptions(Zygote.vmCommandLine(SystemServer.class.getName()));
        } catch (Throwable t) {
            log("app processes are not started on demand (" + t + "): they attach by"
                    + " themselves");
            return;
        }
        final String binder = System.getenv("GESHER_BINDER");
        atm.setProcessStarter(new ActivityTaskManagerService.ProcessStarter() {
            @Override public int start(String packageName, long startSeq, int targetSdk) throws Exception {
                return start(packageName, packageName, startSeq, targetSdk);
            }
            @Override public int start(String packageName, String processName, long startSeq,
                    int targetSdk) throws Exception {
                return start(packageName, processName, startSeq, targetSdk,
                        WindowManagerService.resolveWindowMode(packageName, null));
            }
            @Override public int start(String packageName, String processName, long startSeq,
                    int targetSdk, String windowMode) throws Exception {
            File logFile = Device.packageLog(packageName);
            logFile.getParentFile().mkdirs();
            ApkPackage pkg = pm.get(packageName);
            if (pkg == null) throw new IllegalStateException("Unknown package " + packageName);
            if (protectedDevice && !atm.protectedStartCurrent(pkg, startSeq)) {
                throw new IllegalStateException("Contained start retired for " + packageName);
            }
            String uid = String.valueOf(pkg.uid());
            List<String> env = new java.util.ArrayList<>(List.of("GESHER_BINDER=" + binder,
                    "GESHER_UID=" + uid,
                    "GESHER_BINDER_UID=" + uid,
                    "GESHER_LOG_FILE=" + logFile.getPath() + ".logcat",
                    // The package's AppUserModelID, its Start menu shortcut's (shell track).
                    com.gesher.app.shell.AppShell.ENV_APP_USER_MODEL_ID + "="
                            + com.gesher.app.shell.AppShell.appUserModelId(packageName),
                    // The display the server describes, which the app's windows are on.
                    "GESHER_DISPLAY=" + com.gesher.app.DeviceProfile.describe()));
            // ATM captured this exact mode with its pending process, before spawning.
            if (com.gesher.app.window.WindowMode.parse(windowMode) == null) {
                throw new IllegalArgumentException("Missing resolved app window mode");
            }
            env.add(com.gesher.app.window.WindowMode.ENV + "=" + windowMode);
            // Android's INTERNET permission: no inet sockets without it (docs/network.md).
            String inet = ConnectivityService.internetEnvironment(packageName);
            if (inet != null) env.add(inet);
            // Gesher's own proxy for the package, over Windows' (docs/network.md).
            env.addAll(com.gesher.net.ProxySetting.environment(packageName));
            // One window for the app's tasks, or one each (ADR 0088).
            env.addAll(atm.appWindowsEnvironment(packageName, startSeq));
            String command = Zygote.commandLine(vm, APP_PROCESS, List.of("--package",
                    packageName, "--start-seq", String.valueOf(startSeq), "--target-sdk",
                    String.valueOf(targetSdk)));
            // Experimental (ADR 0090): the package's own AppContainer.
            if (AppSandbox.enabled()) {
                return AppSandbox.spawn(command, env, logFile, pkg, packageName,
                        ConnectivityService.requests(pkg, ConnectivityService.INTERNET), startSeq,
                        pinnedInputs.get(pkg));
            }
            return Zygote.spawn(command, env, logFile.getPath(), false);
            }
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

    /** Host control for a published persistent or launch-scoped device. */
    private static final class DeviceControl implements GesherService.Device {
        private final Services.Created mServices;
        private final Installer mInstaller;
        private final CohostOwnerService mCohostControl;
        private final long mIdleTimeoutMs;
        private final long mStarted = System.currentTimeMillis();
        private volatile long mLastCall = SystemClock.uptimeMillis();
        private volatile boolean mParked;
        private boolean mShuttingDown;

        DeviceControl(Services.Created services, Installer installer,
                      CohostOwnerService cohostControl, long idleTimeoutS) {
            mServices = services;
            mInstaller = installer;
            mCohostControl = cohostControl;
            mIdleTimeoutMs = idleTimeoutS * 1000;
        }

        /** How long a cached app process may stay unused before it is ended, by default. */
        private static final long CACHED_PROCESS_IDLE_S = 600;

        /**
         * Ends cached app processes nothing used for a while (ADR 0082): every
         * min(60 s, idle time / 4), those idle for {@code GESHER_CACHED_PROCESS_IDLE_S} seconds (600; 0 never ends them).
         */
        private void startIdleReaper() {
            long idleS = CACHED_PROCESS_IDLE_S;
            String s = System.getenv("GESHER_CACHED_PROCESS_IDLE_S");
            if (s != null) {
                try {
                    idleS = Long.parseLong(s.trim());
                } catch (NumberFormatException e) {
                    log("GESHER_CACHED_PROCESS_IDLE_S is not a number: " + s);
                }
            }
            if (idleS <= 0) return;
            log("cached app processes end after " + idleS + " s unused");
            final long idleMs = idleS > Long.MAX_VALUE / 1000 ? Long.MAX_VALUE : idleS * 1000;
            Thread reaper = new Thread(() -> {
                while (true) {
                    SystemClock.sleep(Math.min(60_000, Math.max(1000, idleMs / 4)));
                    try {
                        mServices.atm.reapIdleProcesses(idleMs);
                    } catch (Throwable t) {
                        log("idle reaper: " + t);
                    }
                }
            }, "process-reaper");
            reaper.setDaemon(true);
            reaper.start();
        }

        static void writeReady(File ready) throws IOException {
            ready.getParentFile().mkdirs();
            try (OutputStream out = new FileOutputStream(ready)) {
                out.write(String.valueOf(Process.myPid()).getBytes(StandardCharsets.UTF_8));
            }
        }

        void startPersistent() throws Exception {
            // The camera app, when the PC has a camera (hwsvc2, ADR 0051).
            CameraApp.sync(mInstaller, mServices.pm);
            trackLauncherRecords(mServices.pm);
            if (mCohostControl != null) mCohostControl.setRepublish(this::republish);
            // The launcher waits for this file: the services are registered.
            writeReady(Device.readyFile());
            long event = Zygote.createEvent(Device.shutdownEventName());
            Thread events = new Thread(() -> {
                while (true) {
                    if (Zygote.waitEvent(event, 60_000)) {
                        shutdown("the shutdown event was set (gesher-app shutdown)");
                    }
                }
            }, "shutdown-event");
            events.setDaemon(true);
            events.start();
            if (mIdleTimeoutMs > 0) {
                Thread idle = new Thread(() -> {
                    while (true) {
                        SystemClock.sleep(Math.min(5000, mIdleTimeoutMs));
                        long since = mServices.atm.idleSince();
                        long now = SystemClock.uptimeMillis();
                        if (mParked) {
                            if (now - mCohostControl.lastUse() >= mIdleTimeoutMs) {
                                closeParkedOwner();
                            }
                            continue;
                        }
                        // Scheduled alarms and jobs due within the timeout keep the
                        // device for at most one more timeout (devsvc, DeviceWork).
                        if ((mCohostControl == null || !mCohostControl.hasActiveLaunch())
                                && since != 0 && now - since >= mIdleTimeoutMs
                                && now - mLastCall >= mIdleTimeoutMs
                                && !DeviceWork.keepsIdleDevice(now - since, mIdleTimeoutMs)) {
                            shutdown("idle: no app in use for " + (mIdleTimeoutMs / 1000)
                                    + " s");
                        }
                    }
                }, "idle");
                idle.setDaemon(true);
                idle.start();
            }
            startIdleReaper();
            // Clicks on the apps' toasts, also those of earlier runs (hwsvc2, ADR 0051).
            NotificationManagerService notifications = NotificationManagerService.sLast;
            if (notifications != null) notifications.startActivator();
            log("the Gesher device is ready (idle timeout "
                    + (mIdleTimeoutMs > 0 ? (mIdleTimeoutMs / 1000) + " s" : "none") + ")");
        }

        private synchronized void closeParkedOwner() {
            if (mParked && mCohostControl.closeIfIdle()) System.exit(0);
        }

        private synchronized void republish() {
            if (!mParked) return;
            try {
                mServices.atm.resumeDeviceProcesses();
                writeReady(Device.readyFile());
                touch();
                mParked = false;
                mShuttingDown = false;
            } catch (IOException failure) {
                throw new IllegalStateException("Cannot publish the protected device", failure);
            }
        }

        private synchronized void park(java.util.Properties state) throws IOException {
            Device.write(Device.cohostOwnerFile(), state);
            writeReady(Device.cohostOwnerReadyFile());
            mParked = true;
            java.nio.file.Files.deleteIfExists(Device.readyFile().toPath());
            java.nio.file.Files.delete(Device.stateFile().toPath());
        }

        @Override
        public void touch() {
            mLastCall = SystemClock.uptimeMillis();
        }

        @Override
        public void toastActivated(String appId, String arguments) {
            NotificationManagerService n = NotificationManagerService.sLast;
            if (n != null) n.activated(appId, arguments, "a launcher the toast activator started");
        }

        @Override
        public String[] install(String apk) throws Exception {
            if (mInstaller == null) throw new UnsupportedOperationException("Temporary device");
            return mInstaller.install(apk);
        }

        @Override
        public boolean uninstall(String packageName) throws Exception {
            if (mInstaller == null) throw new UnsupportedOperationException("Temporary device");
            return mInstaller.uninstall(packageName);
        }

        @Override
        public int lastExitCode(String packageName) {
            Integer code = mServices.atm.lastExitCode(packageName);
            return code == null ? Integer.MIN_VALUE : code;
        }

        @Override
        public GesherService.LaunchResult startActivityAndWait(android.content.Intent intent) {
            return mServices.atm.startActivityForHost(intent);
        }
        @Override public GesherService.LaunchResult startShortcutAndWait(String pkg, String key) {
            return mServices.atm.startShortcutForHost(pkg, key);
        }

        @Override
        public String status() {
            StringBuilder sb = new StringBuilder();
            sb.append(text("device_status", System.getenv("GESHER_BINDER"), Process.myPid(),
                    (System.currentTimeMillis() - mStarted) / 1000, mServices.pm.packages().size()));
            for (ActivityTaskManagerService.ProcessRecord p : mServices.atm.processes()) {
                sb.append("\n  ").append(p).append(p.activities.isEmpty() ? ""
                        : text("activities_count", p.activities.size()));
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
                boolean stopped = true;
                try {
                    if (mCohostControl != null) mServices.atm.stopDeviceProcesses();
                    else mServices.atm.endAllProcesses(5000);
                } catch (RuntimeException failure) {
                    stopped = false;
                    log("could not stop app processes individually: " + failure);
                }
                mServices.atm.flushUsageStats();
                mServices.am.flushShortcutQuota();
                if (mInstaller != null) {
                    java.util.Properties state = Device.read(Device.stateFile());
                    if (state != null && Device.pid(state, Device.SYSTEM_SERVER) == Process.myPid()) {
                        if (stopped && mCohostControl != null && !mCohostControl.closeIfIdle()) {
                            try {
                                park(state);
                                log("the Gesher device stopped; an independent app is still running");
                                return;
                            } catch (IOException failure) {
                                log("could not park the protected policy owner: " + failure);
                            }
                        }
                        Device.stateFile().delete();
                    }
                    Device.readyFile().delete();
                }
                log("the Gesher device stopped");
                System.out.flush();
                // binderd and servicemanager are in this process's job: they end with it.
                System.exit(0);
            }, "shutdown");
            t.start();
        }
    }
}
