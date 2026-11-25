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
import java.net.URI;
import java.net.URISyntaxException;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;

/**
 * Gesher's own proxy, over Windows' settings (docs/network.md): an HTTP proxy
 * ({@code host:port}), SOCKS5 ({@code socks5://host:port}) or {@code direct} (no proxy),
 * for every app or for one package,
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
    private static final String[] SOCKS_PREFIXES = {"socks5://", "socks://", "socks5=", "socks="};
    /** The key of every app's setting; a package's is {@code proxy.<package>}. */
    private static final String ALL = "proxy";

    /** Settings of the next launch of a package (the system server's). */
    private static final Map<String, String> sLaunch = new HashMap<>();

    /**
     * A setting as it is saved: {@code direct}, {@code windows}, {@code host:port} (HTTP),
     * or {@code socks5://host:port}. SOCKS aliases are normalized, with port 1080 only
     * when omitted. An HTTP prefix and a trailing slash are dropped; null if invalid.
     */
    public static String parse(String value) {
        if (value == null) return null;
        String v = value.trim();
        String lower = v.toLowerCase(Locale.ROOT);
        if (lower.equals(DIRECT) || lower.equals("none") || lower.equals("off")) return DIRECT;
        if (lower.equals(WINDOWS) || lower.equals("default")) return WINDOWS;
        for (String prefix : SOCKS_PREFIXES) {
            if (lower.startsWith(prefix)) return parseSocks(v.substring(prefix.length()));
        }
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

    /** Parse only an explicit SOCKS endpoint; never resolve a host while reading settings. */
    private static String parseSocks(String value) {
        String v = value.endsWith("/") ? value.substring(0, value.length() - 1) : value;
        String host;
        String digits = null;
        if (v.startsWith("[")) {
            int close = v.indexOf(']');
            if (close < 0) return null;
            host = v.substring(0, close + 1);
            if (close + 1 < v.length()) {
                if (v.charAt(close + 1) != ':') return null;
                digits = v.substring(close + 2);
            }
            if (!host.matches("\\[[0-9A-Fa-f:.]+\\]")) return null;
            try {
                // URI validates IPv6 syntax without DNS, including embedded IPv4.
                if (new URI("socks5://" + host).getHost() == null) return null;
            } catch (URISyntaxException e) {
                return null;
            }
        } else {
            int colon = v.indexOf(':');
            host = colon < 0 ? v : v.substring(0, colon);
            if (colon >= 0) digits = v.substring(colon + 1);
            if (host.isEmpty() || host.length() > 253) return null;
            String name = host.endsWith(".") ? host.substring(0, host.length() - 1) : host;
            for (String label : name.split("\\.", -1)) {
                if (!label.matches("[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?")) return null;
            }
            if (name.matches("[0-9]+(?:\\.[0-9]+){3}")) {
                for (String octet : name.split("\\.")) {
                    try {
                        if (Integer.parseInt(octet) > 255) return null;
                    } catch (NumberFormatException e) {
                        return null;
                    }
                }
            }
        }
        int port = 1080;
        if (digits != null) {
            if (!digits.matches("[0-9]+")) return null;
            try {
                port = Integer.parseInt(digits);
            } catch (NumberFormatException e) {
                return null;
            }
            if (port < 1 || port > 65535) return null;
        }
        return "socks5://" + host + ":" + port;
    }

    public static File file() {
        return new File(Device.dataDir(), "system/proxy.properties");
    }

    /**
     * The saved settings ({@code proxy}, {@code proxy.<package>}); none (Windows' settings)
     * when the file cannot be read or is not in Properties form (a backslash-u escape
     * without four hex digits), as the control panel shows it.
     */
    public static Properties load() {
        try {
            return read();
        } catch (IOException | IllegalArgumentException e) {
            System.out.println("gesher-net: " + file() + ": " + e);
            return new Properties();
        }
    }

    /** The saved settings, or the reason the file cannot be read. */
    private static Properties read() throws IOException {
        Properties p = new Properties();
        File f = file();
        if (f.isFile()) {
            try (InputStream in = new FileInputStream(f)) {
                p.load(in);
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
        // A file that cannot be read is not replaced: that would lose its other settings.
        Properties p;
        try {
            p = read();
        } catch (IllegalArgumentException e) {
            throw new IOException(file() + ": " + e.getMessage(), e);
        }
        String key = packageName == null ? ALL : ALL + "." + packageName;
        if (value == null || (packageName == null && value.equals(WINDOWS))) {
            p.remove(key);
        } else {
            p.setProperty(key, value);
        }
        File f = file();
        f.getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(f)) {
            p.store(out, "Gesher's proxy: gesher-app proxy [PACKAGE] host:port|socks5://host:port|direct|windows");
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
     * where supported: {@code HTTP_PROXY}/{@code HTTPS_PROXY}, and {@code ALL_PROXY} for
     * SOCKS-aware clients. The SOCKS URL keeps its scheme; support depends on the client.
     * Direct mode clears all three; HTTP mode clears a previously inherited ALL_PROXY.
     * This machine's names are in {@code NO_PROXY}. Windows' names do not
     * differ by case: these replace the device's own, in either case.
     */
    public static java.util.List<String> environment(String packageName) {
        java.util.List<String> env = new java.util.ArrayList<>();
        String v = forPackage(packageName);
        if (v == null) return env;
        env.add(ENV + "=" + v);
        boolean socks = v.startsWith("socks5://");
        String url = v.equals(DIRECT) ? "" : socks ? v : "http://" + v;
        env.add("HTTP_PROXY=" + url);
        env.add("HTTPS_PROXY=" + url);
        env.add("ALL_PROXY=" + (socks ? url : ""));
        env.add("NO_PROXY=localhost,127.0.0.1,::1");
        return env;
    }

    /** This process's setting ($GESHER_PROXY), or null for Windows' settings. */
    public static String ofProcess() {
        String v = parse(System.getenv(ENV));
        return v == null || v.equals(WINDOWS) ? null : v;
    }
}
