/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import com.gesher.server.Device;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;

/**
 * Whether an app's tasks share one desktop window (the default) or each task is a window of
 * its own (ADR 0088). The setting is per package, {@code separate.<package>=true} in
 * {@code <data>/system/app-windows.properties} ({@code gesher-app windows}, the Control
 * Center). The system server gives a process the setting as {@link #ENV} when it starts it
 * and groups that package's tasks by the same value, so a change applies from the app's
 * next start.
 */
public final class AppWindows {
    private AppWindows() {
    }

    /** {@code 1}: the process's tasks are windows of their own. */
    public static final String ENV = "GESHER_SEPARATE_WINDOWS";

    /**
     * IActivityTaskManager.moveTaskToFront flag of Gesher's own window activation (the user
     * clicked or focused a task's desktop window), outside Android's MOVE_TASK_* flags. Only
     * this request can be a late activation of a window its app's other task just covered;
     * an app's own moveTaskToFront is always carried out.
     */
    public static final int MOVE_TASK_FROM_WINDOW = 0x40000000;
    private static final String KEY = "separate.";

    /** The mode owned by each live or pending package process lifetime. */
    private static final Map<String, Boolean> sStarted = new HashMap<>();

    public static File file() {
        return new File(Device.dataDir(), "system/app-windows.properties");
    }

    private static Properties load() {
        Properties p = new Properties();
        File f = file();
        if (f.isFile()) {
            try (InputStream in = new FileInputStream(f)) {
                p.load(in);
            } catch (IOException | IllegalArgumentException e) {
                System.out.println("gesher-app: " + f + ": " + e);
            }
        }
        return p;
    }

    /** The saved setting: whether the package's tasks are windows of their own. */
    public static boolean saved(String packageName) {
        try {
            return Boolean.parseBoolean(load().getProperty(KEY + packageName, "false").trim());
        } catch (RuntimeException | LinkageError e) {
            return false;  // no device data (a host-only fixture): the default
        }
    }

    /** Saves the package's setting (false removes it: one window, the default). */
    public static synchronized void save(String packageName, boolean separate) throws IOException {
        Properties p = load();
        if (separate) {
            p.setProperty(KEY + packageName, "true");
        } else {
            p.remove(KEY + packageName);
        }
        File f = file();
        f.getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(f)) {
            p.store(out, "Gesher's app windows: gesher-app windows PACKAGE separate|shared");
        }
    }

    /** ATM records a choice while admitting the first process of a package lifetime. */
    public static void started(String packageName, boolean separate) {
        synchronized (sStarted) {
            sStarted.put(packageName, separate);
        }
    }

    /** Exact admitted process mode, including an explicit shared override of inherited env. */
    public static java.util.List<String> environment(boolean separate) {
        return java.util.List.of(ENV + (separate ? "=1" : "=0"));
    }

    /** ATM retires the choice after the last live/pending process record is removed. */
    public static void retired(String packageName) {
        synchronized (sStarted) {
            sStarted.remove(packageName);
        }
    }

    /**
     * The system server: whether the package's tasks are windows of their own, as its
     * processes were told (the saved setting before one started).
     */
    public static boolean separate(String packageName) {
        synchronized (sStarted) {
            Boolean v = sStarted.get(packageName);
            if (v != null) return v;
        }
        // An unstarted package has no mode owner. Do not retain a lookup forever or
        // let it replace the choice of the next admitted package lifetime.
        return saved(packageName);
    }

    /** This app process: whether its tasks are windows of their own ({@link #ENV}). */
    public static boolean separateInProcess() {
        return "1".equals(System.getenv(ENV));
    }
}
