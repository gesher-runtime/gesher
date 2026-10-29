/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.web3app;

import android.app.Activity;
import android.os.Bundle;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises the real Java/JNI session request entry point. */
public class SessionActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(this::exercise, "session-fixture").start();
    }

    private boolean exerciseCapacity(Class<?> cls, Class<?> client) throws Exception {
        CountDownLatch ready = new CountDownLatch(1), completions = new CountDownLatch(128);
        AtomicIntegerArray counts = new AtomicIntegerArray(128);
        AtomicInteger unexpected = new AtomicInteger();
        Object callback = Proxy.newProxyInstance(client.getClassLoader(), new Class<?>[] {client},
                (proxy, method, args) -> {
                    if (method.getName().equals("onEvent")) {
                        int type = (Integer) args[0];
                        long id = (Long) args[1];
                        if (type == 1) ready.countDown();
                        if (type == 30 && id >= 1000 && id < 1128) {
                            int slot = (int) id - 1000;
                            if (counts.incrementAndGet(slot) != 1 || (Integer) args[2] != 0x800704c7)
                                unexpected.incrementAndGet();
                            completions.countDown();
                        }
                    }
                    if (method.getReturnType() == boolean.class) return false;
                    if (method.getReturnType() == String.class) return "{}";
                    return null;
                });
        Object host = cls.getMethod("create", client, double.class, int.class)
                .invoke(null, callback, 1.0, -1);
        if (host == null) return false;
        Method close = cls.getMethod("tryDestroy");
        try {
            if (!ready.await(20, TimeUnit.SECONDS)) return false;
            Method submit = cls.getMethod("submitSession", long.class, String.class,
                    String.class, String.class, int.class);
            String pending = "{\"expression\":\"new Promise(()=>{})\",\"awaitPromise\":true}";
            for (long id = 1000; id < 1128; ++id) {
                if ((Integer) submit.invoke(host, id, "", "Runtime.evaluate", pending, 60000) != 0)
                    return false;
            }
            int rejected = (Integer) submit.invoke(host, 1128L, "", "Runtime.enable", "{}", 60000);
            boolean queuedClose = (Boolean) close.invoke(host);
            boolean drained = completions.await(20, TimeUnit.SECONDS);
            Thread.sleep(1000); // bounded observation for duplicate events
            Web3.log("session capacity rejected=" + rejected + " closed=" + queuedClose
                    + " drained=" + drained + " unexpected=" + unexpected.get());
            return rejected == -5 && queuedClose && drained && unexpected.get() == 0;
        } finally {
            close.invoke(host);
        }
    }

    private boolean exerciseBudget(Class<?> cls, Class<?> client) throws Exception {
        CountDownLatch ready = new CountDownLatch(1), expired = new CountDownLatch(1);
        CountDownLatch cancelled = new CountDownLatch(7);
        AtomicInteger unexpected = new AtomicInteger();
        Object callback = Proxy.newProxyInstance(client.getClassLoader(), new Class<?>[] {client},
                (proxy, method, args) -> {
                    if (method.getName().equals("onEvent")) {
                        int type = (Integer) args[0];
                        long id = (Long) args[1];
                        if (type == 1) ready.countDown();
                        if (type == 30 && id == 2000) {
                            if ((Integer) args[2] != 0x800705b4) unexpected.incrementAndGet();
                            expired.countDown();
                        } else if (type == 30 && id >= 2001 && id <= 2007) {
                            if ((Integer) args[2] != 0x800704c7) unexpected.incrementAndGet();
                            cancelled.countDown();
                        }
                    }
                    if (method.getReturnType() == boolean.class) return false;
                    if (method.getReturnType() == String.class) return "{}";
                    return null;
                });
        Object host = cls.getMethod("create", client, double.class, int.class)
                .invoke(null, callback, 1.0, -1);
        if (host == null) return false;
        Method close = cls.getMethod("tryDestroy");
        try {
            if (!ready.await(20, TimeUnit.SECONDS)) return false;
            Method submit = cls.getMethod("submitSession", long.class, String.class,
                    String.class, String.class, int.class);
            String base = "{\"expression\":\"new Promise(()=>{})\",\"awaitPromise\":true}";
            StringBuilder params = new StringBuilder(base);
            while (params.length() < 524288) params.append(' ');
            String padded = params.toString();
            for (long id = 2000; id < 2007; ++id) {
                int duration = id == 2000 ? 15000 : 60000;
                if ((Integer) submit.invoke(host, id, "", "Runtime.evaluate", padded, duration) != 0)
                    return false;
            }
            int full = (Integer) submit.invoke(host, 2007L, "", "Runtime.evaluate", padded, 60000);
            boolean timedOut = expired.await(30, TimeUnit.SECONDS);
            if (full != -5 || !timedOut) {
                Web3.log("session budget full=" + full + " timedOut=" + timedOut);
                return false;
            }
            int afterExpiry = (Integer) submit.invoke(host, 2007L, "", "Runtime.evaluate", padded, 60000);
            boolean closed = (Boolean) close.invoke(host);
            boolean drained = cancelled.await(20, TimeUnit.SECONDS);
            Web3.log("session budget full=" + full + " timedOut=" + timedOut
                    + " afterExpiry=" + afterExpiry + " closed=" + closed
                    + " drained=" + drained + " unexpected=" + unexpected.get());
            return afterExpiry == 0 && closed && drained && unexpected.get() == 0;
        } finally {
            close.invoke(host);
        }
    }

    private void exercise() {
        boolean passed = false;
        Object host = null;
        try {
            Class<?> cls = Class.forName("com.gesher.app.webview.WebView2Host");
            Class<?> client = Class.forName("com.gesher.app.webview.WebView2Host$Client");
            CountDownLatch ready = new CountDownLatch(1), result = new CountDownLatch(1);
            CountDownLatch reentrant = new CountDownLatch(1), timeout = new CountDownLatch(1),
                    lateMarker = new CountDownLatch(1);
            AtomicReference<String> latePayload = new AtomicReference<>("");
            AtomicInteger lateStatus = new AtomicInteger(-1);
            AtomicInteger timeoutCount = new AtomicInteger(), timeoutStatus = new AtomicInteger();
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
                            if (type == 30 && (Long) args[1] == 2L) {
                                timeoutCount.incrementAndGet();
                                timeoutStatus.set((Integer) args[2]);
                                timeout.countDown();
                            }
                            if (type == 30 && (Long) args[1] == 6L) {
                                lateStatus.set((Integer) args[2]);
                                latePayload.set((String) args[3]);
                                lateMarker.countDown();
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
            int invalid = (Integer) submit.invoke(host, 5L, "", "Runtime.enable", "{}", 0);
            char[] oversized = new char[257];
            java.util.Arrays.fill(oversized, 'x');
            int tooLong = (Integer) submit.invoke(host, 7L, "", new String(oversized), "{}", 5000);
            int sessionTooLong = (Integer) submit.invoke(host, 8L, new String(oversized),
                    "Runtime.enable", "{}", 5000);
            char[] huge = new char[524289];
            java.util.Arrays.fill(huge, ' ');
            int paramsTooLong = (Integer) submit.invoke(host, 9L, "", "Runtime.enable",
                    new String(huge), 5000);
            int delayed = (Integer) submit.invoke(host, 2L, "", "Runtime.evaluate",
                    "{\"expression\":\"new Promise(r => setTimeout(() => {window.__sessionLate=7; r(7)}, 1000))\","
                            + "\"awaitPromise\":true,\"returnByValue\":true}", 100);
            boolean timedOut = timeout.await(5, TimeUnit.SECONDS);
            Thread.sleep(1500); // allow time for the page-side marker
            int marker = (Integer) submit.invoke(host, 6L, "", "Runtime.evaluate",
                    "{\"expression\":\"window.__sessionLate\",\"returnByValue\":true}", 5000);
            boolean markerObserved = lateMarker.await(5, TimeUnit.SECONDS)
                    && lateStatus.get() == 0 && latePayload.get().contains("\"value\":7");
            int third = (Integer) submit.invoke(host, 3L, "", "Runtime.enable", "{}", 5000);
            boolean closed = reentrant.await(10, TimeUnit.SECONDS) && closeResult.get();
            int afterClose = (Integer) submit.invoke(host, 4L, "", "Runtime.enable", "{}", 5000);
            passed = accepted == 0 && completed && status.get() == 0 && count.get() == 1
                    && payload.get().contains("\"value\":42") && duplicate == -3 && invalid == -2
                    && tooLong == -2 && sessionTooLong == -2 && paramsTooLong == -2
                    && delayed == 0 && timedOut && timeoutStatus.get() == 0x800705b4
                    && timeoutCount.get() == 1 && marker == 0 && markerObserved
                    && third == 0 && closed && afterClose == -1;
            boolean capacity = exerciseCapacity(cls, client);
            boolean budget = exerciseBudget(cls, client);
            passed &= capacity && budget;
            Web3.log("session capacity " + capacity + " budget " + budget);
            Web3.log("session request accepted=" + accepted + " completed=" + completed
                    + " hresult=" + status.get() + " count=" + count.get()
                    + " duplicate=" + duplicate + " invalid=" + invalid + " tooLong=" + tooLong
                    + " sessionTooLong=" + sessionTooLong + " paramsTooLong=" + paramsTooLong
                    + " timeout=" + timedOut + " timeoutHresult=" + timeoutStatus.get()
                    + " timeoutCount=" + timeoutCount.get() + " marker=" + markerObserved
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
