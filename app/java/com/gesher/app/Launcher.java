/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.app.ActivityManager;
import android.app.ActivityOptions;
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
import android.net.Uri;
import android.os.ServiceManager;
import android.os.SystemClock;

import com.gesher.app.shell.AppShell;
import static com.gesher.app.locale.UiText.text;
import com.gesher.app.window.WindowMode;
import com.gesher.net.ProxySetting;
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
 *     launch APK|PACKAGE [--activity CLASS] [--ez KEY true|false]...
 *            [--proxy host:port|direct|windows]
 *            [--window | --hidden-window | --headless [--keep-task]] [--detach | --wait]
 *                       install or update the APK, start its launcher activity through the
 *                       system server (am start -W) and show the app's output until the
 *                       app's process ends (--wait, the default). --detach: return once the
 *                       activity has started (its first frame drawn, idle), exit code 0, and
 *                       leave the app running: what gesher-launch asks for
 *                       ($GESHER_LAUNCH_DETACH=1) when nobody reads the output (a Start menu
 *                       shortcut, a double-clicked .apk). Its windows are Win32 windows (--window, the
 *                       default, or $GESHER_APP_WINDOW): the app runs until they are
 *                       closed. --hidden-window: the same, never shown (tests read them
 *                       back). --headless: no windows; once the activity is idle its task
 *                       is removed (with --keep-task it runs until it finishes).
 *                       --proxy: the app's HTTP proxy for this launch (its next process)
 *     proxy [[PACKAGE] host:port|direct|windows]
 *                       Gesher's own proxy, over Windows' settings: for every app, or for
 *                       one package (windows: Windows' settings for it); no argument
 *                       shows them. App processes started from now on use it
 *     install APK       install or update the package
 *     view PACKAGE URL  (run by an app's URL protocol, docs/installer.md) start the package's
 *                       activity for URL (ACTION_VIEW, BROWSABLE) in its task: an OAuth
 *                       redirect the Windows browser opened comes back to the app
 *     uninstall PACKAGE
 *     list              the installed packages (IPackageManager.getInstalledPackages)
 *     status            the device: its processes
 *     start             start the device (if it is not running)
 *     shutdown          stop the device: its app processes, the system server,
 *                       servicemanager and binderd
 *     toast-activated   (run by COM) a click on an app's toast, given to the device
 *     apk-icon APK      (run by the .apk icon handler, gesher_shellext.dll) draw the APK's
 *                       app icon for Explorer without installing it (AppShell.apkIcon)
 * </pre>
 *
 * Every command but shutdown, status and apk-icon starts the device if it does not run: binderd,
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
    /**
     * Set by gesher-launch (compat/shell/gesher_launch.cc) when it has no output handles to
     * pass on: {@code launch} returns once the activity has started (as {@code --detach}).
     */
    static final String DETACH_ENV = "GESHER_LAUNCH_DETACH";
    /** {@link #DETACH_ENV} as this process got it (it is not handed on to the device). */
    private static boolean sDetachRequested;

    private Launcher() {
    }

    private static void log(String message) {
        Main.log(message);
    }

    public static void main(String[] args) {
        int code;
        try {
            // The trusted host launcher is the shell, not an Android application.
            android.system.Os.setenv("GESHER_BINDER_UID", "2000", true);
            // gesher-launch's request is this launcher's only: a device this launcher
            // starts must not inherit it.
            sDetachRequested = "1".equals(System.getenv(DETACH_ENV));
            if (sDetachRequested) android.system.Os.unsetenv(DETACH_ENV);
            code = run(args);
        } catch (Throwable t) {
            System.out.flush();
            System.err.println(TAG + ": " + text("failed", t));
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
                // Its Start menu shortcut and URL protocols, recorded in its data directory,
                // which goes too.
                Properties shortcut = AppShell.shortcutOf(rest.get(0));
                Properties protocols = AppShell.urlProtocolsOf(rest.get(0));
                boolean removed = gesher().uninstall(rest.get(0));
                log(text(removed ? "uninstalled" : "not_installed", rest.get(0)));
                if (removed) {
                    AppShell.unregisterQuietly(rest.get(0), shortcut);
                    AppShell.unregisterUrlProtocolsQuietly(rest.get(0), protocols);
                }
                return removed ? 0 : 1;
            }
            case "list": return list();
            case "proxy": return proxy(rest);
            case "view": {
                if (rest.size() != 2) return usage();
                return view(rest.get(0), rest.get(1));
            }
            case "start": {
                ensureDevice(true);
                log(gesher().status());
                return 0;
            }
            case "status": {
                if (ensureDevice(false) == null) {
                    log(text("no_device"));
                    return 0;
                }
                log(gesher().status());
                return 0;
            }
            case "sync-shortcuts": return syncShortcuts();
            case "shutdown": return shutdown();
            case "toast-activated": return toastActivated();
            case "apk-icon": {
                // Not on the device: the APK is read here, not installed.
                if (rest.size() != 1) return usage();
                return AppShell.apkIcon(new File(rest.get(0)));
            }
            default: return usage();
        }
    }

    /**
     * {@code toast-activated [-Embedding]}: what COM runs for Gesher's toast activator when
     * no Gesher process serves it (the installer's LocalServer32; hwsvc2, ADR 0051): a click
     * on an app's toast after the device stopped. Serves the activator for that one
     * activation, then starts the device (if it does not run) and hands it the click, which
     * sends the notification's content (or action) intent.
     */
    private static int toastActivated() throws Exception {
        int hr = com.gesher.server.WindowsHost.registerToastActivator(true);
        if (hr != 0) {
            log(text("toast_not_registered", String.format("0x%08x", hr)));
            return 1;
        }
        log(text("toast_serving", com.gesher.server.WindowsHost.TOAST_ACTIVATOR_CLSID));
        String[] a = com.gesher.server.WindowsHost.waitActivation(60_000);
        com.gesher.server.WindowsHost.revokeToastActivator();
        if (a == null) {
            log(text("toast_timeout"));
            return 1;
        }
        log(text("toast_click", a[0], a[3]));
        ensureDevice(true);
        gesher().toastActivated(a[0], a[3]);
        log(text("toast_sent"));
        return 0;
    }

    /**
     * {@code proxy [[PACKAGE] host:port|direct|windows]}: Gesher's own proxy for every app,
     * or for one package, over Windows' settings (com.gesher.net.ProxySetting); no
     * argument shows the saved settings. App processes started from now on use it.
     */
    private static int proxy(List<String> args) throws Exception {
        if (args.size() > 2) return usage();
        if (args.isEmpty()) {
            Properties p = ProxySetting.load();
            if (p.isEmpty()) log(text("proxy_windows"));
            for (String key : new java.util.TreeSet<>(p.stringPropertyNames())) {
                log((key.equals("proxy") ? text("every_app") : key.substring(6)) + ": "
                        + p.getProperty(key));
            }
            return 0;
        }
        String pkg = args.size() == 2 ? args.get(0) : null;
        String value = ProxySetting.parse(args.get(args.size() - 1));
        if (value == null) {
            log(text("proxy_invalid", args.get(args.size() - 1)));
            return usage();
        }
        ProxySetting.save(pkg, value);
        log(text("proxy_saved", pkg == null ? text("every_app") : pkg, value));
        return 0;
    }

    private static int usage() {
        System.err.println(text("usage") + "gesher-app launch APK|PACKAGE [--activity CLASS] [--ez KEY"
                + " true|false]... [--proxy host:port|direct|windows]"
                + " [--window | --hidden-window | --headless [--keep-task]] [--detach | --wait]"
                + " | proxy [[PACKAGE] host:port|direct|windows] | install APK | uninstall PACKAGE | view PACKAGE URL | list | status | start"
                + " | shutdown | apk-icon APK");
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
        if (lock == 0) throw new IllegalStateException(text("cannot_lock", Device.lockName()));
        try {
            Properties state = Device.read(Device.stateFile());
            if (state != null) {
                String name = state.getProperty(Device.STATE_DEVICE);
                if (Device.alive(state, Device.SYSTEM_SERVER) && waitReady(state)) {
                    use(state);
                    log(text("connected", name, Device.pid(state, Device.SYSTEM_SERVER)));
                    return state;
                }
                // Crashed or killed, or never got ready: what is left of it goes.
                log(text("cleaning", name));
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
        log(text("device_starting", name, logs));
        Properties state = new Properties();
        state.setProperty(Device.STATE_DEVICE, name);
        state.setProperty(Device.STATE_SHUTDOWN_EVENT, Device.shutdownEventName());
        state.setProperty("started", String.valueOf(System.currentTimeMillis()));
        int binderd = Zygote.spawn(Zygote.quote(dir + "binderd.exe") + " " + name, env,
                new File(logs, "binderd.log").getPath(), true);
        record(state, Device.BINDERD, binderd);
        if (!Zygote.waitForBinder(name, binderd, 30_000)) {
            int code = Zygote.exitCode(binderd);
            Zygote.kill(binderd);
            printTail(new File(logs, "binderd.log"), 40);
            throw new IllegalStateException(text("binder_handshake", name, code));
        }
        log(text("binder_ready", name, binderd));
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
            throw new IllegalStateException(text("server_not_ready", name, new File(logs, "system_server.log")));
        }
        log(text("device_started", name, binderd, servicemanager, server));
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
        throw new IllegalStateException(text("no_service"));
    }

    private static int shutdown() throws Exception {
        long lock = Zygote.lock(Device.lockName(), (int) (2 * READY_TIMEOUT_MS));
        try {
            Properties state = Device.read(Device.stateFile());
            if (state == null) {
                log(text("no_device"));
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
            log(text("device_stopped", name));
            return 0;
        } finally {
            Zygote.unlock(lock);
        }
    }

    // --- packages ----------------------------------------------------------------------

    private static String install(GesherService.Client gesher, String apk) throws Exception {
        File f = new File(apk);
        if (!f.isFile()) throw new IllegalArgumentException(text("no_file", apk));
        String[] r = gesher.install(f.getAbsolutePath());
        switch (r[1]) {
            case "unchanged":
                log(text("unchanged", r[0], r[2]));
                break;
            default:
                log(text("updated".equals(r[1]) ? "updated" : "installed", r[0], r[3], r[2]));
        }
        // Its Start menu shortcut and URL protocols (docs/installer.md), made or updated.
        AppShell.registerQuietly(r[0], r[2], !"unchanged".equals(r[1]));
        AppShell.registerUrlProtocolsQuietly(r[0], r[2], !"unchanged".equals(r[1]));
        // The APK file shows the app's icon in Explorer (gesher_shellext.dll).
        AppShell.rememberApkQuietly(r[0], f);
        return r[0];
    }

    private static int list() throws Exception {
        ensureDevice(true);
        gesher();
        IPackageManager pm = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package"));
        @SuppressWarnings("unchecked")
        List<PackageInfo> packages = pm.getInstalledPackages(0, 0).getList();
        log(text(packages.size() == 1 ? "package_count_one" : "package_count", packages.size()));
        for (PackageInfo p : packages) {
            // getPackageInfo for each, as `pm dump` asks.
            PackageInfo full = pm.getPackageInfo(p.packageName, PackageManager.GET_ACTIVITIES
                    | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS, 0);
            System.out.println(text("package_details", p.packageName, p.versionName,
                    p.getLongVersionCode(), p.applicationInfo.sourceDir, count(full.activities),
                    count(full.services), count(full.providers)));
        }
        return 0;
    }

    private static int syncShortcuts() throws Exception {
        ensureDevice(true);
        gesher();
        IPackageManager pm = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package"));
        @SuppressWarnings("unchecked")
        List<PackageInfo> packages = pm.getInstalledPackages(0, 0).getList();
        int count = 0;
        for (PackageInfo p : packages) {
            if (p.applicationInfo != null && p.applicationInfo.sourceDir != null) {
                String codePath = new File(p.applicationInfo.sourceDir).getParent();
                String targetPath = codePath != null ? codePath : p.applicationInfo.sourceDir;
                AppShell.registerQuietly(p.packageName, targetPath, false);
                AppShell.registerUrlProtocolsQuietly(p.packageName, targetPath, false);
                count++;
            }
        }
        log("processed " + count + " packages for shortcut synchronization");
        return 0;
    }

    private static int count(Object[] a) {
        return a == null ? 0 : a.length;
    }

    // --- launching ---------------------------------------------------------------------

    /**
     * {@code view PACKAGE URL}: what an app's URL protocol runs (AppShell), as a browser on
     * Android starts the activity of a BROWSABLE link: ACTION_VIEW of {@code url} in the
     * package. It goes into the package's task when it has one (ActivityOptions'
     * launch task), so that an OAuth redirect reaches the activity that waits for it (AppAuth's
     * RedirectUriReceiverActivity hands it down with CLEAR_TOP); a package that does not run
     * is started in windows, as a launch. Returns once the activity is started.
     */
    private static int view(String pkg, String url) throws Exception {
        ensureDevice(true);
        GesherService.Client gesher = gesher();
        IPackageManager pm = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package"));
        if (pm.getPackageInfo(pkg, 0L, 0) == null) {
            log(text("not_installed_url", pkg, url));
            return 1;
        }
        Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url))
                .addCategory(Intent.CATEGORY_BROWSABLE)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .setPackage(pkg);
        IActivityTaskManager atm = ActivityTaskManager.getService();
        ActivityManager.RunningTaskInfo task = GesherAppProcess.taskOf(atm, pkg);
        android.os.Bundle options = null;
        if (task != null) {
            ActivityOptions o = ActivityOptions.makeBasic();
            o.setLaunchTaskId(task.taskId);
            options = o.toBundle();
        } else if (!running(ActivityManager.getService(), pkg)) {
            gesher.setWindowMode(pkg, WindowMode.fromEnvironment(WindowMode.WINDOW));
        }
        log(text("start_intent", intent.toShortString(false, true, true, false)
                + (task != null ? text("in_task", task.taskId) : "")));
        WaitResult result = atm.startActivityAndWait(null /* caller: the launcher */,
                "com.gesher.launcher", null, intent, null, null, null, 0, 0, null, options, 0);
        log(text("start_result", result.result,
                result.who != null ? result.who.flattenToShortString() : text("no_new_activity"),
                result.timeout ? text("timed_out") : ""));
        // START_SUCCESS, or a delivery to a running activity (START_DELIVERED_TO_TOP).
        return result.result >= ActivityManager.START_SUCCESS ? 0 : 1;
    }

    private static int launch(List<String> args) throws Exception {
        String apk = null;
        String activity = null;
        String shortcutId = null;
        boolean keepTask = false;
        boolean detach = sDetachRequested;
        String proxy = null;
        String windowMode = WindowMode.fromEnvironment(WindowMode.WINDOW);
        Map<String, Boolean> extras = new LinkedHashMap<>();
        for (int i = 0; i < args.size(); i++) {
            String a = args.get(i);
            switch (a) {
                case "--activity": activity = args.get(++i); break;
                case "--shortcut": shortcutId = args.get(++i); break;
                case "--keep-task": keepTask = true; break;
                case "--detach": detach = true; break;
                case "--wait": detach = false; break;
                case "--proxy": {
                    proxy = ProxySetting.parse(args.get(++i));
                    if (proxy == null) {
                        log(text("proxy_invalid", "--proxy " + args.get(i)));
                        return usage();
                    }
                    break;
                }
                case "--window": windowMode = WindowMode.WINDOW; break;
                case "--hidden-window": windowMode = WindowMode.HIDDEN; break;
                case "--headless": windowMode = WindowMode.HEADLESS; break;
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
        if (shortcutId != null && (activity != null || !extras.isEmpty())) return usage();
        Properties state = ensureDevice(true);
        GesherService.Client gesher = gesher();
        // An APK is installed or updated first; a package name (a Start menu shortcut's
        // `launch <package>`) must be installed.
        String pkg;
        if (new File(apk).isFile()) {
            pkg = install(gesher, apk);
        } else {
            pkg = apk;
            IPackageManager pm = IPackageManager.Stub.asInterface(
                    ServiceManager.getService("package"));
            PackageInfo pinfo = pm.getPackageInfo(pkg, 0L, 0);
            if (pinfo == null) {
                log(text("not_installed_hint", pkg));
                return 1;
            }
            if (pinfo.applicationInfo != null && pinfo.applicationInfo.sourceDir != null) {
                String codePath = new File(pinfo.applicationInfo.sourceDir).getParent();
                String targetPath = codePath != null ? codePath : pinfo.applicationInfo.sourceDir;
                AppShell.registerQuietly(pkg, targetPath, false);
                AppShell.registerUrlProtocolsQuietly(pkg, targetPath, false);
            }
            log(text("launching", pkg));
        }

        Intent intent = null;
        if (shortcutId != null) {
            IPackageManager shortcutPm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
            PackageInfo publisher = shortcutPm.getPackageInfo(pkg, 0L, 0);
            String identity = com.gesher.server.ApkPackage.parse(publisher.applicationInfo.sourceDir).codeIdentity();
            intent = AppShell.dynamicShortcutIntent(pkg, shortcutId, identity);
        }
        if (intent == null) {
            intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            if (activity == null) {
                intent.setPackage(pkg);
            } else {
                String cls = activity.startsWith(".") ? pkg + activity
                        : activity.indexOf('.') < 0 ? pkg + "." + activity : activity;
                intent.setComponent(new ComponentName(pkg, cls));
            }
        } else {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        }
        for (Map.Entry<String, Boolean> e : extras.entrySet()) {
            intent.putExtra(e.getKey(), (boolean) e.getValue());
        }

        // Its windows: the system server starts its process with them (ADR 0029). With
        // windows, the app runs until the user closes them (the task stays).
        gesher.setWindowMode(pkg, windowMode);
        if (!WindowMode.HEADLESS.equals(windowMode)) keepTask = true;
        log(text("windows", pkg, windowMode));
        // Its proxy: this launch's, else the saved settings (gesher-app proxy).
        gesher.setProxy(pkg, proxy);
        if (proxy != null) {
            log(text("proxy_launch", pkg, proxy, running(ActivityManager.getService(), pkg)
                    ? text("next_process") : ""));
        }

        // The app's output: its process appends to the package's log (not copied when
        // nobody reads it: detached).
        Tail tail = new Tail(Device.packageLog(pkg));
        if (!detach) tail.start();
        try {
            if (shortcutId == null) log(text("start_intent", intent.toShortString(false, true, true, false)));
            IActivityTaskManager atm = ActivityTaskManager.getService();
            GesherService.LaunchResult launch = shortcutId == null ? gesher.startActivityAndWait(intent)
                    : gesher.startShortcutAndWait(pkg, shortcutId);
            WaitResult result = launch.result;
            log(text("start_result", result.result,
                    result.who != null ? result.who.flattenToShortString() : text("no_activity"),
                    result.timeout ? text("timed_out") : "") + ", " + result.totalTime + " ms");
            if (!ActivityManager.isStartResultSuccessful(result.result)
                    || (detach && result.timeout)) {
                throw new IllegalStateException(text("start_failed", intent));
            }
            if (launch.exitReceipt == null || launch.taskId < 0) {
                throw new IllegalStateException(text("launch_not_identified"));
            }
            if (!keepTask) {
                log(text("remove_task", launch.taskId,
                        result.who != null ? result.who.flattenToShortString() : pkg));
                atm.removeTask(launch.taskId);
            }
            if (detach) {
                // The activity is up (startActivityAndWait waited until it is idle: it
                // has drawn its first frame). The app's process belongs to the device
                // (its system server started it and follows it), not to this launcher:
                // nothing here keeps it, and the device's idle timeout counts app
                // processes only.
                log(text("detached", pkg, Device.packageLog(pkg)));
                return 0;
            }
            // Follow only this resolved launch's retained process generation. Another
            // package sibling or a newer same-name process must not supply its result.
            GesherService.LaunchExit terminal = null;
            int consecutiveFailures = 0;
            while (true) {
                try {
                    GesherService.LaunchExit status = gesher.launchExit(launch.exitReceipt);
                    consecutiveFailures = 0;
                    if (status.hasExitCode) {
                        terminal = status;
                        break;
                    }
                    if (status.state == GesherService.EXIT_UNAVAILABLE) break;
                } catch (Throwable t) {
                    consecutiveFailures++;
                    if (state != null && !Device.alive(state, Device.SYSTEM_SERVER)) {
                        log(text("server_stopped"));
                        break;
                    }
                    if (consecutiveFailures >= 5) {
                        log(text("server_lost", t.getMessage()));
                        break;
                    }
                    SystemClock.sleep(300);
                    continue;
                }
                SystemClock.sleep(300);
            }
            tail.finish();
            if (terminal == null) {
                if (state != null && !Device.alive(state, Device.SYSTEM_SERVER)) {
                    log(text("server_unexpected"));
                } else {
                    log(text("no_exit_code", pkg));
                }
                return 1;
            }
            int exit = terminal.exitCode;
            log(text("process_ended", pkg, exit));
            if (exit != 0) {
                log(text("process_failed", exit));
                return 1;
            }
            log(text("done"));
            return 0;
        } finally {
            tail.finish();
        }
    }

    private static boolean running(IActivityManager am, String pkg) throws Exception {
        if (am == null) return false;
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
            LogLines lines = new LogLines(System.out);
            while (true) {
                boolean last = mDone;
                long length = mFile.length();
                if (length < offset) {
                    offset = 0;  // a new log
                    lines.reset();
                }
                if (length > offset) {
                    try (InputStream in = new FileInputStream(mFile)) {
                        in.skip(offset);
                        for (int n; (n = in.read(buffer)) > 0; ) {
                            lines.write(buffer, 0, n);
                            offset += n;
                        }
                    } catch (IOException e) {
                        // try again
                    }
                }
                if (last) { lines.finish(); return; }
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
