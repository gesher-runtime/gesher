/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.webkit.CookieManager;
import android.webkit.ValueCallback;
import java.lang.reflect.Constructor;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Public SDK call sites; Windows main requires a private, disposable WebView2 profile. */
public final class CookieCallbackContractTest {
    public static int checks;
    private interface Operation { void run(ValueCallback<Boolean> callback); }
    private static synchronized void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static CookieManager manager() throws Exception {
        Constructor<?> ctor = Class.forName("com.gesher.app.webview.GesherCookieManager")
                .getDeclaredConstructor();
        ctor.setAccessible(true);
        return (CookieManager) ctor.newInstance();
    }
    private static void await(CountDownLatch latch, String message) throws Exception {
        check(latch.await(15, TimeUnit.SECONDS), message);
    }
    private static void callback(Handler handler, Operation operation, Boolean expected)
            throws Exception {
        CountDownLatch invoked = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicBoolean insideCall = new AtomicBoolean();
        LinkedBlockingQueue<Boolean> values = new LinkedBlockingQueue<>();
        check(handler.post(() -> {
            insideCall.set(true);
            try {
                operation.run(value -> {
                    try {
                        check(!insideCall.get(), "callback ran inline");
                        check(Looper.myLooper() == handler.getLooper(), "wrong callback Looper");
                        check(Thread.currentThread() == handler.getLooper().getThread(), "wrong callback thread");
                        if (expected != null) check(expected.equals(value), "wrong callback value");
                    } catch (Throwable t) { failure.compareAndSet(null, t); }
                    values.add(value);
                });
            } catch (Throwable t) { failure.compareAndSet(null, t); }
            finally { insideCall.set(false); invoked.countDown(); }
        }), "caller Handler rejected operation");
        await(invoked, "caller never invoked operation");
        if (failure.get() != null) throw new AssertionError("operation failed", failure.get());
        check(values.poll(15, TimeUnit.SECONDS) != null, "callback missing");
        if (failure.get() != null) throw new AssertionError("callback failed", failure.get());
        check(values.poll(40, TimeUnit.MILLISECONDS) == null, "callback delivered twice");
    }
    public static void contracts(boolean backendAvailable) throws Exception {
        CookieManager manager = manager();
        HandlerThread first = new HandlerThread("cookie-caller-one");
        HandlerThread second = new HandlerThread("cookie-caller-two");
        first.start(); second.start();
        try {
            for (HandlerThread thread : new HandlerThread[]{first, second}) {
                Handler handler = new Handler(thread.getLooper());
                callback(handler, cb -> manager.setCookie("https://cookie-contract.invalid/",
                        "session=value; Path=/", cb), backendAvailable);
                callback(handler, cb -> manager.setCookie(null, "name=value", cb), false);
                callback(handler, cb -> manager.setCookie("https://cookie-contract.invalid/",
                        "not-a-cookie", cb), false);
                callback(handler, manager::removeSessionCookies, backendAvailable ? null : false);
                callback(handler, manager::removeAllCookies, backendAvailable ? null : false);
            }
        } finally {
            first.quitSafely(); second.quitSafely();
            first.join(5000); second.join(5000);
            check(!first.isAlive() && !second.isAlive(), "caller threads did not terminate");
        }
        check(Looper.myLooper() == null, "no-Looper contract needs a plain caller");
        Operation[] operations = {
            cb -> manager.setCookie("https://cookie-contract.invalid/", "plain=value", cb),
            cb -> manager.setCookie(null, "invalid", cb),
            manager::removeSessionCookies, manager::removeAllCookies
        };
        for (Operation operation : operations) {
            boolean rejected = false;
            try { operation.run(value -> { throw new AssertionError("rejected callback ran"); }); }
            catch (IllegalStateException expected) { rejected = true; }
            check(rejected, "non-null callback accepted without a Looper");
            operation.run(null);
            checks++;
        }
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 1 || args[0].isEmpty()) {
            throw new IllegalArgumentException("Pass a private disposable WebView2 profile path");
        }
        System.loadLibrary("android_runtime");
        Class<?> host = Class.forName("com.gesher.app.webview.WebView2Host");
        check(Boolean.TRUE.equals(host.getMethod("load").invoke(null)), "WebView2 bridge missing");
        host.getMethod("configure", String.class, String.class).invoke(null, args[0], "");
        contracts(true);
        System.out.println("CookieCallbackContractTest all passed: " + checks
                + " checks; real HandlerThreads and native WebView2 callbacks");
    }
}
