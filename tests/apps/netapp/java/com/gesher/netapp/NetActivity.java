/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.netapp;

import android.app.Activity;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkRequest;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import javax.net.ssl.HttpsURLConnection;

/**
 * Networking the way apps use it (docs/network.md), each result on a "NetApp" line:
 * ConnectivityManager's active network (capabilities, link properties, NetworkInfo),
 * a default network callback and a request callback, an HTTPS GET through
 * HttpsURLConnection (the network security config's trust manager, the system CAs),
 * cleartext refused (target SDK 36). With the extra {@code watch}, it waits for the test to
 * add an address to the default network's adapter (192.0.2.77) and to remove it, and
 * reports the callbacks Windows' change brought.
 */
public class NetActivity extends Activity {
    static final String WATCHED = "192.0.2.77";

    static void log(String s) {
        System.out.println("NetApp " + s);
        System.out.flush();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            run(getIntent().getBooleanExtra("watch", false));
        } catch (Throwable t) {
            log("failed: " + t);
            t.printStackTrace(System.out);
            throw new RuntimeException(t);
        }
    }

    private void run(boolean watch) throws Exception {
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        Network active = cm.getActiveNetwork();
        NetworkCapabilities nc = cm.getNetworkCapabilities(active);
        LinkProperties lp = cm.getLinkProperties(active);
        NetworkInfo info = cm.getActiveNetworkInfo();
        log("active network: " + active + " " + nc);
        log("link properties: " + lp);
        log("network info: " + info);
        log("state: internet " + (nc != null && nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET))
                + ", validated " + (nc != null && nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED))
                + ", metered " + cm.isActiveNetworkMetered()
                + ", connected " + (info != null && info.isConnected())
                + ", type " + (info != null ? info.getTypeName() : null)
                + ", transport " + transport(nc)
                + ", interface " + (lp != null ? lp.getInterfaceName() : null)
                + ", addresses " + (lp != null ? lp.getLinkAddresses().size() : 0)
                + ", dns " + (lp != null ? lp.getDnsServers().size() : 0)
                + ", networks " + cm.getAllNetworks().length);

        HandlerThread thread = new HandlerThread("netapp-callbacks");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        CountDownLatch available = new CountDownLatch(1);
        AtomicReference<String> added = new AtomicReference<>();
        CountDownLatch addedSeen = new CountDownLatch(1);
        CountDownLatch removedSeen = new CountDownLatch(1);
        ConnectivityManager.NetworkCallback defaultCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                log("default callback: onAvailable " + network);
                available.countDown();
            }

            @Override
            public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                log("default callback: onCapabilitiesChanged " + network + " " + transport(caps));
            }

            @Override
            public void onLinkPropertiesChanged(Network network, LinkProperties props) {
                boolean has = false;
                for (LinkAddress a : props.getLinkAddresses()) {
                    has |= a.getAddress().getHostAddress().equals(WATCHED);
                }
                log("default callback: onLinkPropertiesChanged " + network + " "
                        + props.getInterfaceName() + ", " + props.getLinkAddresses().size()
                        + " addresses" + (has ? " (with " + WATCHED + ")" : ""));
                if (has && added.compareAndSet(null, network.toString())) {
                    addedSeen.countDown();
                } else if (!has && added.get() != null) {
                    removedSeen.countDown();
                }
            }

            @Override
            public void onLost(Network network) {
                log("default callback: onLost " + network);
            }
        };
        cm.registerDefaultNetworkCallback(defaultCallback, handler);
        log("default callback: available " + available.await(20, TimeUnit.SECONDS));

        CountDownLatch requested = new CountDownLatch(1);
        NetworkRequest request = new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build();
        ConnectivityManager.NetworkCallback requestCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                requested.countDown();
            }
        };
        cm.registerNetworkCallback(request, requestCallback, handler);
        log("request callback (INTERNET): available " + requested.await(20, TimeUnit.SECONDS));
        cm.unregisterNetworkCallback(requestCallback);

        // Network I/O off the main thread, as StrictMode wants it.
        AtomicReference<String> https = new AtomicReference<>();
        AtomicReference<String> cleartext = new AtomicReference<>();
        Thread io = new Thread(() -> {
            try {
                HttpsURLConnection c = (HttpsURLConnection) new URL("https://example.com/").openConnection();
                c.setConnectTimeout(20_000);
                c.setReadTimeout(30_000);
                String body = read(c);
                https.set(c.getResponseCode() + " " + (body.contains("Example Domain") ? "Example Domain" : "?")
                        + " " + c.getCipherSuite());
            } catch (Throwable t) {
                https.set("failed: " + t);
                t.printStackTrace(System.out);
            }
            try {
                HttpURLConnection c = (HttpURLConnection) new URL("http://example.com/").openConnection();
                c.setConnectTimeout(20_000);
                read(c);
                cleartext.set("allowed (" + c.getResponseCode() + ")");
            } catch (Throwable t) {
                cleartext.set("refused: " + t.getMessage());
            }
        }, "netapp-io");
        io.start();
        io.join(120_000);
        log("https: " + https.get());
        log("cleartext http: " + cleartext.get());

        if (watch) {
            log("watching: add " + WATCHED + " now");
            log("watch: added seen " + addedSeen.await(90, TimeUnit.SECONDS) + " on " + added.get());
            log("watch: removed seen " + removedSeen.await(90, TimeUnit.SECONDS));
        }
        cm.unregisterNetworkCallback(defaultCallback);
        thread.quitSafely();
        log("done");
    }

    static String transport(NetworkCapabilities nc) {
        if (nc == null) return "none";
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) return "vpn";
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return "wifi";
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) return "ethernet";
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) return "cellular";
        return "other";
    }

    static String read(HttpURLConnection c) throws Exception {
        try (InputStream in = c.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[8192];
            for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
            return out.toString("UTF-8");
        }
    }
}
