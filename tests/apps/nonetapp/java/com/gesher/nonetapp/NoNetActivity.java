/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.nonetapp;

import android.app.Activity;
import android.net.ConnectivityManager;
import android.os.Bundle;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * An app without INTERNET and ACCESS_NETWORK_STATE (docs/network.md): what a phone refuses
 * it, each on a "NoNetApp" line. Socket I/O off the main thread, as StrictMode wants it.
 */
public class NoNetActivity extends Activity {
    static void log(String s) {
        System.out.println("NoNetApp " + s);
        System.out.flush();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Thread io = new Thread(() -> {
            try {
                InetAddress.getByName("example.com");
                log("dns: resolved");
            } catch (Throwable t) {
                log("dns: " + t.getClass().getSimpleName() + ": " + t.getMessage());
            }
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), 9), 2000);
                log("socket: connected");
            } catch (Throwable t) {
                log("socket: " + t.getClass().getSimpleName() + ": " + t.getMessage());
            }
        }, "nonetapp-io");
        io.start();
        try {
            io.join(60_000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        try {
            log("connectivity: active network " + cm.getActiveNetwork());
        } catch (SecurityException e) {
            log("connectivity: SecurityException: " + e.getMessage());
        }
        log("done");
    }
}
