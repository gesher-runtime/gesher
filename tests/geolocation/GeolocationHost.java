/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.webview;

import android.content.Context;
import android.content.ContextWrapper;
import android.location.ILocationManager;
import android.location.Location;
import android.location.LocationManager;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.HandlerThread;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

/** Executes the unchanged production shim with real Android Handler/MessageQueue.
 * Only the surrounding WebView and LocationManager backend are controlled seams. */
public final class GeolocationHost {
    private static int checks;
    private static void check(boolean value, String message) {
        ++checks;
        if (!value) throw new AssertionError(message);
    }

    static final class TestContext extends ContextWrapper {
        Backend backend;
        TestContext() { super(null); }
        @Override public String getSystemServiceName(Class<?> type) { return Context.LOCATION_SERVICE; }
        @Override public Object getSystemService(String name) { return backend; }
        @Override public Executor getMainExecutor() { return Runnable::run; }
    }

    static final class Backend extends LocationManager {
        Consumer<Location> callback;
        Executor executor;
        CancellationSignal signal;
        String mode = "async";
        int cancels;
        boolean callbackOnCancel;
        Backend(Context context) { super(context, new ILocationManager.Default()); }
        @Override public boolean isProviderEnabled(String name) { return true; }
        @Override public Location getLastKnownLocation(String name) { return null; }
        @Override public void getCurrentLocation(String name, CancellationSignal signal,
                Executor executor, Consumer<Location> callback) {
            this.callback = callback; this.executor = executor; this.signal = signal;
            signal.setOnCancelListener(() -> {
                ++cancels;
                if (callbackOnCancel) callback.accept(null); // deliberately reentrant backend
            });
            if (mode.equals("security")) throw new SecurityException("location denied");
            if (mode.equals("failure")) throw new IllegalStateException("backend failed");
            if (mode.equals("inline")) callback.accept(location());
        }
        void deliver(Location location) { executor.execute(() -> callback.accept(location)); }
    }

    static Location location() {
        Location location = new Location("gps");
        location.setLatitude(31); location.setLongitude(35); location.setTime(123);
        return location;
    }

    private static final class Harness implements AutoCloseable {
        final HandlerThread thread = new HandlerThread("geolocation-webview");
        final TestContext context = new TestContext();
        final Backend backend = new Backend(context);
        final GesherWebViewProvider provider;
        final GeolocationShim shim;
        Harness() {
            context.backend = backend;
            thread.start();
            provider = new GesherWebViewProvider(context, new Handler(thread.getLooper()));
            shim = new GeolocationShim(provider);
        }
        void ui(Runnable task) throws Exception {
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<Throwable> failed = new AtomicReference<>();
            if (!provider.uiHandler().post(() -> {
                try { task.run(); } catch (Throwable failure) { failed.set(failure); }
                finally { done.countDown(); }
            })) throw new AssertionError("UI post rejected");
            if (!done.await(5, TimeUnit.SECONDS)) throw new AssertionError("UI task timed out");
            if (failed.get() != null) throw new AssertionError("UI failure", failed.get());
        }
        void request(long timeout) {
            try {
                Method method = GeolocationShim.class.getDeclaredMethod("getCurrent", int.class,
                        boolean.class, long.class, long.class, int.class);
                method.setAccessible(true);
                Field generation = GeolocationShim.class.getDeclaredField("mGeneration");
                generation.setAccessible(true);
                method.invoke(shim, 1, false, timeout, 0L, generation.getInt(shim));
            } catch (InvocationTargetException failure) {
                if (failure.getCause() instanceof RuntimeException runtime) throw runtime;
                throw new AssertionError(failure.getCause());
            } catch (ReflectiveOperationException failure) { throw new AssertionError(failure); }
        }
        int pending() {
            try {
                Field gets = GeolocationShim.class.getDeclaredField("mGets"); gets.setAccessible(true);
                return ((List<?>) gets.get(shim)).size();
            } catch (ReflectiveOperationException failure) { throw new AssertionError(failure); }
        }
        boolean queued() { return provider.uiHandler().hasMessages(0); }
        void clean(String reason) {
            check(pending() == 0, reason + ": pending request retained");
            check(!queued(), reason + ": timeout retained in real MessageQueue");
        }
        @Override public void close() throws Exception {
            ui(shim::pageGone); thread.quit(); thread.join(5000);
            check(!thread.isAlive(), "UI thread leaked");
        }
    }

    private static void completion(String mode, boolean nullResult) throws Exception {
        try (Harness h = new Harness()) {
            h.backend.mode = mode;
            h.ui(() -> h.request(TimeUnit.DAYS.toMillis(30)));
            if (!mode.equals("inline")) {
                h.ui(() -> check(h.queued(), "pending request has no timeout"));
                // The real request executor must marshal this non-UI completion.
                h.backend.deliver(nullResult ? null : location());
            }
            h.ui(() -> {
                h.clean(mode + " completion");
                check(h.provider.host.messages.size() == 1, "missing/duplicate location result");
                check(h.backend.cancels == 0, "successful request canceled backend");
                h.backend.callback.accept(null);
                check(h.provider.host.messages.size() == 1, "duplicate backend result delivered");
            });
        }
    }

    private static void navigation() throws Exception {
        try (Harness h = new Harness()) {
            h.backend.callbackOnCancel = true;
            h.ui(() -> {
                for (int i = 0; i < 1000; ++i) h.request(TimeUnit.DAYS.toMillis(30));
                check(h.pending() == 1000, "load setup failed");
                h.shim.pageGone(); h.clean("navigation");
                check(h.backend.cancels == 1000, "navigation did not cancel every request");
                check(h.provider.host.messages.isEmpty(), "old document received cancellation result");
            });
            h.backend.deliver(location());
            h.ui(() -> check(h.provider.host.messages.isEmpty(), "late result reached new document"));
        }
    }

    private static void failure(String mode) throws Exception {
        try (Harness h = new Harness()) {
            h.backend.mode = mode;
            h.backend.callbackOnCancel = true;
            h.ui(() -> {
                boolean failed = false;
                try { h.request(TimeUnit.DAYS.toMillis(30)); }
                catch (IllegalStateException expected) { failed = true; }
                h.clean(mode);
                check(h.backend.cancels == 1, "failed request not canceled");
                check(failed == mode.equals("failure"), "unexpected exception mapping");
                check(h.provider.host.messages.size() == (failed ? 0 : 1), "unexpected failure result");
            });
        }
    }

    private static void noTimeout() throws Exception {
        try (Harness h = new Harness()) {
            h.ui(() -> {
                h.request(-1);
                check(!h.queued(), "infinite timeout unexpectedly queued a timer");
                h.backend.callback.accept(location());
                h.clean("infinite timeout completion");
                for (int i = 0; i < 1000; ++i) {
                    h.request(TimeUnit.DAYS.toMillis(30));
                    h.backend.callback.accept(location());
                }
                h.clean("repeated successful requests");
            });
        }
    }

    private static void queuedThenNavigation() throws Exception {
        try (Harness h = new Harness()) {
            h.ui(() -> {
                h.request(TimeUnit.DAYS.toMillis(30));
                h.backend.deliver(location()); // result is enqueued behind this UI turn
                h.shim.pageGone();
            });
            h.ui(() -> {
                h.clean("result queued before navigation");
                check(h.provider.host.messages.isEmpty(), "queued result reached new document");
            });
        }
    }

    private static void rejectedTimeout() throws Exception {
        Harness h = new Harness();
        try {
            h.ui(() -> {
                h.thread.quit();
                h.request(TimeUnit.DAYS.toMillis(30));
                h.clean("rejected timeout post");
                check(h.backend.cancels == 1, "rejected timeout left backend request alive");
            });
        } finally {
            h.thread.quit(); h.thread.join(5000);
            check(!h.thread.isAlive(), "closed Looper thread leaked");
        }
    }

    private static void timeout() throws Exception {
        try (Harness h = new Harness()) {
            h.backend.callbackOnCancel = true;
            h.ui(() -> h.request(0));
            h.ui(() -> {
                h.clean("timeout");
                check(h.backend.cancels == 1, "timeout did not cancel location");
                check(h.provider.host.messages.size() == 1
                        && h.provider.host.messages.get(0).contains("\"error\":3"), "wrong timeout result");
                h.backend.callback.accept(location());
                check(h.provider.host.messages.size() == 1, "late timeout result delivered");
            });
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        completion("async", false); completion("async", true); completion("inline", false);
        navigation(); failure("security"); failure("failure"); timeout();
        noTimeout(); queuedThenNavigation(); rejectedTimeout();
        System.out.println("Geolocation retention host passed: " + checks + " checks");
    }
}
