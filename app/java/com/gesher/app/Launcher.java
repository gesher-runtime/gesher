/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.app.ActivityManager;
import android.app.ActivityTaskManager;
import android.app.GesherAppProcess;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.app.WaitResult;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.ServiceManager;
import android.os.SystemClock;

import com.gesher.server.Device;
import com.gesher.server.GesherService;
import com.gesher.server.SystemServer;
import com.gesher.server.Zygote;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;

/**
 * {@code gesher-app}'s commands on the user's Gesher device (ADR 0026): one system server
 * shared by every launch, with installed packages and app processes it starts itself.
 *
 * <pre>
 *   Launcher [--build-prop FILE] [--framework-res FILE] COMMAND
 *     launch APK [--activity CLASS] [--ez KEY true|false]... [--keep-task]
 *                       install or update the APK, start its launcher activity through the
 *                       system server (am start -W), show the app's output, and once the
 *                       activity is idle remove its task (the app is headless; with
 *                       --keep-task it runs until it finishes); ends when the app's process
 *                       ends
 *     install APK       install or update the package
 *     uninstall PACKAGE
 *     list              the installed packages (IPackageManager.getInstalledPackages)
 *     status            the device: its processes
 *     start             start the device (if it is not running)
 *     shutdown          stop the device: its app processes, the system server,
 *                       servicemanager and binderd
 * </pre>
 *
 * Every command but shutdown and status starts the device if it does not run: binderd,
 * servicemanager and the system server ({@code --persistent}), detached (no console
 * window: they outlive the console), with a binder device of their own; it stops when shut
 * down, or after {@code $GESHER_IDLE_TIMEOUT} seconds (default 600) with no app process.
 * Launchers find it through {@link Device}'s state file, under a mutex, and clean up after
 * a device whose system server is gone.
 */
public final class Launcher {
    static final String TAG = "gesher-app";
    /** How long a launcher waits for a starting system server's services. */
    private static final long READY_TIMEOUT_MS = 300_000;

    private Launcher() {
    }

    private static void log(String message) {
        Main.log(message);
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
        System.exit(code);
    }

    private static int run(String[] args) throws Exception {
        String buildProp = null;
        String frameworkRes = null;
        int i = 0;
        for (; i < args.length && args[i].startsWith("--"); i++) {
            switch (args[i]) {
                case "--build-prop": buildProp = args[++i]; break;
                case "--framework-res": frameworkRes = args[++i]; break;
                default: return usage();
            }
        }
        if (i >= args.length) return usage();
        String command = args[i++];
        List<String> rest = new ArrayList<>();
        for (; i < args.length; i++) rest.add(args[i]);

        String androidRoot = System.getenv("ANDROID_ROOT");
        if (androidRoot == null || androidRoot.isEmpty()) androidRoot = "/system";
        if (buildProp == null) buildProp = androidRoot + "/build.prop";
        if (frameworkRes == null) frameworkRes = androidRoot + "/framework/framework-res.apk";
        Main.bootstrap(TAG, buildProp, frameworkRes);

        switch (command) {
            case "launch": return launch(rest);
            case "install": {
                if (rest.size() != 1) return usage();
                ensureDevice(true);
                install(gesher(), rest.get(0));
                return 0;
            }
            case "uninstall": {
                if (rest.size() != 1) return usage();
                ensureDevice(true);
                boolean removed = gesher().uninstall(rest.get(0));
                log(removed ? "uninstalled " + rest.get(0)
                        : rest.get(0) + " is not installed");
                return removed ? 0 : 1;
            }
            case "list": return list();
            case "start": {
                ensureDevice(true);
                log(gesher().status());
                return 0;
            }
            case "status": {
                if (ensureDevice(false) == null) {
                    log("no Gesher device is running");
                    return 0;
                }
                log(gesher().status());
                return 0;
            }
            case "shutdown": return shutdown();
            default: return usage();
        }
    }

    private static int usage() {
        System.err.println("usage: gesher-app launch APK [--activity CLASS] [--ez KEY"
                + " true|false]... [--keep-task] | install APK | uninstall PACKAGE | list"
                + " | status | start | shutdown");
        return 2;
    }

    // --- the device --------------------------------------------------------------------

    /**
     * Finds the running device, or starts one ({@code start}), and makes this process use
     * its binder device. Returns its state, or null when none runs and {@code start} is
     * false.
     */
    static Properties ensureDevice(boolean start) throws Exception {
        long lock = Zygote.lock(Device.lockName(), (int) (2 * READY_TIMEOUT_MS));
        if (lock == 0) throw new IllegalStateException("cannot take " + Device.lockName());
        try {
            Properties state = Device.read(Device.stateFile());
            if (state != null) {
                String name = state.getProperty(Device.STATE_DEVICE);
                if (Device.alive(state, Device.SYSTEM_SERVER) && waitReady(state)) {
                    use(state);
                    log("connected to the running Gesher device " + name + " (system server"
                            + " pid " + Device.pid(state, Device.SYSTEM_SERVER) + ")");
                    return state;
                }
                // Crashed or killed, or never got ready: what is left of it goes.
                log("cleaning up the Gesher device " + name + ": its system server is gone"
                        + " or does not answer");
                Device.killLeftovers(state);
                Device.stateFile().delete();
                Device.readyFile().delete();
            }
            if (!start) return null;
            state = startDevice();
            use(state);
            return state;
        } finally {
            Zygote.unlock(lock);
        }
    }

    /** Waits until the system server of {@code state} has registered its services. */
    private static boolean waitReady(Properties state) throws IOException {
        String pid = String.valueOf(Device.pid(state, Device.SYSTEM_SERVER));
        long deadline = SystemClock.uptimeMillis() + READY_TIMEOUT_MS;
        while (SystemClock.uptimeMillis() < deadline) {
            File ready = Device.readyFile();
            if (ready.isFile() && pid.equals(new String(Files.readAllBytes(ready.toPath()),
                    StandardCharsets.UTF_8).trim())) {
                return true;
            }
            if (!Device.alive(state, Device.SYSTEM_SERVER)) return false;
            SystemClock.sleep(200);
        }
        return false;
    }

    /** Binder calls of this process go to the device's binderd. */
    private static void use(Properties state) throws Exception {
        android.system.Os.setenv("GESHER_BINDER", state.getProperty(Device.STATE_DEVICE), true);
    }

    private static Properties startDevice() throws Exception {
        String name = "gesher-" + Long.toString(System.currentTimeMillis(), 36);
        File logs = Device.logsDir();
        logs.mkdirs();
        String exe = Zygote.exePath();
        String dir = exe.substring(0, exe.lastIndexOf('\\') + 1);
        List<String> env = List.of("GESHER_BINDER=" + name);
        for (String log : new String[] {"binderd.log", "servicemanager.log",
                "system_server.log"}) {
            new File(logs, log).delete();
        }
        log("starting the Gesher device " + name + " (logs in " + logs + ")");
        Properties state = new Properties();
        state.setProperty(Device.STATE_DEVICE, name);
        state.setProperty(Device.STATE_SHUTDOWN_EVENT, Device.shutdownEventName());
        state.setProperty("started", String.valueOf(System.currentTimeMillis()));
        int binderd = Zygote.spawn(Zygote.quote(dir + "binderd.exe") + " " + name, env,
                new File(logs, "binderd.log").getPath(), true);
        record(state, Device.BINDERD, binderd);
        int servicemanager = Zygote.spawn(Zygote.quote(dir + "servicemanager.exe"), env,
                new File(logs, "servicemanager.log").getPath(), true);
        record(state, Device.SERVICEMANAGER, servicemanager);
        Device.write(Device.stateFile(), state);

        String idle = System.getenv("GESHER_IDLE_TIMEOUT");
        List<String> args = new ArrayList<>(List.of("--persistent", "--adopt",
                binderd + "," + servicemanager));
        if (idle != null && !idle.isEmpty()) {
            args.add("--idle-timeout");
            args.add(idle);
        }
        int server = Zygote.spawn(Zygote.commandLine(
                Zygote.vmCommandLine(Launcher.class.getName()), SystemServer.class.getName(),
                args), env, new File(logs, "system_server.log").getPath(), true);
        record(state, Device.SYSTEM_SERVER, server);
        Device.write(Device.stateFile(), state);
        if (!waitReady(state)) {
            Device.killLeftovers(state);
            Device.stateFile().delete();
            printTail(new File(logs, "system_server.log"), 40);
            throw new IllegalStateException("the system server of the Gesher device " + name
                    + " did not get ready (its log: " + new File(logs, "system_server.log")
                    + ")");
        }
        log("started the Gesher device " + name + ": binderd pid " + binderd
                + ", servicemanager pid " + servicemanager + ", system server pid " + server);
        return state;
    }

    private static void record(Properties state, String name, int pid) {
        state.setProperty(name, String.valueOf(pid));
        state.setProperty(name + ".start", String.valueOf(Zygote.startTime(pid)));
    }

    /** The device's gesher service, waited for (it is registered with the others). */
    private static GesherService.Client gesher() {
        for (int i = 0; i < 300; i++) {
            GesherService.Client c = GesherService.Client.find();
            if (c != null) return c;
            SystemClock.sleep(100);
        }
        throw new IllegalStateException("no gesher service in the device's servicemanager");
    }

    private static int shutdown() throws Exception {
        long lock = Zygote.lock(Device.lockName(), (int) (2 * READY_TIMEOUT_MS));
        try {
            Properties state = Device.read(Device.stateFile());
            if (state == null) {
                log("no Gesher device is running");
                return 0;
            }
            String name = state.getProperty(Device.STATE_DEVICE);
            if (Device.alive(state, Device.SYSTEM_SERVER)) {
                Zygote.signalEvent(state.getProperty(Device.STATE_SHUTDOWN_EVENT,
                        Device.shutdownEventName()));
                long deadline = SystemClock.uptimeMillis() + 30_000;
                while (Device.alive(state, Device.SYSTEM_SERVER)
                        && SystemClock.uptimeMillis() < deadline) {
                    SystemClock.sleep(200);
                }
            }
            Device.killLeftovers(state);
            Device.stateFile().delete();
            Device.readyFile().delete();
            log("the Gesher device " + name + " stopped");
            return 0;
        } finally {
            Zygote.unlock(lock);
        }
    }

    // --- packages ----------------------------------------------------------------------

    private static String install(GesherService.Client gesher, String apk) throws Exception {
        File f = new File(apk);
        if (!f.isFile()) throw new IllegalArgumentException("no such file: " + apk);
        String[] r = gesher.install(f.getAbsolutePath());
        switch (r[1]) {
            case "unchanged":
                log(r[0] + " is installed and unchanged (" + r[2] + ")");
                break;
            default:
                log(r[1] + " " + r[0] + " (version " + r[3] + ") in " + r[2]);
        }
        return r[0];
    }

    private static int list() throws Exception {
        ensureDevice(true);
        gesher();
        IPackageManager pm = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package"));
        @SuppressWarnings("unchecked")
        List<PackageInfo> packages = pm.getInstalledPackages(0, 0).getList();
        log(packages.size() + " installed " + (packages.size() == 1 ? "package" : "packages"));
        for (PackageInfo p : packages) {
            // getPackageInfo for each, as `pm dump` asks.
            PackageInfo full = pm.getPackageInfo(p.packageName, PackageManager.GET_ACTIVITIES
                    | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS, 0);
            System.out.println("package:" + p.packageName + " version " + p.versionName + " ("
                    + p.getLongVersionCode() + ") " + p.applicationInfo.sourceDir + ", "
                    + count(full.activities) + " activities, " + count(full.services)
                    + " services, " + count(full.providers) + " providers");
        }
        return 0;
    }

    private static int count(Object[] a) {
        return a == null ? 0 : a.length;
    }

    // --- launching ---------------------------------------------------------------------

    private static int launch(List<String> args) throws Exception {
        String apk = null;
        String activity = null;
        boolean keepTask = false;
        Map<String, Boolean> extras = new LinkedHashMap<>();
        for (int i = 0; i < args.size(); i++) {
            String a = args.get(i);
            switch (a) {
                case "--activity": activity = args.get(++i); break;
                case "--keep-task": keepTask = true; break;
                case "--ez": {
                    String key = args.get(++i);
                    extras.put(key, Boolean.parseBoolean(args.get(++i)));
                    break;
                }
                default:
                    if (a.startsWith("-") || apk != null) return usage();
                    apk = a;
            }
        }
        if (apk == null) return usage();
        ensureDevice(true);
        GesherService.Client gesher = gesher();
        String pkg = install(gesher, apk);

        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (activity == null) {
            intent.setPackage(pkg);
        } else {
            String cls = activity.startsWith(".") ? pkg + activity
                    : activity.indexOf('.') < 0 ? pkg + "." + activity : activity;
            intent.setComponent(new ComponentName(pkg, cls));
        }
        for (Map.Entry<String, Boolean> e : extras.entrySet()) {
            intent.putExtra(e.getKey(), (boolean) e.getValue());
        }

        // The app's output: its process appends to the package's log.
        Tail tail = new Tail(Device.packageLog(pkg));
        tail.start();
        try {
            log("starting " + intent.toShortString(false, true, true, false)
                    + " through the system server");
            IActivityTaskManager atm = ActivityTaskManager.getService();
            WaitResult result = atm.startActivityAndWait(null /* caller: the launcher */,
                    "com.gesher.launcher", null, intent, null, null, null, 0, 0, null, null, 0);
            log("startActivityAndWait: result " + result.result + ", "
                    + (result.who != null ? result.who.flattenToShortString() : "no activity")
                    + (result.timeout ? ", timed out" : "") + ", " + result.totalTime + " ms");
            if (result.result != ActivityManager.START_SUCCESS) {
                throw new IllegalStateException("the system server did not start " + intent);
            }
            if (!keepTask) {
                ActivityManager.RunningTaskInfo task = GesherAppProcess.taskOf(atm, pkg);
                if (task != null) {
                    log("removing task " + task.taskId + " ("
                            + task.baseActivity.flattenToShortString() + ")");
                    atm.removeTask(task.taskId);
                }
            }
            // Until the package's process ends: the server ends it with its last activity
            // (and service, and provider client).
            IActivityManager am = ActivityManager.getService();
            while (running(am, pkg)) {
                SystemClock.sleep(300);
            }
            int exit = Integer.MIN_VALUE;
            for (int i = 0; i < 40 && exit == Integer.MIN_VALUE; i++) {
                exit = gesher.lastExitCode(pkg);
                if (exit == Integer.MIN_VALUE) SystemClock.sleep(250);
            }
            tail.finish();
            log("the process of " + pkg + " ended"
                    + (exit != Integer.MIN_VALUE ? " (exit code " + exit + ")" : ""));
            if (exit != 0 && exit != Integer.MIN_VALUE) {
                log("failed: the app's process exited with code " + exit);
                return 1;
            }
            log("done");
            return 0;
        } finally {
            tail.finish();
        }
    }

    private static boolean running(IActivityManager am, String pkg) throws Exception {
        List<ActivityManager.RunningAppProcessInfo> processes = am.getRunningAppProcesses();
        if (processes == null) return false;
        for (ActivityManager.RunningAppProcessInfo p : processes) {
            if (pkg.equals(p.processName)) return true;
        }
        return false;
    }

    /** Copies what is appended to a file to stdout, from its length when started. */
    private static final class Tail extends Thread {
        private final File mFile;
        private final long mStart;
        private volatile boolean mDone;

        Tail(File file) {
            super("tail " + file.getName());
            setDaemon(true);
            mFile = file;
            mStart = file.length();
        }

        @Override
        public void run() {
            long offset = mStart;
            byte[] buffer = new byte[16 * 1024];
            while (true) {
                boolean last = mDone;
                long length = mFile.length();
                if (length < offset) offset = 0;  // a new log
                if (length > offset) {
                    try (InputStream in = new FileInputStream(mFile)) {
                        in.skip(offset);
                        for (int n; (n = in.read(buffer)) > 0; ) {
                            System.out.write(buffer, 0, n);
                            offset += n;
                        }
                        System.out.flush();
                    } catch (IOException e) {
                        // try again
                    }
                }
                if (last) return;
                SystemClock.sleep(150);
            }
        }

        /** Copies the rest and stops. */
        void finish() {
            if (!isAlive()) return;
            mDone = true;
            try {
                join(5000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private static void printTail(File file, int lines) {
        try {
            List<String> all = Files.readAllLines(file.toPath(), StandardCharsets.UTF_8);
            for (String l : all.subList(Math.max(0, all.size() - lines), all.size())) {
                System.err.println("  " + l);
            }
        } catch (IOException e) {
            // no log
        }
    }
}
