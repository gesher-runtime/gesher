/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Message;
import android.os.RemoteException;
import android.os.SystemClock;
import com.gesher.net.IConnectivity;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Production registration code with a controlled service and deterministic Handler scheduling.
 * Windows additionally exercises real Binder Parcel transport and a real HandlerThread.
 */
public final class ConnectivityCallbackContractTest {
    private static final String EMPTY = "internet\t0\n";
    private static final String ONLINE = "internet\t1\nnet\t12\tethernet\t1500\t1000000\t1000000\t0\t0\t0\t1\t1\n";
    private static final Unsafe UNSAFE;
    private static final AtomicInteger checks = new AtomicInteger();
    static {
        try { Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true); UNSAFE = (Unsafe)field.get(null); }
        catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Work { void run() throws Exception; }
    private interface Removal { void run(IConnectivity.IListener listener) throws RemoteException; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks.incrementAndGet();
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String message) throws Exception {
        try { work.run(); }
        catch (Throwable failure) { check(type.isInstance(failure), message + ": " + failure); return; }
        throw new AssertionError(message + ": no exception");
    }
    private static Field field(String name) throws Exception {
        Field field = ConnectivityManager.class.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static Object value(String name) throws Exception { return field(name).get(null); }
    private static void value(String name, Object value) throws Exception { field(name).set(null, value); }
    private static void await(CountDownLatch latch, String message) {
        try { check(latch.await(5, TimeUnit.SECONDS), message); }
        catch (InterruptedException failure) { Thread.currentThread().interrupt(); throw new AssertionError(failure); }
    }
    private static NetworkRequest request() { return new NetworkRequest.Builder().clearCapabilities().build(); }

    /** This legal Handler subclass captures messages rather than substituting framework classes. */
    private static final class ControlledHandler extends Handler {
        private static final class Task {
            final Runnable callback; final boolean delayed;
            Task(Runnable callback, boolean delayed) { this.callback = callback; this.delayed = delayed; }
        }
        List<Task> tasks;
        private ControlledHandler() { super(); }
        static ControlledHandler create() throws Exception {
            ControlledHandler handler = (ControlledHandler)UNSAFE.allocateInstance(ControlledHandler.class);
            handler.tasks = new ArrayList<>(); return handler;
        }
        @Override public boolean sendMessageAtTime(Message message, long when) {
            synchronized (tasks) {
                tasks.add(new Task(message.getCallback(), when > SystemClock.uptimeMillis() + 1000));
            }
            return true;
        }
        void immediate() {
            while (true) {
                Runnable next = null;
                synchronized (tasks) {
                    for (int i = 0; i < tasks.size(); i++) {
                        if (!tasks.get(i).delayed) { next = tasks.remove(i).callback; break; }
                    }
                }
                if (next == null) return;
                next.run();
            }
        }
        Runnable timeout() {
            synchronized (tasks) {
                for (int i = 0; i < tasks.size(); i++) {
                    if (tasks.get(i).delayed) return tasks.remove(i).callback;
                }
            }
            throw new AssertionError("Expected a request deadline");
        }
    }
    private static class Callback extends ConnectivityManager.NetworkCallback {
        int available, lost, unavailable;
        Runnable unavailableAction;
        @Override public void onAvailable(Network network) { available++; }
        @Override public void onLost(Network network) { lost++; }
        @Override public void onUnavailable() { unavailable++; if (unavailableAction != null) unavailableAction.run(); }
    }
    private static final class EqualCallback extends Callback {
        @Override public boolean equals(Object other) { return other instanceof EqualCallback; }
        @Override public int hashCode() { return 1; }
    }
    private static final class Service extends IConnectivity.Stub {
        final List<IConnectivity.IListener> listeners = new ArrayList<>();
        String snapshot = EMPTY;
        int adds, removes, snapshots;
        boolean rejectSnapshot, failRemoval;
        volatile Removal beforeRemoval;
        @Override public synchronized String getSnapshot() {
            snapshots++;
            if (rejectSnapshot) throw new SecurityException("controlled permission denial");
            return snapshot;
        }
        @Override public synchronized void addListener(IConnectivity.IListener listener) {
            adds++; listeners.add(listener);
        }
        @Override public void removeListener(IConnectivity.IListener listener) throws RemoteException {
            Removal before = beforeRemoval;
            if (before != null) before.run(listener);
            synchronized (this) {
                removes++;
                for (int i = 0; i < listeners.size(); i++) {
                    if (listeners.get(i).asBinder() == listener.asBinder()) { listeners.remove(i); break; }
                }
                if (failRemoval) throw new RemoteException("controlled dead service");
            }
        }
        synchronized IConnectivity.IListener first() { return listeners.get(0); }
        synchronized int count() { return listeners.size(); }
        void emit(String text) throws RemoteException {
            List<IConnectivity.IListener> copy;
            synchronized (this) { snapshot = text; copy = new ArrayList<>(listeners); }
            for (IConnectivity.IListener listener : copy) listener.onSnapshot(text);
        }
    }
    private static final class Harness implements AutoCloseable {
        final ConnectivityManager manager = new ConnectivityManager(null);
        final ControlledHandler handler = ControlledHandler.create();
        final Object lock = value("sLock");
        final Service service = new Service();
        final Object previousService = value("sService"), previousHandler = value("sConnectivityHandler");
        @SuppressWarnings("unchecked") final Map<ConnectivityManager.NetworkCallback, ?> registrations =
                (Map<ConnectivityManager.NetworkCallback, ?>)value("sCallbacks");
        Harness(boolean parcel) throws Exception {
            check(registrations.isEmpty() && value("sListener") == null, "fixture starts without existing registrations");
            IConnectivity connection = service;
            if (parcel) {
                IBinder remote = (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class},
                        (self, method, args) -> {
                            if (method.getName().equals("queryLocalInterface")) return null;
                            try { return method.invoke(service.asBinder(), args); }
                            catch (InvocationTargetException failure) { throw failure.getCause(); }
                        });
                connection = IConnectivity.Stub.asInterface(remote);
            }
            value("sService", connection); value("sCurrent", null); value("sConnectivityHandler", handler);
        }
        int count() { synchronized (lock) { return registrations.size(); } }
        void register(Callback callback) { manager.registerNetworkCallback(ConnectivityCallbackContractTest.request(), callback, handler); }
        Runnable request(Callback callback) {
            manager.requestNetwork(ConnectivityCallbackContractTest.request(), callback, handler, 60_000); return handler.timeout();
        }
        @Override public void close() throws Exception {
            service.beforeRemoval = null; service.failRemoval = false;
            List<ConnectivityManager.NetworkCallback> copy;
            synchronized (lock) { copy = new ArrayList<>(registrations.keySet()); }
            for (ConnectivityManager.NetworkCallback callback : copy) manager.unregisterNetworkCallback(callback);
            value("sService", previousService); value("sCurrent", null); value("sListener", null);
            value("sConnectivityHandler", previousHandler);
        }
    }
    private static void basic(boolean parcel) throws Exception {
        try (Harness h = new Harness(parcel)) {
            Callback never = new Callback();
            rejects(IllegalArgumentException.class, () -> h.manager.unregisterNetworkCallback(never), "never registered callback rejects cleanup");
            rejects(IllegalArgumentException.class, () -> h.manager.unregisterNetworkCallback((ConnectivityManager.NetworkCallback)null), "null callback");
            h.service.rejectSnapshot = true;
            rejects(SecurityException.class, () -> h.register(never), "permission denied registration");
            rejects(IllegalArgumentException.class, () -> h.manager.unregisterNetworkCallback(never), "failed registration is still never registered");
            h.service.rejectSnapshot = false;
            h.register(never); h.handler.immediate();
            check(h.count() == 1 && h.service.count() == 1 && never.available == 0, "successful registration");
            h.manager.unregisterNetworkCallback(never);
            h.manager.unregisterNetworkCallback(never);
            check(h.count() == 0 && h.service.removes == 1 && h.service.count() == 0, "repeated unregister is a no-op");
            h.register(never); h.manager.unregisterNetworkCallback(never);
            check(h.service.adds == 2 && h.service.removes == 2, "same callback can be reused immediately");
        }
    }
    private static void identity(boolean parcel) throws Exception {
        try (Harness h = new Harness(parcel)) {
            Callback first = new EqualCallback(), second = new EqualCallback();
            h.register(first); h.register(second);
            check(h.count() == 2 && h.service.count() == 1, "equal callback instances remain distinct");
            h.service.emit(ONLINE); h.handler.immediate();
            check(first.available == 1 && second.available == 1, "both equal callbacks receive availability");
            h.manager.unregisterNetworkCallback(first);
            h.service.emit(EMPTY); h.handler.immediate();
            check(first.lost == 0 && second.lost == 1 && h.count() == 1, "identity cancellation preserves peer");
            check(h.service.removes == 0, "listener retained for second registration");
            h.manager.unregisterNetworkCallback(second);
            check(h.service.removes == 1 && h.service.count() == 0, "last identity retires listener once");
        }
    }
    private static void timeouts(boolean parcel) throws Exception {
        try (Harness h = new Harness(parcel)) {
            Callback callback = new Callback();
            Runnable obsolete = h.request(callback);
            IConnectivity.IListener oldListener = h.service.first();
            h.service.emit(ONLINE); // Queue old delivery, then retire its registration.
            h.manager.unregisterNetworkCallback(callback);
            h.service.snapshot = EMPTY;
            h.register(callback);
            oldListener.onSnapshot(ONLINE); obsolete.run(); h.handler.immediate();
            check(callback.available == 0 && callback.unavailable == 0 && h.count() == 1,
                    "queued old snapshot, old listener and old timeout cannot affect reuse");
            h.manager.unregisterNetworkCallback(callback);
            callback.unavailableAction = () -> {
                check(!Thread.holdsLock(h.lock), "onUnavailable is outside registration lock");
                h.manager.unregisterNetworkCallback(callback); h.register(callback);
            };
            Runnable timeout = h.request(callback);
            h.handler.immediate(); timeout.run(); h.handler.immediate(); timeout.run();
            check(callback.unavailable == 1 && h.count() == 1, "timeout retires once before reentrant cleanup and reuse");
            h.manager.unregisterNetworkCallback(callback); callback.unavailableAction = null;
            Runnable satisfiedDeadline = h.request(callback);
            h.service.emit(ONLINE); h.handler.immediate();
            h.service.emit(EMPTY); h.handler.immediate(); satisfiedDeadline.run();
            check(callback.available == 1 && callback.lost == 1 && callback.unavailable == 1,
                    "a once-satisfied request does not time out after network loss");
        }
    }
    private static void teardown(boolean parcel) throws Exception {
        try (Harness h = new Harness(parcel)) {
            Callback callback = new Callback(); h.register(callback);
            IConnectivity.IListener oldListener = h.service.first();
            CountDownLatch removing = new CountDownLatch(1), permitRemoval = new CountDownLatch(1), registered = new CountDownLatch(1);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            h.service.beforeRemoval = listener -> { removing.countDown(); await(permitRemoval, "removal unblocked"); };
            Thread retire = new Thread(() -> { try { h.manager.unregisterNetworkCallback(callback); } catch (Throwable error) { failure.set(error); } }, "connectivity-retire");
            Thread replace = new Thread(() -> { try { h.register(callback); registered.countDown(); } catch (Throwable error) { failure.set(error); } }, "connectivity-replace");
            boolean concurrent;
            retire.start();
            try {
                await(removing, "old remote listener removal entered"); replace.start();
                concurrent = registered.await(2, TimeUnit.SECONDS);
            } finally {
                permitRemoval.countDown(); retire.join(5000); replace.join(5000); h.service.beforeRemoval = null;
            }
            check(!retire.isAlive() && !replace.isAlive() && failure.get() == null, "teardown and replacement finish cleanly: " + failure.get());
            check(concurrent, "remote teardown does not hold registration lock");
            check(h.count() == 1 && h.service.count() == 1 && h.service.first().asBinder() != oldListener.asBinder(),
                    "retiring old listener leaves replacement registered");
            oldListener.onSnapshot(ONLINE); h.handler.immediate();
            check(callback.available == 0, "retired service listener cannot publish");
            h.service.emit(ONLINE); h.handler.immediate();
            check(callback.available == 1, "replacement listener can publish");
            h.service.failRemoval = true;
            h.manager.unregisterNetworkCallback(callback); h.manager.unregisterNetworkCallback(callback);
            check(h.count() == 0 && h.service.count() == 0, "remote removal failure does not resurrect local registration");
        }
    }
    private static void liveHandler() throws Exception {
        try (Harness h = new Harness(true)) {
            HandlerThread thread = new HandlerThread("connectivity-live-contract"); thread.start();
            CountDownLatch finished = new CountDownLatch(1); AtomicReference<Throwable> failure = new AtomicReference<>();
            Callback callback = new Callback();
            callback.unavailableAction = () -> {
                try { h.manager.unregisterNetworkCallback(callback); h.manager.unregisterNetworkCallback(callback); }
                catch (Throwable error) { failure.set(error); }
                finally { finished.countDown(); }
            };
            try {
                h.manager.requestNetwork(request(), callback, new Handler(thread.getLooper()), 25);
                await(finished, "real HandlerThread deadline delivered");
                check(failure.get() == null && callback.unavailable == 1 && h.count() == 0,
                        "real HandlerThread permits timeout cleanup: " + failure.get());
                check(h.service.snapshots > 0 && h.service.adds == 1 && h.service.removes == 1, "real Binder service calls complete");
            } finally { thread.quitSafely(); thread.join(5000); check(!thread.isAlive(), "real HandlerThread terminates"); }
        }
    }
    public static void contracts(String selected, boolean parcel) throws Exception {
        checks.set(0);
        if (selected.equals("all") || selected.equals("basic")) basic(parcel);
        if (selected.equals("all") || selected.equals("identity")) identity(parcel);
        if (selected.equals("all") || selected.equals("timeouts")) timeouts(parcel);
        if (selected.equals("all") || selected.equals("teardown")) teardown(parcel);
        if (parcel) liveHandler();
        System.out.println("ConnectivityCallbackContractTest all passed: " + checks.get() + ", parcel=" + parcel);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime"); contracts(args.length == 0 ? "all" : args[0], true);
    }
}
