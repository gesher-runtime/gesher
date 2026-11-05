/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.os.Binder;
import android.os.IBinder;
import android.os.IServiceCallback;
import android.os.IServiceManager;
import android.os.RemoteException;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Existing/future service registration, cancellation, reentrancy, and dead observers. */
public final class ServiceRegistrationCallbackTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Token extends Binder {
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        @Override public void linkToDeath(IBinder.DeathRecipient recipient, int flags) {
            recipients.add(recipient);
        }
        @Override public boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) {
            return recipients.remove(recipient);
        }
        void die() {
            for (IBinder.DeathRecipient recipient : new ArrayList<>(recipients)) recipient.binderDied();
        }
    }

    /** Replays a publication delayed after addService's registry-lock snapshot. */
    private static void delayedSnapshot(IServiceManager manager, String name, IBinder service)
            throws ReflectiveOperationException {
        Field field = manager.getClass().getDeclaredField("mNotifications");
        field.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, Map<IBinder, ?>> subscriptions = (Map<String, Map<IBinder, ?>>) field.get(manager);
        Object registration = subscriptions.get(name).values().iterator().next();
        Method dispatch = registration.getClass().getDeclaredMethod("notifyService", IBinder.class);
        dispatch.setAccessible(true);
        dispatch.invoke(registration, service);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Class<?> type = Class.forName("com.gesher.server.Services$ServiceManagerStandIn");
        Constructor<?> ctor = type.getDeclaredConstructor(Map.class);
        ctor.setAccessible(true);
        Binder first = new Binder();
        Map<String, IBinder> services = new LinkedHashMap<>();
        services.put("present", first);
        IServiceManager manager = (IServiceManager) ctor.newInstance(services);
        List<IBinder> observed = new ArrayList<>();
        Token token = new Token();
        IServiceCallback callback = new IServiceCallback.Stub() {
            @Override public IBinder asBinder() { return token; }
            @Override public void onRegistration(String name, IBinder binder) {
                require("present".equals(name) || "future".equals(name), "wrong registration name");
                observed.add(binder);
            }
        };
        manager.registerForNotifications("present", callback);
        require(observed.size() == 1 && observed.get(0) == first, "existing service not delivered");
        manager.registerForNotifications("present", callback);
        require(observed.size() == 1 && token.recipients.size() == 1, "duplicate observer retained");
        manager.registerForNotifications("future", callback);
        require(observed.size() == 1, "missing service fabricated a callback");
        Binder second = new Binder();
        manager.addService("future", second, false, 0);
        require(observed.size() == 2 && observed.get(1) == second, "future service not delivered");
        manager.unregisterForNotifications("future", callback);
        manager.addService("future", new Binder(), false, 0);
        require(observed.size() == 2 && token.recipients.size() == 1,
                "unregister retained a subscription or removed a different name");
        token.die();
        manager.addService("present", new Binder(), false, 0);
        require(observed.size() == 2 && token.recipients.isEmpty(), "dead callback retained");

        Token broken = new Token();
        manager.registerForNotifications("present", new IServiceCallback.Stub() {
            @Override public IBinder asBinder() { return broken; }
            @Override public void onRegistration(String name, IBinder binder) throws RemoteException {
                throw new RemoteException("test transport failure");
            }
        });
        require(broken.recipients.isEmpty(), "failed callback retained its death recipient");
        Token localFailure = new Token();
        manager.registerForNotifications("present", new IServiceCallback.Stub() {
            @Override public IBinder asBinder() { return localFailure; }
            @Override public void onRegistration(String name, IBinder binder) {
                throw new IllegalStateException("test local observer failure");
            }
        });
        require(localFailure.recipients.isEmpty(), "failed local observer retained registration");
        final int[] reentrant = {0};
        manager.registerForNotifications("present", new IServiceCallback.Stub() {
            @Override public void onRegistration(String name, IBinder binder) throws RemoteException {
                manager.unregisterForNotifications(name, this);
                reentrant[0]++;
            }
        });
        manager.addService("present", new Binder(), false, 0);
        require(reentrant[0] == 1, "callback could not unregister itself");
        require(manager.getDeclaredInstances("missing").length == 0
                && manager.getUpdatableNames("missing").length == 0, "null service metadata arrays");

        // A replacement arriving while a local callback is running must not
        // invoke that observer concurrently or deliver an old snapshot later.
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch replaced = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Binder oldService = new Binder();
        Binder newService = new Binder();
        manager.addService("ordered", oldService, false, 0);
        IServiceCallback ordered = new IServiceCallback.Stub() {
            @Override public void onRegistration(String name, IBinder binder) {
                if (binder == oldService) {
                    entered.countDown();
                    try { require(release.await(2, TimeUnit.SECONDS), "callback not released"); }
                    catch (InterruptedException e) { throw new AssertionError(e); }
                } else {
                    require(binder == newService, "unexpected service identity");
                    replaced.countDown();
                }
            }
        };
        Thread registering = new Thread(() -> {
            try { manager.registerForNotifications("ordered", ordered); }
            catch (Throwable e) { failure.set(e); }
        });
        Thread replacing = new Thread(() -> {
            try { manager.addService("ordered", newService, false, 0); }
            catch (Throwable e) { failure.set(e); }
        });
        registering.start();
        require(entered.await(2, TimeUnit.SECONDS), "initial callback did not begin");
        replacing.start();
        boolean concurrent = replaced.await(100, TimeUnit.MILLISECONDS);
        release.countDown();
        registering.join(2000);
        replacing.join(2000);
        require(!registering.isAlive() && !replacing.isAlive(), "notification dispatch deadlocked");
        require(failure.get() == null, "notification dispatch failed: " + failure.get());
        require(!concurrent && replaced.getCount() == 0, "observer was invoked concurrently");

        // Two local observers can publish each other's service concurrently. A
        // callback monitor held across user code would deadlock these publishers.
        CountDownLatch bothEntered = new CountDownLatch(2);
        CountDownLatch bothReplaced = new CountDownLatch(2);
        Binder leftInitial = new Binder();
        Binder rightInitial = new Binder();
        Binder leftReplacement = new Binder();
        Binder rightReplacement = new Binder();
        manager.registerForNotifications("left", new IServiceCallback.Stub() {
            @Override public void onRegistration(String name, IBinder binder) throws RemoteException {
                if (binder == leftReplacement) { bothReplaced.countDown(); return; }
                require(binder == leftInitial, "unexpected left service");
                bothEntered.countDown();
                try { require(bothEntered.await(2, TimeUnit.SECONDS), "right observer did not start"); }
                catch (InterruptedException e) { throw new AssertionError(e); }
                manager.addService("right", rightReplacement, false, 0);
            }
        });
        manager.registerForNotifications("right", new IServiceCallback.Stub() {
            @Override public void onRegistration(String name, IBinder binder) throws RemoteException {
                if (binder == rightReplacement) { bothReplaced.countDown(); return; }
                require(binder == rightInitial, "unexpected right service");
                bothEntered.countDown();
                try { require(bothEntered.await(2, TimeUnit.SECONDS), "left observer did not start"); }
                catch (InterruptedException e) { throw new AssertionError(e); }
                manager.addService("left", leftReplacement, false, 0);
            }
        });
        Thread leftPublisher = new Thread(() -> {
            try { manager.addService("left", leftInitial, false, 0); }
            catch (Throwable e) { failure.set(e); }
        });
        Thread rightPublisher = new Thread(() -> {
            try { manager.addService("right", rightInitial, false, 0); }
            catch (Throwable e) { failure.set(e); }
        });
        leftPublisher.setDaemon(true);
        rightPublisher.setDaemon(true);
        leftPublisher.start();
        rightPublisher.start();
        leftPublisher.join(3000);
        rightPublisher.join(3000);
        require(!leftPublisher.isAlive() && !rightPublisher.isAlive(),
                "cross-observer publication deadlocked");
        require(failure.get() == null && bothReplaced.getCount() == 0,
                "cross-observer publication failed: " + failure.get());

        Binder stale = new Binder();
        Binder latest = new Binder();
        List<IBinder> latestObserved = new ArrayList<>();
        manager.registerForNotifications("stale", new IServiceCallback.Stub() {
            @Override public void onRegistration(String name, IBinder binder) throws RemoteException {
                latestObserved.add(binder);
                if (binder == stale) {
                    manager.addService(name, latest, false, 0);
                    try { delayedSnapshot(manager, name, stale); }
                    catch (ReflectiveOperationException e) { throw new AssertionError(e); }
                }
            }
        });
        manager.addService("stale", stale, false, 0);
        require(latestObserved.size() == 2 && latestObserved.get(0) == stale
                && latestObserved.get(1) == latest, "stale snapshot discarded the latest publication");
        System.out.println("ServiceRegistrationCallbackTest all passed");
    }
}
