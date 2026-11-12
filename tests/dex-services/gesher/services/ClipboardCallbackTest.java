/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ClipData;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import android.os.IBinder;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Actual clipboard service and framework callback list, with controlled callback-token death. */
public final class ClipboardCallbackTest {
    private static final String PACKAGE = "gesher.clipboard.fixture";
    public static int checks;

    private static synchronized void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    public static IClipboard service() throws Exception {
        Constructor<?> constructor = Class.forName("com.gesher.server.ClipboardService").getDeclaredConstructor();
        constructor.setAccessible(true); return (IClipboard)constructor.newInstance();
    }
    private static final class Listener implements IOnPrimaryClipChangedListener {
        final PowerWakeLockCallbackTest.Token token = new PowerWakeLockCallbackTest.Token();
        final AtomicInteger calls = new AtomicInteger();
        Runnable action;
        @Override public IBinder asBinder() { return token.binder; }
        @Override public void dispatchPrimaryClipChanged() throws RemoteException {
            calls.incrementAndGet(); if (action != null) action.run();
        }
    }
    private static void add(IClipboard service, Listener listener) throws Exception {
        service.addPrimaryClipChangedListener(listener, PACKAGE, null, 0, 0);
    }
    private static void remove(IClipboard service, Listener listener) throws Exception {
        service.removePrimaryClipChangedListener(listener, PACKAGE, null, 0, 0);
    }
    private static void clear(IClipboard service) {
        try { service.clearPrimaryClip(PACKAGE, null, 0, 0); }
        catch (RemoteException failure) { throw new AssertionError(failure); }
    }
    private static void await(CountDownLatch latch, String why) {
        try { check(latch.await(5, TimeUnit.SECONDS), why); }
        catch (InterruptedException failure) { Thread.currentThread().interrupt(); throw new AssertionError(failure); }
    }

    public static void contracts() throws Exception {
        IClipboard reentrant = service();
        Object list = field(reentrant.getClass(), "mListeners").get(reentrant);
        check(list instanceof RemoteCallbackList, "Fixture must use framework RemoteCallbackList");
        Listener nested = new Listener();
        nested.action = () -> {
            check(!Thread.holdsLock(reentrant) && !Thread.holdsLock(list), "Callback held clipboard/list monitor");
            if (nested.calls.get() == 1) clear(reentrant);
        };
        add(reentrant, nested); clear(reentrant);
        check(nested.calls.get() == 2, "Reentrant clipboard update lost its callback");
        remove(reentrant, nested);

        IClipboard concurrent = service();
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicReference<Throwable> failed = new AtomicReference<>();
        Listener paused = new Listener();
        paused.action = () -> { if (paused.calls.get() == 1) { entered.countDown(); await(release, "First callback was not released"); } };
        add(concurrent, paused);
        Thread first = new Thread(() -> {
            try { clear(concurrent); } catch (Throwable failure) { failed.set(failure); }
        }, "clipboard-paused-callback");
        first.start();
        try { await(entered, "First notification never entered"); clear(concurrent); }
        finally { release.countDown(); first.join(5000); }
        check(!first.isAlive() && failed.get() == null && paused.calls.get() == 2,
                "Concurrent clipboard broadcasts overlapped an active framework snapshot: " + failed.get());
        remove(concurrent, paused);

        IClipboard changing = service(); Listener added = new Listener(), mutating = new Listener();
        mutating.action = () -> {
            try { remove(changing, mutating); add(changing, added); }
            catch (Exception failure) { throw new AssertionError(failure); }
        };
        add(changing, mutating); clear(changing);
        check(mutating.calls.get() == 1 && added.calls.get() == 0, "Listener mutation changed in-flight snapshot");
        clear(changing);
        check(mutating.calls.get() == 1 && added.calls.get() == 1, "Listener mutation did not affect next notification");
        remove(changing, added);

        IClipboard isolation = service(); Listener broken = new Listener(), healthy = new Listener(), dead = new Listener();
        broken.action = () -> { throw new IllegalStateException("Expected local clipboard listener failure"); };
        add(isolation, broken); add(isolation, healthy); add(isolation, dead); dead.token.die();
        clear(isolation); clear(isolation);
        check(broken.calls.get() == 2 && healthy.calls.get() == 2 && dead.calls.get() == 0,
                "Dead/throwing listener suppressed a live listener or left the broadcast active");
        remove(isolation, broken); remove(isolation, healthy);
        check(((RemoteCallbackList<?>)field(isolation.getClass(), "mListeners").get(isolation))
                .getRegisteredCallbackCount() == 0, "Callback death/unregistration leaked listeners");
    }

    /** Windows run additionally observes native clipboard writes and Windows event delivery. */
    private static void windowsClipboard() throws Exception {
        Class<?> host = Class.forName("com.gesher.server.WindowsHost");
        check((Boolean)host.getMethod("available").invoke(null), "Windows clipboard backend unavailable");
        IClipboard service = service();
        ClipData original = service.getPrimaryClip(PACKAGE, null, 0, 0);
        Listener listener = new Listener();
        try {
            String text = "gesher-clipboard-" + System.nanoTime();
            service.setPrimaryClip(ClipData.newHtmlText("fixture", text, "<b>" + text + "</b>"), PACKAGE, null, 0, 0);
            check(text.equals(host.getMethod("clipboardText").invoke(null)), "Service clipboard did not reach Windows");
            CountDownLatch changed = new CountDownLatch(1); listener.action = changed::countDown;
            add(service, listener);
            String external = text + "-external";
            check((Boolean)host.getMethod("setClipboard", String.class, String.class).invoke(null, external, null),
                    "Windows fixture clipboard write failed");
            await(changed, "Native Windows clipboard change did not notify the service listener");
            ClipData received = service.getPrimaryClip(PACKAGE, null, 0, 0);
            check(received != null && external.contentEquals(received.getItemAt(0).getText()),
                    "Native clipboard change was not reflected in the primary clip");
        } finally {
            remove(service, listener);
            if (original != null) service.setPrimaryClip(original, PACKAGE, null, 0, 0); else clear(service);
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        // The concurrency fixture keeps Windows event noise separate from its exact counts.
        Class<?> host = Class.forName("com.gesher.server.WindowsHost");
        Field available = field(host, "sAvailable"); Object previous = available.get(null);
        try { available.set(null, false); contracts(); }
        finally { available.set(null, previous); }
        windowsClipboard();
        System.out.println("ClipboardCallbackTest all passed: " + checks + " callback and native clipboard checks");
    }
}
