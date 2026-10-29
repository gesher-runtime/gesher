/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.web3app;

import android.app.Activity;
import android.os.Bundle;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises the real Java/JNI session request entry point. */
public class SessionActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(this::exercise, "session-fixture").start();
    }

    private void exercise() {
        boolean passed = false;
        Object host = null;
        try {
            Class<?> cls = Class.forName("com.gesher.app.webview.WebView2Host");
            Class<?> client = Class.forName("com.gesher.app.webview.WebView2Host$Client");
            CountDownLatch ready = new CountDownLatch(1), result = new CountDownLatch(1);
            CountDownLatch reentrant = new CountDownLatch(1);
            AtomicReference<Object> hostRef = new AtomicReference<>();
            AtomicReference<Boolean> closeResult = new AtomicReference<>(false);
            AtomicInteger count = new AtomicInteger(), status = new AtomicInteger(-1);
            AtomicReference<String> payload = new AtomicReference<>("");
            Object callback = Proxy.newProxyInstance(client.getClassLoader(), new Class<?>[] {client},
                    (proxy, method, args) -> {
                        if (method.getName().equals("onEvent")) {
                            int type = (Integer) args[0];
                            if (type == 1) ready.countDown();
                            if (type == 30 && (Long) args[1] == 1L) {
                                count.incrementAndGet();
                                status.set((Integer) args[2]);
                                payload.set((String) args[3]);
                                result.countDown();
                            }
                            if (type == 30 && (Long) args[1] == 3L) {
                                try {
                                    Object current = hostRef.get();
                                    closeResult.set((Boolean) current.getClass().getMethod("tryDestroy")
                                            .invoke(current));
                                } catch (Throwable e) { Web3.log("session reentrant exception " + e); }
                                reentrant.countDown();
                            }
                        }
                        if (method.getReturnType() == boolean.class) return false;
                        if (method.getReturnType() == String.class) return "{}";
                        return null;
                    });
            host = cls.getMethod("create", client, double.class, int.class)
                    .invoke(null, callback, 1.0, -1);
            if (host == null) throw new Exception("no host");
            hostRef.set(host);
            if (!ready.await(20, TimeUnit.SECONDS)) throw new Exception("not ready");
            Method submit = cls.getMethod("submitSession", long.class, String.class,
                    String.class, String.class, int.class);
            int accepted = (Integer) submit.invoke(host, 1L, "", "Runtime.evaluate",
                    "{\"expression\":\"6*7\",\"returnByValue\":true}", 5000);
            boolean completed = result.await(10, TimeUnit.SECONDS);
            int duplicate = (Integer) submit.invoke(host, 1L, "", "Runtime.enable", "{}", 5000);
            int invalid = (Integer) submit.invoke(host, 2L, "", "Runtime.enable", "{}", 0);
            int third = (Integer) submit.invoke(host, 3L, "", "Runtime.enable", "{}", 5000);
            boolean closed = reentrant.await(10, TimeUnit.SECONDS) && closeResult.get();
            int afterClose = (Integer) submit.invoke(host, 4L, "", "Runtime.enable", "{}", 5000);
            passed = accepted == 0 && completed && status.get() == 0 && count.get() == 1
                    && payload.get().contains("\"value\":42") && duplicate == -3 && invalid == -2
                    && third == 0 && closed && afterClose == -1;
            Web3.log("session request accepted=" + accepted + " completed=" + completed
                    + " hresult=" + status.get() + " count=" + count.get()
                    + " duplicate=" + duplicate + " invalid=" + invalid
                    + " reentrant=" + closed + " afterClose=" + afterClose);
        } catch (Throwable e) {
            Web3.log("session exception " + e);
        } finally {
            if (host != null) {
                try { passed &= (Boolean) host.getClass().getMethod("tryDestroy").invoke(host); }
                catch (Throwable e) { passed = false; Web3.log("session destroy exception " + e); }
            }
            Web3.log("session checks " + passed);
            Web3.log("session done");
            runOnUiThread(this::finish);
        }
    }
}
