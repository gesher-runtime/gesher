/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkRequest;
import android.net.ProxyInfo;
import android.os.Bundle;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.InetAddress;
import java.net.Inet4Address;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.DatagramSocket;
import java.net.Proxy;
import java.io.FileDescriptor;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Networking leftovers (ADR 0058), on "Web3 net" lines: Windows' proxy settings (the test
 * sets them) as ConnectivityManager.getDefaultProxy, android.net.Proxy's default host and
 * port, and the proxy HttpURLConnection uses; the sticky CONNECTIVITY_ACTION a receiver
 * gets when it registers; a network request whose callback is a PendingIntent. With the
 * extra {@code pac}, the settings are a PAC script that proxies one host only.
 */
public class NetActivity extends Activity {
    static final String ACTION_NETWORK = "com.gesher.web3app.NETWORK";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        boolean pac = getIntent().getBooleanExtra("pac", false);
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        ProxyInfo p = cm.getDefaultProxy();
        Web3.log("net default proxy: " + p + " pac " + (p != null ? p.getPacFileUrl() : null));
        Web3.log("net Proxy.getDefaultHost " + android.net.Proxy.getDefaultHost() + " port "
                + android.net.Proxy.getDefaultPort() + ", http.proxyHost "
                + System.getProperty("http.proxyHost"));

        // CONNECTIVITY_ACTION: sticky, so registering returns the current one.
        CountDownLatch delivered = new CountDownLatch(1);
        Intent sticky = registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                NetworkInfo info = intent.getParcelableExtra(ConnectivityManager.EXTRA_NETWORK_INFO,
                        NetworkInfo.class);
                Web3.log("net CONNECTIVITY_ACTION received: " + (info != null ? info.getTypeName()
                        + " connected " + info.isConnected() : "no NetworkInfo"));
                delivered.countDown();
            }
        }, new IntentFilter(ConnectivityManager.CONNECTIVITY_ACTION), Context.RECEIVER_EXPORTED);
        Web3.log("net CONNECTIVITY_ACTION sticky: " + (sticky != null));

        // A request whose callback is a PendingIntent.
        CountDownLatch network = new CountDownLatch(1);
        AtomicReference<String> got = new AtomicReference<>();
        registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Network n = intent.getParcelableExtra(ConnectivityManager.EXTRA_NETWORK, Network.class);
                NetworkRequest r = intent.getParcelableExtra(ConnectivityManager.EXTRA_NETWORK_REQUEST,
                        NetworkRequest.class);
                got.set(n + " for request " + (r != null));
                network.countDown();
            }
        }, new IntentFilter(ACTION_NETWORK), Context.RECEIVER_EXPORTED);
        PendingIntent pi = PendingIntent.getBroadcast(this, 0,
                new Intent(ACTION_NETWORK).setPackage(getPackageName()),
                PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        cm.requestNetwork(new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build(), pi);

        Thread io = new Thread(() -> {
            try {
                Web3.log("net CONNECTIVITY_ACTION delivered " + delivered.await(20, TimeUnit.SECONDS));
                Web3.log("net PendingIntent request: " + (network.await(20, TimeUnit.SECONDS)
                        ? got.get() : "not sent"));
                cm.releaseNetworkRequest(pi);
                if (!pac) binding(cm);
                String host = pac ? "pac-proxied.test" : "web3-proxied.test";
                Web3.log("net http://" + host + "/net: " + get("http://" + host + "/net"));
                if (pac) {
                    Web3.log("net direct under PAC: " + get("http://127.0.0.1:" + Web3.port(1)
                            + "/direct"));
                }
            } catch (Throwable t) {
                Web3.log("net failed: " + t);
            }
            Web3.log("net done");
            runOnUiThread(this::finish);
        }, "web3-net");
        io.start();
    }

    private static int socketInterface(Socket socket) throws Exception {
        FileDescriptor fd = (FileDescriptor) Socket.class.getMethod("getFileDescriptor$").invoke(socket);
        return (Integer) Class.forName("com.gesher.net.WindowsNetworks")
                .getMethod("socketInterface", FileDescriptor.class).invoke(null, fd);
    }

    private static void require(boolean success, String message) {
        if (!success) throw new AssertionError(message);
    }

    private static void binding(ConnectivityManager cm) throws Exception {
        Network chosen = cm.getActiveNetwork();
        require(chosen != null, "no active network");
        int index = (int) (chosen.getNetworkHandle() >>> 32) - 100;
        InetAddress local = null;
        for (android.net.LinkAddress address : cm.getLinkProperties(chosen).getLinkAddresses()) {
            if (address.getAddress() instanceof Inet4Address) { local = address.getAddress(); break; }
        }
        require(local != null, "no IPv4 address on active network");
        InetAddress[] dns = chosen.getAllByName("example.com");
        require(dns.length > 0, "interface DNS returned no addresses");
        Web3.log("net per-network DNS resolved example.com true");
        int port = Web3.port(1);
        try (Socket socket = chosen.getSocketFactory().createSocket()) {
            require(socketInterface(socket) == index, "socket factory did not bind the kernel interface");
            socket.connect(new InetSocketAddress(local, port), 10_000);
            require(socket.getLocalAddress().equals(local), "bound socket used a different source address");
        }
        Web3.log("net bound SocketFactory kernel interface and source address true");
        try (Socket socket = new Socket(Proxy.NO_PROXY)) {
            chosen.bindSocket(socket);
            require(socketInterface(socket) == index, "explicit socket did not bind");
        }
        try (DatagramSocket socket = new DatagramSocket(null)) {
            chosen.bindSocket(socket);
            FileDescriptor fd = (FileDescriptor) DatagramSocket.class.getMethod("getFileDescriptor$").invoke(socket);
            int bound = (Integer) Class.forName("com.gesher.net.WindowsNetworks")
                    .getMethod("socketInterface", FileDescriptor.class).invoke(null, fd);
            require(bound == index, "datagram socket did not bind");
        }
        Web3.log("net explicit TCP and UDP socket binding true");
        HttpURLConnection connection = (HttpURLConnection) chosen.openConnection(
                new URL("http://" + local.getHostAddress() + ":" + port + "/network-binding"), Proxy.NO_PROXY);
        connection.setConnectTimeout(10_000);
        connection.setReadTimeout(10_000);
        try (InputStream input = connection.getInputStream()) {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            byte[] bytes = new byte[128];
            for (int count; (count = input.read(bytes)) > 0;) body.write(bytes, 0, count);
            require(body.toString("UTF-8").trim().equals(local.getHostAddress()),
                    "Network.openConnection reached the server from another interface");
        } finally { connection.disconnect(); }
        Web3.log("net Network.openConnection server observed the selected source address true");
        Network missing = Network.fromNetworkHandle((0x7fffffffL << 32) | 0xcafed00dL);
        require(cm.bindProcessToNetwork(chosen), "process network binding failed");
        try {
            require(chosen.equals(cm.getBoundNetworkForProcess()), "process binding getter disagrees");
            try (Socket socket = new Socket(Proxy.NO_PROXY)) {
                socket.getReuseAddress();
                require(socketInterface(socket) == index, "new socket did not inherit process binding");
                socket.connect(new InetSocketAddress(local, port), 10_000);
                require(socket.getLocalAddress().equals(local), "process-bound socket used another source");
            }
            require(InetAddress.getAllByName("example.com").length > 0, "process-bound DNS failed");
            require(!cm.bindProcessToNetwork(missing), "invalid network was accepted");
            require(chosen.equals(cm.getBoundNetworkForProcess()), "failed binding changed the process network");
        } finally { require(cm.bindProcessToNetwork(null), "process unbind failed"); }
        require(cm.getBoundNetworkForProcess() == null, "process remained bound");
        Web3.log("net process socket and DNS binding, invalid network preservation, unbind true");
        boolean rejected = false;
        try { missing.getAllByName("example.com"); }
        catch (java.net.UnknownHostException expected) { rejected = true; }
        require(rejected, "DNS on a missing network fell back to another interface");
        Web3.log("net missing-network DNS refused fallback true");
    }

    static String get(String url) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(15_000);
            c.setReadTimeout(20_000);
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] b = new byte[4096];
                for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
                return c.getResponseCode() + " " + out.toString("UTF-8").trim();
            }
        } catch (Exception e) {
            return "failed: " + e;
        }
    }
}
