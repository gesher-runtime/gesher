/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import android.net.Uri;
import android.os.IBinder;
import android.os.ServiceManager;

import com.android.net.IProxyService;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Windows' proxy settings as Android's default proxy (docs/network.md, ADR 0058). The
 * user's Internet Options (a proxy server and its bypass list, a PAC script's URL,
 * automatic detection: WinHTTP's view of them) become {@code ConnectivityManager.getDefaultProxy()}
 * and, through {@code android.net.Proxy.setHttpProxyConfiguration} (as ActivityThread
 * applies it in every app process), the {@code http(s).proxyHost/Port/nonProxyHosts}
 * properties that {@code Proxy.getDefaultHost()}, HttpURLConnection and OkHttp use. A PAC
 * script makes it a PAC proxy (automatic detection, WPAD, is not followed): the framework's PacProxySelector asks
 * {@code com.android.net.IProxyService} for each URL, which this class serves in every
 * process with Windows' own evaluation of the script (WinHttpGetProxyForUrl). A watcher
 * applies Windows' changes. The natives are compat/net/windows_proxy.cc
 * (libgesher_proxy.so).
 */
public final class WindowsProxy {
    private WindowsProxy() {
    }

    /** PacProxySelector.PROXY_SERVICE. */
    public static final String PROXY_SERVICE = "com.android.net.IProxyService";

    private static volatile boolean sLoaded;
    private static Boolean sAvailable;

    private static boolean load() {
        if (sLoaded) return true;
        synchronized (WindowsProxy.class) {
            if (sAvailable == null) {
                try {
                    System.loadLibrary("gesher_proxy");
                    sAvailable = true;
                } catch (UnsatisfiedLinkError e) {
                    sAvailable = false;
                }
                sLoaded = sAvailable;
            }
            return sAvailable;
        }
    }

    private static native String nativeConfig();

    private static native String nativeProxyForUrl(String url, String pac);

    private static native boolean nativeWaitForChange(int timeoutMs);

    /** The user's settings: auto, pac, proxy and bypass ("" when unset). */
    public static final class Config {
        public boolean autoDetect;
        public String pacUrl = "";
        public String proxy = "";
        public String bypass = "";

        @Override
        public String toString() {
            return "auto " + autoDetect + ", pac '" + pacUrl + "', proxy '" + proxy
                    + "', bypass '" + bypass + "'";
        }
    }

    /** Windows' settings now (an empty Config without the natives). */
    public static Config config() {
        Config c = new Config();
        if (!load()) return c;
        for (String line : nativeConfig().split("\n")) {
            String[] f = line.split("\t", 2);
            if (f.length < 2) continue;
            switch (f[0]) {
                case "auto": c.autoDetect = f[1].equals("1"); break;
                case "pac": c.pacUrl = f[1]; break;
                case "proxy": c.proxy = f[1]; break;
                case "bypass": c.bypass = f[1]; break;
                default: break;
            }
        }
        return c;
    }

    /**
     * Android's default proxy for Windows' settings: a PAC proxy for a PAC script, a direct proxy (host, port, exclusion list) for a proxy server,
     * else null (no proxy).
     */
    public static android.net.ProxyInfo defaultProxy() {
        return defaultProxy(config());
    }

    static android.net.ProxyInfo defaultProxy(Config c) {
        if (!c.pacUrl.isEmpty()) return android.net.ProxyInfo.buildPacProxy(Uri.parse(c.pacUrl));
        if (!c.proxy.isEmpty()) {
            String server = serverFor(c.proxy);
            int colon = server.lastIndexOf(':');
            if (server.isEmpty()) return null;
            String host = colon > 0 ? server.substring(0, colon) : server;
            int port = 80;
            try {
                if (colon > 0) port = Integer.parseInt(server.substring(colon + 1));
            } catch (NumberFormatException e) {
                port = 80;
            }
            return android.net.ProxyInfo.buildDirectProxy(host, port, exclusions(c.bypass));
        }
        // Automatic detection (WPAD) is on by default in Windows and finds nothing on most
        // networks: it is not taken as a PAC proxy (every request would look for one).
        return null;
    }

    /**
     * The HTTP proxy of a WinINet proxy string: "host:port", or per scheme
     * "http=host:port;https=host:port;..." (http's, else https's, else the first).
     */
    static String serverFor(String proxy) {
        String first = null;
        String https = null;
        for (String part : proxy.split("[;\\s]+")) {
            if (part.isEmpty()) continue;
            int eq = part.indexOf('=');
            String scheme = eq > 0 ? part.substring(0, eq).toLowerCase(java.util.Locale.ROOT) : "";
            String server = eq > 0 ? part.substring(eq + 1) : part;
            int sep = server.indexOf("://");
            if (sep >= 0) server = server.substring(sep + 3);
            if (server.endsWith("/")) server = server.substring(0, server.length() - 1);
            if (scheme.isEmpty() || scheme.equals("http")) return server;
            if (scheme.equals("https") && https == null) https = server;
            if (first == null && !scheme.equals("socks") && !scheme.equals("ftp")) first = server;
        }
        return https != null ? https : first != null ? first : "";
    }

    /**
     * The exclusion list of a WinINet bypass list: "<local>" (names without a dot) has no
     * Android equivalent and is left out; "*.example.com" stays as it is.
     */
    static List<String> exclusions(String bypass) {
        List<String> out = new ArrayList<>();
        for (String s : bypass.split("[;\\s,]+")) {
            String t = s.trim();
            if (t.isEmpty() || t.equalsIgnoreCase("<local>")) continue;
            out.add(t);
        }
        return out;
    }

    /** Windows' answer for a URL under the current PAC script: a PAC result. */
    public static String resolve(String url) {
        if (!load()) return null;
        Config c = config();
        if (c.pacUrl.isEmpty()) {
            android.net.ProxyInfo p = defaultProxy(c);
            return p != null && p.getHost() != null ? "PROXY " + p.getHost() + ":" + p.getPort()
                    : "DIRECT";
        }
        return nativeProxyForUrl(url, c.pacUrl);
    }

    // ---- In every process ----

    private static String sApplied;

    /**
     * Applies Windows' proxy to this process (Proxy.setHttpProxyConfiguration, as
     * ActivityThread does at bind), serves PacProxySelector's IProxyService here, and
     * applies Windows' later changes. Called where each process sets up its local services.
     */
    public static void installInProcess() {
        try {
            IBinder binder = new IProxyService.Stub() {
                @Override
                public String resolvePacFile(String host, String url) {
                    return resolve(url);
                }

                @Override
                public void setPacFile(String file) {
                }
            };
            Field f = ServiceManager.class.getDeclaredField("sCache");
            f.setAccessible(true);
            @SuppressWarnings("unchecked")
            Map<String, IBinder> cache = (Map<String, IBinder>) f.get(null);
            synchronized (cache) {
                cache.put(PROXY_SERVICE, binder);
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            System.out.println("gesher-net: no proxy service in this process: " + e);
        }
        if (!load()) return;
        apply();
        Thread watcher = new Thread(() -> {
            while (true) {
                if (nativeWaitForChange(-1)) apply();
            }
        }, "gesher-proxy-watch");
        watcher.setDaemon(true);
        watcher.start();
    }

    private static synchronized void apply() {
        Config c = config();
        String now = c.toString();
        if (now.equals(sApplied)) return;
        boolean first = sApplied == null;
        sApplied = now;
        android.net.ProxyInfo p = defaultProxy(c);
        try {
            android.net.Proxy.setHttpProxyConfiguration(p);
        } catch (RuntimeException | LinkageError e) {
            System.out.println("gesher-net: setHttpProxyConfiguration: " + e);
        }
        if (p != null || !first) {
            System.out.println("gesher-net: Windows' proxy (" + now + "): " + p);
        }
    }
}
