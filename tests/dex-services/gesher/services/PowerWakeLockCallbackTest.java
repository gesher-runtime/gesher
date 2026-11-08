/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ContextWrapper;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.IPowerManager;
import android.os.IWakeLockCallback;
import android.os.Looper;
import android.os.Parcel;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.WorkSource;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Production service contracts; main additionally uses real framework/Binder/Windows natives. */
public final class PowerWakeLockCallbackTest {
    public static int checks;
    private interface Work { void run() throws Exception; }
    public static void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Work action, String why) throws Exception {
        try { action.run(); }
        catch (Throwable failure) { check(type.isInstance(failure), why + ": " + failure); return; }
        throw new AssertionError(why);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    public static IPowerManager service() throws Exception {
        Constructor<?> c = Class.forName("com.gesher.server.PowerManagerService").getDeclaredConstructor();
        c.setAccessible(true); return (IPowerManager)c.newInstance();
    }
    public static int held(IPowerManager service) throws Exception {
        synchronized (service) { return ((Map<?,?>)field(service.getClass(), "mLocks").get(service)).size(); }
    }
    public static void flush(IPowerManager service) throws Exception {
        ((ExecutorService)field(service.getClass(), "mCallbacks").get(service))
                .submit(() -> {}).get(5, TimeUnit.SECONDS);
    }

    /** Controlled Binder lifetime only: the production held-lock state is never replaced. */
    public static final class Token implements InvocationHandler {
        public final IBinder binder = (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(),
                new Class<?>[]{IBinder.class}, this);
        public final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        public boolean dead, dieDuringLink;
        @Override public synchronized Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            switch (method.getName()) {
                case "hashCode": return System.identityHashCode(proxy);
                case "equals": return proxy == args[0];
                case "toString": return "PowerFixtureToken@" + System.identityHashCode(proxy);
                case "linkToDeath":
                    if (dead) throw new RemoteException("fixture token already dead");
                    IBinder.DeathRecipient recipient = (IBinder.DeathRecipient)args[0];
                    recipients.add(recipient);
                    if (dieDuringLink) { dead = true; recipient.binderDied(); }
                    return null;
                case "unlinkToDeath": return recipients.remove(args[0]);
                case "isBinderAlive": case "pingBinder": return !dead;
                case "queryLocalInterface": return null;
                default: throw new UnsupportedOperationException("Unexpected fixture Binder call " + method);
            }
        }
        public void die() {
            List<IBinder.DeathRecipient> pending;
            synchronized (this) { dead = true; pending = new ArrayList<>(recipients); }
            for (IBinder.DeathRecipient r : pending) r.binderDied();
        }
    }

    public static class Callback implements IWakeLockCallback {
        public final Token token;
        public final BlockingQueue<Boolean> events = new LinkedBlockingQueue<>();
        private final Object service;
        private final Thread caller = Thread.currentThread();
        private volatile boolean locked, inline;
        public Callback(Object service) { this(service, new Token()); }
        public Callback(Object service, Token token) { this.service = service; this.token = token; }
        @Override public IBinder asBinder() { return token.binder; }
        @Override public void onStateChanged(boolean enabled) throws RemoteException {
            locked |= Thread.holdsLock(service);
            inline |= Thread.currentThread() == caller;
            events.add(enabled);
        }
        public void event(boolean expected) throws Exception {
            Boolean actual = events.poll(5, TimeUnit.SECONDS);
            check(actual != null && actual == expected, "Missing/wrong wake-lock event: " + actual);
            check(!locked && !inline, "Wake-lock listener ran under the service lock or inline");
        }
        public void empty(IPowerManager service) throws Exception {
            flush(service); check(events.isEmpty(), "Unexpected wake-lock listener transition: " + events);
        }
    }

    public static void acquire(IPowerManager service, Token token, String tag, Callback callback) throws Exception {
        service.acquireWakeLock(token.binder, PowerManager.PARTIAL_WAKE_LOCK, tag,
                "gesher.power.fixture", null, null, 0, callback);
    }

    public static void contracts(IPowerManager service) throws Exception {
        Token token = new Token(); Callback first = new Callback(service), second = new Callback(service);
        acquire(service, token, "held", first); first.event(true);
        check(held(service) == 1 && token.recipients.size() == 1, "Acquire did not retain one linked lock");
        acquire(service, token, "held", first); first.empty(service);
        check(token.recipients.size() == 1, "Identical acquire added a death recipient");
        acquire(service, token, "held", second); second.empty(service);
        service.updateWakeLockCallback(token.binder, first); first.empty(service);
        service.updateWakeLockCallback(token.binder, new Callback(service, first.token)); first.empty(service);
        service.updateWakeLockCallback(token.binder, second); second.event(true); first.empty(service);
        service.updateWakeLockCallback(token.binder, null); second.empty(service);
        service.updateWakeLockCallback(token.binder, first); first.event(true);
        acquire(service, token, "changed", first); first.event(false); first.event(true);
        acquire(service, token, "changed-again", second); second.event(true); first.empty(service);
        service.releaseWakeLock(token.binder, 0); second.event(false);
        check(held(service) == 0 && token.recipients.isEmpty(), "Release retained state or death recipient");
        service.releaseWakeLock(token.binder, 0); second.empty(service);
        rejects(IllegalArgumentException.class, () -> service.updateWakeLockCallback(token.binder, first), "Unknown update accepted");
        rejects(IllegalArgumentException.class, () -> service.updateWakeLockCallback(null, first), "Null update accepted");
        rejects(IllegalArgumentException.class, () -> service.releaseWakeLock(null, 0), "Null release accepted");
        rejects(IllegalArgumentException.class, () -> service.acquireWakeLock(null, 1, "null", "pkg", null, null, 0, first), "Null acquisition accepted");
        rejects(IllegalArgumentException.class, () -> service.acquireWakeLock(token.binder, 0, "invalid", "pkg", null, null, 0, first), "Invalid lock level accepted");
        rejects(IllegalArgumentException.class, () -> service.acquireWakeLock(token.binder, 1, null, "pkg", null, null, 0, first), "Null tag accepted");
        rejects(IllegalArgumentException.class, () -> service.acquireWakeLock(token.binder, 1, "invalid", null, null, null, 0, first), "Null package accepted");

        Token dead = new Token(); dead.dead = true;
        rejects(IllegalArgumentException.class, () -> acquire(service, dead, "dead", first), "Dead token reported acquired");
        Token dying = new Token(); dying.dieDuringLink = true;
        rejects(IllegalArgumentException.class, () -> acquire(service, dying, "dying", first), "Death during registration published state");
        check(held(service) == 0 && dying.recipients.isEmpty(), "Failed registration retained state/recipient");
        first.empty(service);

        Token reused = new Token(); acquire(service, reused, "generation-one", first); first.event(true);
        IBinder.DeathRecipient stale = reused.recipients.get(0);
        service.releaseWakeLock(reused.binder, 0); first.event(false);
        acquire(service, reused, "generation-two", second); second.event(true);
        stale.binderDied(); second.empty(service);
        check(held(service) == 1 && reused.recipients.size() == 1, "Stale recipient removed a newer lock");
        reused.die(); second.event(false);
        check(held(service) == 0 && reused.recipients.isEmpty(), "Token death failed cleanup");

        Token quick = new Token(); Callback ordered = new Callback(service);
        acquire(service, quick, "quick", ordered); service.releaseWakeLock(quick.binder, 0);
        ordered.event(true); ordered.event(false); ordered.empty(service);
        AtomicInteger failures = new AtomicInteger(); Token failed = new Token();
        Callback broken = new Callback(service) {
            @Override public void onStateChanged(boolean enabled) throws RemoteException {
                failures.incrementAndGet();
                if (enabled) throw new RemoteException("listener is dead");
                throw new IllegalStateException("local listener rejected delivery");
            }
        };
        acquire(service, failed, "failed-listener", broken); flush(service);
        check(failures.get() == 1 && held(service) == 1, "Callback failure retired a held lock");
        service.releaseWakeLock(failed.binder, 0); flush(service);
        check(failures.get() == 2 && held(service) == 0, "Callback failure interrupted release");
        Token callbackOwner = new Token(); Callback separate = new Callback(service);
        acquire(service, callbackOwner, "separate-death", separate); separate.event(true);
        separate.token.die(); check(held(service) == 1, "Listener Binder death retired the lock token");
        service.releaseWakeLock(callbackOwner.binder, 0); separate.event(false);

        Token reentrant = new Token(); CountDownLatch ended = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Callback nested = new Callback(service) {
            @Override public void onStateChanged(boolean enabled) throws RemoteException {
                super.onStateChanged(enabled);
                if (enabled) {
                    try { service.releaseWakeLock(reentrant.binder, 0); }
                    catch (Throwable error) { failure.set(error); }
                } else ended.countDown();
            }
        };
        acquire(service, reentrant, "reentrant", nested);
        check(ended.await(5, TimeUnit.SECONDS) && failure.get() == null, "Reentrant callback deadlocked/failed: " + failure.get());
        nested.event(true); nested.event(false); check(held(service) == 0, "Reentrant release retained lock");

        Token attributed = new Token(); Callback sourceOne = new Callback(service), sourceTwo = new Callback(service);
        service.acquireWakeLock(attributed.binder, 1, "attributed", "gesher.power.fixture", new WorkSource(12345), null, 0, sourceOne);
        sourceOne.event(true);
        service.updateWakeLockCallback(attributed.binder, sourceTwo);
        sourceOne.empty(service); sourceTwo.empty(service);
        service.releaseWakeLock(attributed.binder, 0); sourceTwo.event(false);
        Token withUid = new Token(); Callback uid = new Callback(service);
        service.acquireWakeLockWithUid(withUid.binder, 1, "uid", "gesher.power.fixture", 12345, 0, uid);
        uid.event(true); service.releaseWakeLockAsync(withUid.binder, 0); uid.event(false);
        Token async = new Token(); service.acquireWakeLockAsync(async.binder, 1, "async", "gesher.power.fixture", null, null);
        check(held(service) == 1, "Async acquire lost real state"); service.releaseWakeLockAsync(async.binder, 0);
        check(held(service) == 0 && async.recipients.isEmpty(), "Async release retained state");
    }

    private static IPowerManager wire(IPowerManager service) throws Exception {
        Class<?> standIn = Class.forName("com.gesher.server.StandIn");
        standIn.getMethod("setStrict", boolean.class).invoke(null, true);
        Object stub = standIn.getMethod("of", Class.class, IInterface.class).invoke(null, IPowerManager.class, service);
        IBinder target = (IBinder)standIn.getMethod("asBinder").invoke(stub);
        return IPowerManager.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, request, reply, flags);
            }
        });
    }
    private static void publicManager(IPowerManager service) throws Exception {
        Looper.prepare();
        try {
            PowerManager manager = new PowerManager(new ContextWrapper(null) {
                @Override public String getOpPackageName() { return "gesher.power.fixture"; }
                @Override public int getDisplayId() { return 0; }
            }, wire(service), null, new Handler(Looper.myLooper()));
            PowerManager.WakeLock lock = manager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "public-listener");
            BlockingQueue<String> events = new LinkedBlockingQueue<>();
            AtomicReference<Throwable> bad = new AtomicReference<>();
            lock.setReferenceCounted(false);
            lock.setStateListener(Runnable::run, enabled -> {
                if (Thread.holdsLock(service)) bad.set(new AssertionError("Public listener held service monitor"));
                events.add("first:" + enabled);
            });
            lock.acquire();
            check("first:true".equals(events.poll(5, TimeUnit.SECONDS)), "Public setStateListener never acquired");
            lock.acquire(); flush(service); check(events.isEmpty(), "Public repeated acquire duplicated callback");
            lock.setStateListener(Runnable::run, enabled -> events.add("second:" + enabled));
            check("second:true".equals(events.poll(5, TimeUnit.SECONDS)), "Public listener replacement was lost");
            lock.release();
            check("second:false".equals(events.poll(5, TimeUnit.SECONDS)), "Public listener never released");
            check(!lock.isHeld() && held(service) == 0 && bad.get() == null, "Public callback state/lock failure");
            lock.setStateListener(Runnable::run, null); lock.acquire();
            lock.setStateListener(Runnable::run, enabled -> events.add("attached:" + enabled));
            check("attached:true".equals(events.poll(5, TimeUnit.SECONDS)), "Public listener attach while held was lost");
            lock.setStateListener(Runnable::run, null); lock.release(); flush(service);
            check(events.isEmpty(), "Cleared public listener received transition");
        } finally { Looper.myLooper().quit(); }
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        IPowerManager service = service();
        contracts(service); publicManager(service);
        System.out.println("PowerWakeLockCallbackTest all passed: " + checks + " checks");
    }
}
