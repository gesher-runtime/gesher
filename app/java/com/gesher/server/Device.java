/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Properties;

/**
 * The Gesher device of a user: binderd, servicemanager and the system server, running for
 * every app launch until they are shut down or idle (docs/services.md, ADR 0026). Its
 * files are under {@code $ANDROID_DATA} ({@code %LOCALAPPDATA%\Gesher\data} for the
 * launcher):
 *
 * <ul>
 * <li>{@code system/gesher-device}: the running device (properties): its binder device
 *     name, the pid and creation time of binderd, servicemanager and the system server, and
 *     the name of the event that shuts it down. Written by the launcher that started it,
 *     removed by the system server when it stops; a file whose system server is gone is
 *     stale, and the next launcher cleans it up.
 * <li>{@code system/gesher-device.ready}: the system server's pid, once its services are
 *     registered.
 * <li>{@code system/packages.list}: the installed packages ({@link Installer}).
 * <li>{@code app/<package>-<n>/base.apk} (and {@code lib/x86_64}), {@code data/<package>}:
 *     an installed package's code and data.
 * </ul>
 *
 * Logs go to {@code $GESHER_LOGS} (default: {@code logs} next to {@code $ANDROID_DATA}):
 * {@code binderd.log}, {@code servicemanager.log}, {@code system_server.log} and one
 * {@code <package>.log} per package, appended to by each of its processes.
 */
public final class Device {
    private Device() {
    }

    public static final String STATE_DEVICE = "device";
    public static final String STATE_SHUTDOWN_EVENT = "shutdown_event";
    public static final String BINDERD = "binderd";
    public static final String SERVICEMANAGER = "servicemanager";
    public static final String SYSTEM_SERVER = "system_server";

    /** {@code $ANDROID_DATA}, as an absolute path. */
    public static String dataDir() {
        String d = System.getenv("ANDROID_DATA");
        return new File(d == null || d.isEmpty() ? "/data" : d).getAbsolutePath();
    }

    public static File stateFile() {
        return new File(dataDir(), "system/gesher-device");
    }

    public static File readyFile() {
        return new File(dataDir(), "system/gesher-device.ready");
    }

    public static File logsDir() {
        String d = System.getenv("GESHER_LOGS");
        if (d != null && !d.isEmpty()) return new File(d);
        return new File(new File(dataDir()).getParentFile(), "logs");
    }

    /** A package's log: every process of it appends to it. */
    public static File packageLog(String packageName) {
        return new File(logsDir(), packageName + ".log");
    }

    /** Names of this data directory's Windows objects (a mutex, an event). */
    private static String key() {
        return Integer.toHexString(dataDir().toLowerCase(java.util.Locale.ROOT).hashCode());
    }

    /** The mutex launchers hold while they find or start the device. */
    public static String lockName() {
        return "Local\\gesher-" + key() + "-device";
    }

    /** The event that tells the system server to shut the device down. */
    public static String shutdownEventName() {
        return "Local\\gesher-" + key() + "-shutdown";
    }

    /** The properties in {@code file}, or null if it does not exist or cannot be read. */
    public static Properties read(File file) {
        if (!file.isFile()) return null;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(file)) {
            p.load(in);
            return p;
        } catch (IOException e) {
            return null;
        }
    }

    /** Writes {@code p} to {@code file} through a temporary file, replacing it. */
    public static void write(File file, Properties p) throws IOException {
        file.getParentFile().mkdirs();
        File tmp = new File(file.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            p.store(out, "Gesher device");
        }
        if (!tmp.renameTo(file)) {
            file.delete();
            if (!tmp.renameTo(file)) throw new IOException("cannot write " + file);
        }
    }

    public static int pid(Properties state, String name) {
        try {
            return Integer.parseInt(state.getProperty(name, "0"));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    public static long startTime(Properties state, String name) {
        try {
            return Long.parseLong(state.getProperty(name + ".start", "0"));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** Whether the process {@code name} of the state still runs (the same process). */
    public static boolean alive(Properties state, String name) {
        int pid = pid(state, name);
        return pid > 0 && Zygote.alive(pid, startTime(state, name));
    }

    /** Ends what is left of a device that is not running as a whole any more. */
    public static void killLeftovers(Properties state) {
        for (String name : new String[] {SYSTEM_SERVER, SERVICEMANAGER, BINDERD}) {
            if (alive(state, name)) Zygote.kill(pid(state, name));
        }
    }
}
