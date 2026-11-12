/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.IContentService;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.IBinder;
import android.os.RemoteException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Production observer lifecycle with controlled Binder death, without a sync backend. */
public final class ContentObserverLifecycleTest {
    private static final Uri URI = Uri.parse("content://gesher.observer/items");
    public static int checks;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static Field field(Class<?> owner, String name) throws Exception {
        Field field = owner.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static IContentService service() throws Exception {
        check(!com.gesher.server.GmsSignatureSpoofing.isEnabled(),
                "Lifecycle fixture requires the ordinary desktop policy; permission fixtures run separately");
        Unsafe unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        Class<?> type = Class.forName("com.gesher.server.ContentService");
        // Only the sync/process constructor dependencies are absent. The actual
        // production collection, registration, delivery and removal methods run.
        Object service = unsafe.allocateInstance(type);
        field(type, "mObservers").set(service, new ArrayList<>());
        return (IContentService)service;
    }

    @SuppressWarnings("unchecked")
    private static List<Object> registry(IContentService service) throws Exception {
        return (List<Object>)field(service.getClass(), "mObservers").get(service);
    }

    private static int registered(IContentService service) throws Exception {
        List<Object> registrations = registry(service);
        synchronized (registrations) { return registrations.size(); }
    }

    private static final class Token implements InvocationHandler {
        final IBinder binder = (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(),
                new Class<?>[]{IBinder.class}, this);
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        final Object registry;
        boolean dead, inlineDeath, concurrentDeath, callUnderRegistryLock, equalityUnderRegistryLock;
        Token(Object registry) { this.registry = registry; }

        @Override public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            switch (method.getName()) {
                // A valid collision forces grouping to use canonical identity,
                // without executing a Binder's equality code under the lock.
                case "hashCode": return 7;
                case "equals":
                    equalityUnderRegistryLock |= Thread.holdsLock(registry);
                    return proxy == args[0];
                case "toString": return "ContentLifecycleToken";
                case "linkToDeath":
                    callUnderRegistryLock |= Thread.holdsLock(registry);
                    synchronized (this) {
                        if (dead) throw new RemoteException("Observer already dead");
                        recipients.add((IBinder.DeathRecipient)args[0]);
                    }
                    if (inlineDeath) die();
                    if (concurrentDeath) {
                        AtomicReference<Throwable> failure = new AtomicReference<>();
                        Thread death = new Thread(() -> {
                            try { die(); } catch (Throwable problem) { failure.set(problem); }
                        }, "content-observer-death-before-publication");
                        death.setDaemon(true);
                        death.start();
                        death.join(3000);
                        check(!death.isAlive() && failure.get() == null,
                                "Death callback blocked behind registration: " + failure.get());
                    }
                    return null;
                case "unlinkToDeath":
                    callUnderRegistryLock |= Thread.holdsLock(registry);
                    synchronized (this) { return recipients.remove(args[0]); }
                case "isBinderAlive": case "pingBinder": return !dead;
                case "queryLocalInterface": return null;
                default: throw new UnsupportedOperationException(method.toString());
            }
        }

        void die() {
            List<IBinder.DeathRecipient> snapshot;
            synchronized (this) { dead = true; snapshot = new ArrayList<>(recipients); }
            for (IBinder.DeathRecipient recipient : snapshot) recipient.binderDied();
        }
        synchronized int links() { return recipients.size(); }
    }

    private static final class Observer implements IContentObserver {
        final Token token;
        int calls;
        boolean failLocally, failRemotely, deliveredUnderLock;
        Runnable action;
        Observer(Object registry) { token = new Token(registry); }
        @Override public IBinder asBinder() { return token.binder; }
        @Override public void onChange(boolean self, Uri uri, int user) throws RemoteException {
            onChangeEtc(self, new Uri[]{uri}, 0, user);
        }
        @Override public void onChangeEtc(boolean self, Uri[] uris, int flags, int user)
                throws RemoteException {
            calls++;
            deliveredUnderLock |= Thread.holdsLock(token.registry);
            if (action != null) action.run();
            if (failRemotely) throw new RemoteException("Expected remote observer failure");
            if (failLocally) throw new IllegalStateException("Expected local observer failure");
        }
    }

    private static void add(IContentService service, Observer observer, Uri uri) throws Exception {
        service.registerContentObserver(uri, true, observer, 0, 36);
    }

    private static void notify(IContentService service) throws Exception {
        service.notifyChange(new Uri[]{URI}, null, false, 0, 0, 36, null);
    }

    public static void contracts() throws Exception {
        // The callback completes before linkToDeath returns, on either the
        // registration thread or an independent Binder-death thread.
        for (boolean concurrent : new boolean[]{false, true}) {
            IContentService service = service();
            Observer observer = new Observer(registry(service));
            observer.token.inlineDeath = !concurrent;
            observer.token.concurrentDeath = concurrent;
            add(service, observer, URI);
            check(registered(service) == 0, "Dead observer was published after its death callback");
            check(observer.token.links() == 0, "Retired registration retained a death recipient");
            notify(service);
            check(observer.calls == 0, "Change delivered to an observer retired before publication");
            check(!observer.token.callUnderRegistryLock, "Binder lifetime call ran under observer lock");
        }

        IContentService service = service();
        Observer gone = new Observer(registry(service));
        gone.token.dead = true;
        add(service, gone, URI);
        check(registered(service) == 0 && gone.token.links() == 0,
                "Already-dead observer retained state");

        Observer live = new Observer(registry(service));
        add(service, live, URI);
        notify(service);
        check(live.calls == 1 && !live.deliveredUnderLock, "Live observer delivery failed or held registry lock");
        service.unregisterContentObserver(live);
        check(registered(service) == 0 && live.token.links() == 0, "Unregistration retained owned state");
        check(!live.token.callUnderRegistryLock, "Unlink ran under observer registry lock");
        notify(service);
        check(live.calls == 1, "Removed observer received a later change");

        Observer repeated = new Observer(registry(service));
        add(service, repeated, URI);
        add(service, repeated, Uri.withAppendedPath(URI, "child"));
        check(registered(service) == 2 && repeated.token.links() == 2, "Independent URI registrations were lost");
        repeated.token.die();
        repeated.token.die();
        check(registered(service) == 0 && repeated.token.links() == 0, "Death did not retire every URI registration once");

        Observer broken = new Observer(registry(service));
        Observer healthy = new Observer(registry(service));
        StringBuilder deliveryOrder = new StringBuilder();
        broken.action = () -> deliveryOrder.append('b');
        healthy.action = () -> deliveryOrder.append('h');
        broken.failLocally = true;
        add(service, broken, URI);
        add(service, healthy, URI);
        notify(service);
        notify(service);
        check(broken.calls == 2 && healthy.calls == 2, "Throwing local observer interrupted unrelated notifications");
        check("bhbh".contentEquals(deliveryOrder), "Notification grouping changed first-seen delivery order");
        check(registered(service) == 2, "Local callback exception silently unregistered its observer");
        service.unregisterContentObserver(broken);
        service.unregisterContentObserver(healthy);

        Observer remoteFailure = new Observer(registry(service));
        remoteFailure.failRemotely = true;
        add(service, remoteFailure, URI);
        notify(service);
        check(registered(service) == 0 && remoteFailure.token.links() == 0,
                "Remote callback failure retained observer/death ownership");

        Observer reentrant = new Observer(registry(service));
        reentrant.action = () -> {
            try { service.unregisterContentObserver(reentrant); }
            catch (RemoteException failure) { throw new AssertionError(failure); }
        };
        add(service, reentrant, URI);
        notify(service);
        notify(service);
        check(reentrant.calls == 1 && registered(service) == 0 && reentrant.token.links() == 0,
                "Reentrant unregistration retained callback ownership");

        // A death callback already captured by Binder may arrive after unlink.
        // Its retired registration cannot remove a later use of the same token.
        Observer reused = new Observer(registry(service));
        add(service, reused, URI);
        List<IBinder.DeathRecipient> staleDeaths;
        synchronized (reused.token) { staleDeaths = new ArrayList<>(reused.token.recipients); }
        service.unregisterContentObserver(reused);
        add(service, reused, URI);
        for (IBinder.DeathRecipient recipient : staleDeaths) recipient.binderDied();
        check(registered(service) == 1 && reused.token.links() == 1,
                "Late retired death callback removed a replacement registration");
        notify(service);
        check(reused.calls == 1, "Replacement registration stopped receiving changes after a late death callback");
        service.unregisterContentObserver(reused);

        // Delivery uses a snapshot. A local observer can replace its registration
        // before that old delivery reports a RemoteException.
        Observer replacement = new Observer(registry(service));
        replacement.action = () -> {
            replacement.action = null;
            try {
                service.unregisterContentObserver(replacement);
                add(service, replacement, URI);
            } catch (Exception failure) { throw new AssertionError(failure); }
        };
        replacement.failRemotely = true;
        add(service, replacement, URI);
        notify(service);
        replacement.failRemotely = false;
        check(registered(service) == 1 && replacement.token.links() == 1,
                "Old remote callback failure removed a replacement registration");
        notify(service);
        check(replacement.calls == 2, "Replacement registration stopped receiving changes after an old remote failure");
        service.unregisterContentObserver(replacement);
        check(registered(service) == 0 && replacement.token.links() == 0,
                "Replacement registration cleanup retained owned state");
        check(!live.token.equalityUnderRegistryLock && !reused.token.equalityUnderRegistryLock
                        && !replacement.token.equalityUnderRegistryLock,
                "Binder equality callback ran under observer registry lock");
        check(!broken.token.equalityUnderRegistryLock && !healthy.token.equalityUnderRegistryLock,
                "Binder equality callback ran under registry lock during notification grouping");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        android.system.Os.unsetenv("GESHER_MICROG_SPOOF");
        contracts();
        System.out.println("ContentObserverLifecycleTest all passed: " + checks + " checks");
    }
}
