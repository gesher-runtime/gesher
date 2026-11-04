/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import com.gesher.server.Device;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;

/**
 * Gesher's own proxy, over Windows' settings (docs/network.md): an HTTP proxy
 * ({@code host:port}) or {@code direct} (no proxy), for every app or for one package,
 * saved in {@code <data>/system/proxy.properties} by {@code gesher-app proxy}; or for one
 * launch ({@code gesher-app launch PACKAGE --proxy ...}). {@code windows} is Windows'
 * settings again. The system server gives an app process its setting as
 * {@code $GESHER_PROXY} when it starts it, which {@link WindowsProxy} takes instead of
 * Windows' settings, and as the http(s)_proxy variables other HTTP clients read.
 */
public final class ProxySetting {
    private ProxySetting() {
    }

    public static final String ENV = "GESHER_PROXY";
    public static final String DIRECT = "direct";
    public static final String WINDOWS = "windows";
    /** The key of every app's setting; a package's is {@code proxy.<package>}. */
    private static final String ALL = "proxy";

    /** Settings of the next launch of a package (the system server's). */
    private static final Map<String, String> sLaunch = new HashMap<>();

    /**
     * A setting as it is saved: {@code direct}, {@code windows} or {@code host:port} (an
     * {@code http://} prefix and a trailing slash dropped); null if it is not one.
     */
    public static String parse(String value) {
        if (value == null) return null;
        String v = value.trim();
        String lower = v.toLowerCase(Locale.ROOT);
        if (lower.equals(DIRECT) || lower.equals("none") || lower.equals("off")) return DIRECT;
        if (lower.equals(WINDOWS) || lower.equals("default")) return WINDOWS;
        if (lower.startsWith("http://")) v = v.substring(7);
        if (v.endsWith("/")) v = v.substring(0, v.length() - 1);
        int colon = v.lastIndexOf(':');
        if (colon <= 0 || colon == v.length() - 1) return null;
        String host = v.substring(0, colon);
        if (!host.matches("[A-Za-z0-9.\\-]+|\\[[0-9A-Fa-f:.]+\\]")) return null;
        try {
            int port = Integer.parseInt(v.substring(colon + 1));
            if (port < 1 || port > 65535) return null;
        } catch (NumberFormatException e) {
            return null;
        }
        return v;
    }

    public static File file() {
        return new File(Device.dataDir(), "system/proxy.properties");
    }

    /** The saved settings ({@code proxy}, {@code proxy.<package>}). */
    public static Properties load() {
        Properties p = new Properties();
        File f = file();
        if (f.isFile()) {
            try (InputStream in = new FileInputStream(f)) {
                p.load(in);
            } catch (IOException e) {
                System.out.println("gesher-net: " + f + ": " + e);
            }
        }
        return p;
    }

    /**
     * Saves the setting of every app (packageName null) or of one package; {@code windows}
     * for every app removes it, for a package it is kept (Windows' settings for that
     * package over every app's setting), null removes the package's.
     */
    public static synchronized void save(String packageName, String value) throws IOException {
        Properties p = load();
        String key = packageName == null ? ALL : ALL + "." + packageName;
        if (value == null || (packageName == null && value.equals(WINDOWS))) {
            p.remove(key);
        } else {
            p.setProperty(key, value);
        }
        File f = file();
        f.getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(f)) {
            p.store(out, "Gesher's proxy: gesher-app proxy [PACKAGE] host:port|direct|windows");
        }
    }

    /** The setting of the package's processes the system server starts next (null: none). */
    public static void setForLaunch(String packageName, String value) {
        synchronized (sLaunch) {
            if (value == null) {
                sLaunch.remove(packageName);
            } else {
                sLaunch.put(packageName, value);
            }
        }
    }

    /**
     * The package's setting: its launch's, else its own saved one, else every app's;
     * null for Windows' settings.
     */
    public static String forPackage(String packageName) {
        String v;
        synchronized (sLaunch) {
            v = sLaunch.get(packageName);
        }
        if (v == null) {
            Properties p = load();
            v = parse(p.getProperty(ALL + "." + packageName));
            if (v == null) v = parse(p.getProperty(ALL));
        }
        return v == null || v.equals(WINDOWS) ? null : v;
    }

    /**
     * The package's process environment for its setting (none for Windows' settings):
     * {@code GESHER_PROXY}, and the variables HTTP clients outside Android's proxy read
     * (Dart's HttpClient, curl, Go): {@code HTTP_PROXY}/{@code HTTPS_PROXY}, empty for
     * {@code direct}, with this machine's names in {@code NO_PROXY}. Windows' names do not
     * differ by case: these replace the device's own, in either case.
     */
    public static java.util.List<String> environment(String packageName) {
        java.util.List<String> env = new java.util.ArrayList<>();
        String v = forPackage(packageName);
        if (v == null) return env;
        env.add(ENV + "=" + v);
        String url = v.equals(DIRECT) ? "" : "http://" + v;
        env.add("HTTP_PROXY=" + url);
        env.add("HTTPS_PROXY=" + url);
        env.add("NO_PROXY=localhost,127.0.0.1,::1");
        return env;
    }

    /** This process's setting ($GESHER_PROXY), or null for Windows' settings. */
    public static String ofProcess() {
        String v = parse(System.getenv(ENV));
        return v == null || v.equals(WINDOWS) ? null : v;
    }
}
