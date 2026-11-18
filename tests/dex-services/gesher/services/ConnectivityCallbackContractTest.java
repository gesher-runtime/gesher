/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.PendingIntent;
import android.content.ContentResolver;
import android.content.ContextWrapper;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Message;
import android.os.MessageQueue;
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

    /** Controlled enqueue/dispatch over a real legacy MessageQueue linked list.
     * Handler.removeCallbacks and MessageQueue removal/recycling run unchanged;
     * no native poller is created. This test process selects legacy queue mode. */
    private static final class ControlledHandler extends Handler {
        private static final class Task {
            final Message message; final Runnable callback; final boolean delayed;
            Task(Message message, boolean delayed) { this.message = message; this.callback = message.getCallback(); this.delayed = delayed; }
        }
        List<Task> tasks;
        MessageQueue queue;
        static Field declared(Class<?> type, String name) throws Exception {
            Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
        }
        private ControlledHandler() { super(); }
        static ControlledHandler create() throws Exception {
            ControlledHandler handler = (ControlledHandler)UNSAFE.allocateInstance(ControlledHandler.class);
            handler.queue = (MessageQueue)UNSAFE.allocateInstance(MessageQueue.class);
            declared(MessageQueue.class, "sUseConcurrentInitialized").setBoolean(null, true);
            declared(MessageQueue.class, "sUseConcurrent").setBoolean(null, false);
            declared(MessageQueue.class, "mMessageCount").set(handler.queue, new java.util.concurrent.atomic.AtomicLong());
            declared(Handler.class, "mQueue").set(handler, handler.queue);
            handler.tasks = new ArrayList<>(); return handler;
        }
        void prune() { tasks.removeIf(t -> t.message.getCallback() != t.callback); }
        @Override public boolean sendMessageAtTime(Message message, long when) {
            synchronized (tasks) {
                prune();
                try {
                    synchronized (queue) {
                        message.setTarget(this);
                        declared(Message.class, "next").set(message, declared(MessageQueue.class, "mMessages").get(queue));
                        declared(MessageQueue.class, "mMessages").set(queue, message);
                        ((java.util.concurrent.atomic.AtomicLong)declared(MessageQueue.class, "mMessageCount").get(queue)).incrementAndGet();
                    }
                } catch (Exception e) { throw new AssertionError(e); }
                tasks.add(new Task(message, when > SystemClock.uptimeMillis() + 1000));
            }
            return true;
        }
        void immediate() {
            while (true) {
                Runnable next = null;
                synchronized (tasks) {
                    prune();
                    for (int i = 0; i < tasks.size(); i++) {
                        if (!tasks.get(i).delayed) { next = tasks.remove(i).message.getCallback(); removeCallbacks(next); break; }
                    }
                }
                if (next == null) return;
                next.run();
            }
        }
        Runnable timeout() {
            synchronized (tasks) {
                prune();
                for (int i = 0; i < tasks.size(); i++) {
                    if (tasks.get(i).delayed) {
                        Runnable next = tasks.remove(i).message.getCallback(); removeCallbacks(next); return next;
                    }
                }
            }
            throw new AssertionError("Expected a request deadline");
        }
        int deadlines() { synchronized (tasks) { prune(); return (int)tasks.stream().filter(t -> t.delayed).count(); } }
    }
    /** Real Handler timestamp ordering, with one controlled registration-thread
     * deschedule after enqueue. This queue has no native poller; removal and
     * ordering use the unchanged framework MessageQueue implementation. */
    private static final class OrderedHandler extends Handler {
        MessageQueue queue;
        boolean paused;
        private OrderedHandler() { super(); }
        static OrderedHandler create() throws Exception {
            OrderedHandler handler = (OrderedHandler)UNSAFE.allocateInstance(OrderedHandler.class);
            handler.queue = (MessageQueue)UNSAFE.allocateInstance(MessageQueue.class);
            ControlledHandler.declared(MessageQueue.class, "sUseConcurrentInitialized").setBoolean(null, true);
            ControlledHandler.declared(MessageQueue.class, "sUseConcurrent").setBoolean(null, false);
            ControlledHandler.declared(MessageQueue.class, "mMessageCount").set(handler.queue,
                    new java.util.concurrent.atomic.AtomicLong());
            ControlledHandler.declared(Handler.class, "mQueue").set(handler, handler.queue);
            return handler;
        }
        private static void until(long when) {
            while (SystemClock.uptimeMillis() < when) {
                try { Thread.sleep(1); }
                catch (InterruptedException e) {
                    Thread.currentThread().interrupt(); throw new AssertionError(e);
                }
            }
        }
        @Override public boolean sendMessageAtTime(Message message, long when) {
            boolean accepted = super.sendMessageAtTime(message, when);
            if (!paused) {
                paused = true;
                until(when + 20);
            }
            return accepted;
        }
        void drain() throws Exception {
            for (int count = 0; count < 8; count++) {
                Message message;
                synchronized (queue) {
                    message = (Message)ControlledHandler.declared(MessageQueue.class, "mMessages").get(queue);
                }
                if (message == null) return;
                until(message.getWhen());
                Runnable callback = message.getCallback();
                removeCallbacks(callback);
                callback.run();
            }
            throw new AssertionError("Unexpected request queue growth");
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
        boolean failAfterAdd;
        Runnable beforeAdd, duringSnapshot;
        volatile Removal beforeRemoval;
        @Override public synchronized String getSnapshot() {
            snapshots++;
            if (rejectSnapshot) throw new SecurityException("controlled permission denial");
            String answer = snapshot;
            Runnable action = duringSnapshot;
            if (action != null) action.run();
            return answer;
        }
        @Override public synchronized void addListener(IConnectivity.IListener listener) throws RemoteException {
            if (beforeAdd != null) beforeAdd.run();
            adds++; listeners.add(listener);
            if (failAfterAdd) throw new RemoteException("controlled partial attachment");
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
        final ConnectivityManager manager = new ConnectivityManager(new ContextWrapper(null) {
            @Override public ContentResolver getContentResolver() { return null; }
        });
        final ControlledHandler handler = ControlledHandler.create();
        final Object lock = value("sLock");
        final Service service = new Service();
        final Object previousService = value("sService"), previousHandler = value("sConnectivityHandler");
        @SuppressWarnings("unchecked") final Map<ConnectivityManager.NetworkCallback, ?> registrations =
                (Map<ConnectivityManager.NetworkCallback, ?>)value("sCallbacks");
        @SuppressWarnings("unchecked") final Map<PendingIntent, ConnectivityManager.NetworkCallback> pending =
                (Map<PendingIntent, ConnectivityManager.NetworkCallback>)value("sIntentCallbacks");
        Harness(boolean parcel) throws Exception {
            check(registrations.isEmpty() && pending.isEmpty() && value("sListener") == null,
                    "fixture starts without existing registrations");
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
            for (PendingIntent operation : new ArrayList<>(pending.keySet())) {
                manager.releaseNetworkRequest(operation);
            }
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
            check(h.count() == 0 && h.service.count() == 0 && value("sListener") == null,
                    "failed initialization releases its partial service listener");
            h.service.rejectSnapshot = false;
            int addsBefore = h.service.adds, removesBefore = h.service.removes;
            h.register(never); h.handler.immediate();
            check(h.count() == 1 && h.service.count() == 1 && never.available == 0, "successful registration");
            h.manager.unregisterNetworkCallback(never);
            h.manager.unregisterNetworkCallback(never);
            check(h.count() == 0 && h.service.removes == removesBefore + 1 && h.service.count() == 0, "repeated unregister is a no-op");
            h.register(never); h.manager.unregisterNetworkCallback(never);
            check(h.service.adds == addsBefore + 2 && h.service.removes == removesBefore + 2, "same callback can be reused immediately");
        }
    }
    private static void subscriptions(boolean parcel) throws Exception {
        try (Harness h = new Harness(parcel)) {
            // Advance between the old pre-subscription read and attachment. A
            // listener is not entitled to receive that earlier event.
            h.service.beforeAdd = () -> h.service.snapshot = ONLINE;
            Callback callback = new Callback(); h.register(callback); h.handler.immediate();
            check(callback.available == 1 && h.manager.getActiveNetwork() != null,
                    "registration observes a change before listener attachment without another event");
            h.manager.unregisterNetworkCallback(callback);
        }
        try (Harness h = new Harness(parcel)) {
            // A same-process Binder can publish during getSnapshot while that
            // call still returns its previously captured value.
            h.service.duringSnapshot = () -> {
                h.service.duringSnapshot = null;
                try { h.service.emit(ONLINE); }
                catch (RemoteException failure) { throw new AssertionError(failure); }
            };
            Callback callback = new Callback(); h.register(callback); h.handler.immediate();
            check(callback.available == 1 && h.manager.getActiveNetwork() != null,
                    "reentrant newer publication wins over the captured initial answer");
        }
        try (Harness h = new Harness(parcel)) {
            Callback callback = new Callback(); h.service.failAfterAdd = true;
            h.service.beforeRemoval = listener -> check(!Thread.holdsLock(h.lock),
                    "failed-attachment cleanup is outside the registration lock");
            rejects(RuntimeException.class, () -> h.register(callback), "partial remote attachment failure");
            check(h.count() == 0 && h.service.count() == 0 && value("sListener") == null,
                    "partial attachment failure leaves neither a callback nor listener");
            h.service.failAfterAdd = false;
            h.register(callback); h.service.emit(ONLINE); h.handler.immediate();
            check(callback.available == 1 && h.count() == 1 && h.service.count() == 1,
                    "a failed attachment does not poison retry");
        }
        try (Harness h = new Harness(parcel)) {
            h.service.snapshot = ONLINE;
            h.service.duringSnapshot = () -> {
                h.service.duringSnapshot = null;
                try { h.service.emit(EMPTY); }
                catch (RemoteException failure) { throw new AssertionError(failure); }
            };
            Callback callback = new Callback(); h.register(callback); h.handler.immediate();
            check(callback.available == 0 && h.manager.getActiveNetwork() == null,
                    "reentrant disconnect cannot be overwritten by an older online answer");
        }
        try (Harness h = new Harness(parcel)) {
            Callback callback = new Callback(); h.service.failAfterAdd = true;
            CountDownLatch removing = new CountDownLatch(1), permitRemoval = new CountDownLatch(1);
            CountDownLatch registered = new CountDownLatch(1);
            AtomicReference<IConnectivity.IListener> retired = new AtomicReference<>();
            AtomicReference<Throwable> firstFailure = new AtomicReference<>(), retryFailure = new AtomicReference<>();
            h.service.beforeRemoval = listener -> {
                if (!retired.compareAndSet(null, listener)) return;
                removing.countDown(); await(permitRemoval, "failed listener cleanup unblocked");
            };
            Thread first = new Thread(() -> {
                try { h.register(callback); }
                catch (Throwable failure) { firstFailure.set(failure); }
            }, "connectivity-failed-attach");
            Thread retry = new Thread(() -> {
                try { h.register(callback); registered.countDown(); }
                catch (Throwable failure) { retryFailure.set(failure); }
            }, "connectivity-attach-retry");
            boolean concurrent;
            first.start();
            try {
                await(removing, "failed attachment reaches listener cleanup");
                h.service.failAfterAdd = false;
                retry.start(); concurrent = registered.await(2, TimeUnit.SECONDS);
            } finally {
                permitRemoval.countDown(); first.join(5000); retry.join(5000);
                h.service.beforeRemoval = null;
            }
            check(!first.isAlive() && !retry.isAlive() && firstFailure.get() instanceof RuntimeException
                            && retryFailure.get() == null,
                    "failed attachment and retry finish independently");
            check(concurrent && h.count() == 1 && h.service.count() == 1
                            && h.service.first().asBinder() != retired.get().asBinder(),
                    "failed attachment cleanup neither blocks nor detaches its replacement");
            retired.get().onSnapshot(ONLINE); h.handler.immediate();
            check(callback.available == 0 && h.manager.getActiveNetwork() == null,
                    "failed old listener cannot publish into a successful retry");
            h.service.emit(ONLINE); h.handler.immediate();
            check(callback.available == 1, "retry listener receives subsequent network changes");
        }
    }

    /** Actual PendingIntent.send with its declared ActivityManager IPC boundary controlled. */
    private static final class PendingRuntime extends IActivityManager.Default implements AutoCloseable {
        final Object singleton;
        final Field instance, current;
        final Object previousManager, previousThread;
        final Harness harness;
        int sends;
        boolean cancelNext;
        Runnable sending;
        Intent last;
        PendingRuntime(Harness harness) throws Exception {
            this.harness = harness;
            Field manager = ActivityManager.class.getDeclaredField("IActivityManagerSingleton");
            manager.setAccessible(true); singleton = manager.get(null);
            instance = android.util.Singleton.class.getDeclaredField("mInstance"); instance.setAccessible(true);
            previousManager = instance.get(singleton);
            current = ActivityThread.class.getDeclaredField("sCurrentActivityThread"); current.setAccessible(true);
            previousThread = current.get(null);
            current.set(null, UNSAFE.allocateInstance(ActivityThread.class));
            instance.set(singleton, this);
        }
        @Override public ActivityManager.PendingIntentInfo getInfoForIntentSender(IIntentSender target) {
            return new ActivityManager.PendingIntentInfo("gesher.connectivity.fixture", 1000, false,
                    ActivityManager.INTENT_SENDER_BROADCAST);
        }
        @Override public int sendIntentSender(IApplicationThread caller, IIntentSender target,
                IBinder allowlist, int code, Intent intent, String type, IIntentReceiver finished,
                String permission, Bundle options) {
            check(!Thread.holdsLock(harness.lock), "PendingIntent dispatch is outside the registration lock");
            sends++; last = new Intent(intent);
            boolean canceled = cancelNext; cancelNext = false;
            Runnable action = sending; sending = null;
            if (action != null) action.run();
            return canceled ? ActivityManager.START_CANCELED : 0;
        }
        PendingIntent token() {
            return new PendingIntent(new IIntentSender.Stub() {
                @Override public void send(int code, Intent intent, String type, IBinder allowlist,
                        IIntentReceiver finished, String permission, Bundle options) {
                    throw new AssertionError("The public API must go through ActivityManager");
                }
            });
        }
        @Override public void close() throws Exception {
            current.set(null, previousThread); instance.set(singleton, previousManager);
        }
    }

    private static void pending(boolean parcel, boolean cancellationOnly) throws Exception {
        if (!cancellationOnly) try (Harness h = new Harness(parcel); PendingRuntime runtime = new PendingRuntime(h)) {
            PendingIntent operation = runtime.token();
            h.manager.registerNetworkCallback(request(), operation);
            // Interleave a second replacement in the old split release/register
            // sequence's unlocked service-removal window. That sequence orphans it.
            AtomicInteger interleaved = new AtomicInteger();
            h.service.beforeRemoval = listener -> {
                if (interleaved.getAndIncrement() != 0) return;
                AtomicReference<Throwable> failure = new AtomicReference<>();
                Thread peer = new Thread(() -> {
                    try { h.manager.requestNetwork(request(), operation); }
                    catch (Throwable error) { failure.set(error); }
                }, "pending-network-replacement");
                peer.start();
                try { peer.join(5000); }
                catch (InterruptedException failureToWait) { throw new AssertionError(failureToWait); }
                check(!peer.isAlive() && failure.get() == null, "replacement during retired listener cleanup");
            };
            h.manager.registerNetworkCallback(request(), operation);
            h.service.beforeRemoval = null;
            if (interleaved.get() == 0) h.manager.requestNetwork(request(), operation);
            check(h.count() == 1 && h.pending.size() == 1 && h.service.count() == 1,
                    "same-token replacements retain exactly one owned callback");
            h.service.emit(ONLINE); h.handler.immediate();
            check(runtime.sends == 1, "only the surviving PendingIntent registration delivers");
            check(runtime.last.getParcelableExtra(ConnectivityManager.EXTRA_NETWORK, Network.class) != null
                    && runtime.last.getParcelableExtra(ConnectivityManager.EXTRA_NETWORK_REQUEST, NetworkRequest.class) != null,
                    "PendingIntent retains its public network and request extras");
            h.manager.releaseNetworkRequest(operation); h.manager.unregisterNetworkCallback(operation);
            check(h.count() == 0 && h.pending.isEmpty() && h.service.count() == 0,
                    "release retires the token and its callback exactly once");
        }
        try (Harness h = new Harness(parcel); PendingRuntime runtime = new PendingRuntime(h)) {
            PendingIntent operation = runtime.token(); h.service.snapshot = ONLINE;
            h.manager.requestNetwork(request(), operation);
            runtime.cancelNext = true;
            runtime.sending = () -> h.manager.registerNetworkCallback(request(), operation);
            h.handler.immediate();
            check(runtime.sends == 2 && h.count() == 1 && h.pending.size() == 1,
                    "a canceled in-flight old send cannot retire its replacement");
            runtime.cancelNext = true;
            h.service.emit(EMPTY); h.handler.immediate();
            h.service.emit(ONLINE); h.handler.immediate();
            check(h.count() == 0 && h.pending.isEmpty() && h.service.count() == 0,
                    "cancellation of the current token does retire its own registration");
        }
        if (!cancellationOnly) try (Harness h = new Harness(parcel); PendingRuntime runtime = new PendingRuntime(h)) {
            PendingIntent operation = runtime.token(); h.manager.requestNetwork(request(), operation);
            CountDownLatch start = new CountDownLatch(1);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Runnable replace = () -> {
                try { await(start, "concurrent replacements released"); h.manager.requestNetwork(request(), operation); }
                catch (Throwable error) { failure.set(error); }
            };
            Thread first = new Thread(replace, "pending-replace-a"), second = new Thread(replace, "pending-replace-b");
            first.start(); second.start(); start.countDown(); first.join(5000); second.join(5000);
            check(!first.isAlive() && !second.isAlive() && failure.get() == null, "concurrent replacements complete");
            check(h.count() == 1 && h.pending.size() == 1 && h.service.count() == 1,
                    "concurrent same-token registration has one owner");
            h.service.emit(ONLINE); h.handler.immediate();
            check(runtime.sends == 1, "concurrent replacement has one delivery");
        }
        if (!cancellationOnly) try (Harness h = new Harness(parcel); PendingRuntime runtime = new PendingRuntime(h)) {
            PendingIntent original = runtime.token();
            PendingIntent alias = new PendingIntent(original.getTarget());
            PendingIntent peer = runtime.token();
            check(original != alias && original.equals(alias),
                    "distinct PendingIntent wrappers refer to the same binder token");
            h.manager.requestNetwork(request(), original);
            h.manager.registerNetworkCallback(request(), alias);
            h.manager.requestNetwork(request(), peer);
            check(h.count() == 2 && h.pending.size() == 2 && h.service.adds == 1,
                    "equal-token replacement keeps a separate token and the shared listener");
            h.manager.releaseNetworkRequest(original);
            check(h.count() == 1 && h.pending.size() == 1 && h.pending.containsKey(peer)
                            && h.service.count() == 1,
                    "release through an equivalent wrapper preserves a distinct token");
            h.service.emit(ONLINE); h.handler.immediate();
            check(runtime.sends == 1, "only the independent token delivers after alias release");
            h.manager.unregisterNetworkCallback(new PendingIntent(peer.getTarget()));
            check(h.count() == 0 && h.pending.isEmpty() && h.service.count() == 0,
                    "equivalent wrapper releases the last token and listener");
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
            h.service.snapshot = ONLINE;
            OrderedHandler handler = OrderedHandler.create();
            Callback callback = new Callback();
            h.manager.requestNetwork(request(), callback, handler, 10);
            handler.drain();
            check(callback.available == 1 && callback.unavailable == 0 && h.count() == 1,
                    "already available network precedes its deadline after registration descheduling");
        }
        try (Harness h = new Harness(parcel)) {
            Callback callback = new Callback();
            h.manager.requestNetwork(request(), callback, h.handler, Integer.MAX_VALUE);
            check(h.handler.deadlines() == 1, "long deadline really queued");
            h.manager.unregisterNetworkCallback(callback);
            check(h.handler.deadlines() == 0, "unregister retained a long deadline");
            h.manager.requestNetwork(request(), callback, h.handler, Integer.MAX_VALUE);
            h.service.emit(ONLINE); h.handler.immediate();
            check(h.handler.deadlines() == 0, "first availability retained its deadline");
            h.manager.unregisterNetworkCallback(callback);
            h.service.snapshot = ONLINE;
            h.manager.requestNetwork(request(), callback, h.handler, Integer.MAX_VALUE);
            h.handler.immediate();
            check(h.handler.deadlines() == 0, "initial availability raced timeout installation");
        }
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
        if (selected.equals("all") || selected.equals("subscriptions")) subscriptions(parcel);
        if (selected.equals("all") || selected.equals("pending") || selected.equals("pending-cancel")) {
            pending(parcel, selected.equals("pending-cancel"));
        }
        if (parcel) liveHandler();
        System.out.println("ConnectivityCallbackContractTest all passed: " + checks.get() + ", parcel=" + parcel);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime"); contracts(args.length == 0 ? "all" : args[0], true);
    }
}
