/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.content.Context;
import android.net.nsd.AdvertisingRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.IBinder;
import android.os.RemoteException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/** Genuine public manager/client policy; explicit app executor, service lookup and Binder lifetime. */
public final class NsdSubscriptionManagerClientTest {
    static int checks;
    static void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks++; }
    interface Action { void run() throws Exception; }
    static void rejects(Class<? extends Throwable> type, Action work, String why) throws Exception {
        try { work.run(); } catch (Throwable error) { check(type.isInstance(error), why + ": " + error); return; }
        throw new AssertionError(why + ": no rejection");
    }
    static NsdServiceInfo info() {
        NsdServiceInfo value = new NsdServiceInfo("Owned callback", "_owned._tcp");
        value.setAttribute("binary", new byte[]{0, (byte)255}); value.setPort(41415); return value;
    }
    static final class Queue implements Executor, NsdSubscriptionClient.Scheduler {
        final ArrayDeque<Runnable> work = new ArrayDeque<>();
        public void execute(Runnable action) { work.add(action); }
        public void later(Runnable action, long delay) { check(delay == 1000, "reconnect spacing is bounded"); work.add(action); }
        void drain() { for (int n = 0; !work.isEmpty(); n++) { if (n > 10000) throw new AssertionError("unbounded queue"); work.remove().run(); } }
        void tick() { int n = work.size(); while (n-- > 0) work.remove().run(); }
    }
    static final class Listener implements NsdManager.ServiceInfoCallback, NsdManager.DiscoveryListener,
            NsdManager.ResolveListener, NsdManager.RegistrationListener, NsdSubscriptionClient.Callback {
        final List<String> events = new ArrayList<>(); NsdServiceInfo last;
        public void onServiceInfoCallbackRegistrationFailed(int error) { events.add("failed:" + error); }
        public void onServiceUpdated(NsdServiceInfo value) { last = value; events.add("updated"); }
        public void onServiceLost() { events.add("lost"); }
        public void onServiceInfoCallbackUnregistered() { events.add("unregistered"); }
        public void updated(NsdServiceInfo value) { onServiceUpdated(value); }
        public void lost() { onServiceLost(); }
        public void failed(int error) { onServiceInfoCallbackRegistrationFailed(error); }
        public void unregistered() { onServiceInfoCallbackUnregistered(); }
        public void onStartDiscoveryFailed(String type, int error) { events.add("discoverFailed:" + error); }
        public void onStopDiscoveryFailed(String type, int error) {} public void onDiscoveryStarted(String type) {}
        public void onDiscoveryStopped(String type) {} public void onServiceFound(NsdServiceInfo info) {}
        public void onServiceLost(NsdServiceInfo info) {} public void onResolveFailed(NsdServiceInfo info, int error) {}
        public void onServiceResolved(NsdServiceInfo info) {} public void onRegistrationFailed(NsdServiceInfo info, int error) {}
        public void onUnregistrationFailed(NsdServiceInfo info, int error) {} public void onServiceRegistered(NsdServiceInfo info) {}
        public void onServiceUnregistered(NsdServiceInfo info) {}
    }
    static final class Factory implements NsdSubscriptionClient.Factory {
        final List<Node> nodes = new ArrayList<>(); Object lock; boolean inlineUpdate;
        final class Node implements NsdSubscriptionClient.Session {
            final NsdServiceInfo selector; final NsdSubscriptionClient.Callback callback; int stops;
            Node(NsdServiceInfo selector, NsdSubscriptionClient.Callback callback) { this.selector = selector; this.callback = callback; }
            public void stop() { check(!Thread.holdsLock(lock), "stop outside public listener lock"); stops++; callback.unregistered(); }
        }
        public NsdSubscriptionClient.Session start(NsdServiceInfo value, NsdSubscriptionClient.Callback callback) {
            check(!Thread.holdsLock(lock), "start outside public listener lock"); Node node = new Node(value, callback); nodes.add(node);
            if (inlineUpdate) callback.updated(info()); return node;
        }
    }
    static NsdManager manager(Executor handler, Factory factory, int[] registrationOther) throws Exception {
        Constructor<NsdManager> constructor = NsdManager.class.getDeclaredConstructor(Context.class, Executor.class,
                WindowsNsd.Factory.class, WindowsNsdResolver.Factory.class, NsdRegistrationClient.Factory.class,
                NsdSubscriptionClient.Factory.class); constructor.setAccessible(true);
        NsdManager value = constructor.newInstance(null, handler, new WindowsNsd.Factory() {
            public WindowsNsd.Session start(String type, String subtype, WindowsNsd.Scope scope, WindowsNsd.Callback callback) {
                throw new AssertionError("unexpected real browse selection");
            }
            public int[] interfaces() { throw new AssertionError("unexpected real scope selection"); }
        }, (WindowsNsdResolver.Factory)(request, callback) -> { throw new AssertionError("unexpected resolve selection"); },
                (NsdRegistrationClient.Factory)(request, other, callback) -> {
                    registrationOther[0] = other; return new NsdRegistrationClient.Session() {
                        public void update(AdvertisingRequest value, int count) {}
                        public void stop() { callback.unregistered(); }
                    };
                }, factory);
        Field lock = NsdManager.class.getDeclaredField("mDiscoveries"); lock.setAccessible(true); factory.lock = lock.get(value); return value;
    }
    static void managerContracts() throws Exception {
        Queue handler = new Queue(), app = new Queue(); Factory factory = new Factory(); int[] other = {-1};
        NsdManager manager = manager(handler, factory, other); Listener listener = new Listener(); NsdServiceInfo selector = info();
        manager.registerServiceInfoCallback(selector, app, listener); Factory.Node node = factory.nodes.get(0);
        selector.getAttributes().get("binary")[0] = 11; check(node.selector.getAttributes().get("binary")[0] == 0, "selector owns raw bytes before handoff");
        rejects(IllegalArgumentException.class, () -> manager.registerServiceInfoCallback(info(), app, listener), "same callback identity rejected");
        rejects(IllegalArgumentException.class, () -> manager.discoverServices("_owned._tcp", 1, listener), "cross-kind discovery identity rejected");
        rejects(IllegalArgumentException.class, () -> manager.resolveService(info(), listener), "cross-kind resolution identity rejected");
        rejects(IllegalArgumentException.class, () -> manager.registerService(info(), 1, listener), "cross-kind registration identity rejected");
        node.callback.updated(info()); check(listener.events.isEmpty(), "service dispatch precedes app executor"); handler.drain();
        check(listener.events.isEmpty(), "app executor owns callback delivery"); app.drain(); check(listener.events.equals(List.of("updated")), "complete update delivered");
        node.callback.updated(info()); node.callback.lost(); handler.drain(); manager.unregisterServiceInfoCallback(listener);
        check(node.stops == 1, "explicit cancel owns one actual stop"); manager.unregisterServiceInfoCallback(listener); check(node.stops == 1, "duplicate cancel idempotent");
        handler.drain(); app.drain(); check(listener.events.equals(List.of("updated", "unregistered")), "cancel suppresses queued update and loss at execution time");
        rejects(IllegalArgumentException.class, () -> manager.unregisterServiceInfoCallback(listener), "terminal callback identity removed");
        manager.registerServiceInfoCallback(info(), app, listener); node.callback.updated(info()); handler.drain(); app.drain();
        check(listener.events.size() == 2, "old generation cannot reach reused listener");
        manager.unregisterServiceInfoCallback(listener); handler.drain(); app.drain();
        Listener rejected = new Listener(); manager.registerServiceInfoCallback(info(), command -> { throw new RejectedExecutionException("explicit fixture"); }, rejected);
        Factory.Node denied = factory.nodes.get(2); denied.callback.updated(info());
        rejects(RejectedExecutionException.class, handler::drain, "application executor rejection remains visible");
        check(denied.stops == 1, "rejected executor retires real ownership"); handler.drain();
        manager.registerServiceInfoCallback(info(), Runnable::run, rejected); manager.unregisterServiceInfoCallback(rejected); handler.drain();
        Factory early = new Factory(); early.inlineUpdate = true; NsdManager direct = manager(Runnable::run, early, new int[1]);
        NsdManager.ServiceInfoCallback callback = new NsdManager.ServiceInfoCallback() {
            public void onServiceInfoCallbackRegistrationFailed(int error) { throw new AssertionError(error); }
            public void onServiceUpdated(NsdServiceInfo info) { direct.unregisterServiceInfoCallback(this); }
            public void onServiceLost() {} public void onServiceInfoCallbackUnregistered() {}
        };
        direct.registerServiceInfoCallback(info(), Runnable::run, callback); check(early.nodes.get(0).stops == 1, "synchronous callback cancel survives session handoff");
        Factory many = new Factory(); NsdManager quota = manager(handler, many, other);
        for (int i = 0; i < 201; i++) quota.registerServiceInfoCallback(info(), Runnable::run, new Listener());
        check(many.nodes.size() == 201, "no invented public subscription admission quota");
        Listener discovery = new Listener(); quota.discoverServices("_owned._tcp", 1, discovery); handler.drain();
        check(discovery.events.equals(List.of("discoverFailed:4")), "subscriptions count in shared discovery quota");
        quota.registerService(info(), 1, new Listener()); check(other[0] == 201, "subscriptions count in registration admission");
    }

    static final class Remote implements INsdSubscription, InvocationHandler {
        final IBinder binder = (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class}, this);
        final List<IBinder.DeathRecipient> deaths = new ArrayList<>();
        final Map<Integer, INsdSubscription.IClient> requests = new LinkedHashMap<>();
        boolean alive = true, ready = true, deferStop, ambiguousOnce;
        SecurityException permissionDenied;
        int registerCalls, unregisterCalls; int lastId; INsdSubscription.IClient lastToken;
        public IBinder asBinder() { return binder; }
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            switch (method.getName()) {
                case "equals": return proxy == args[0]; case "hashCode": return System.identityHashCode(proxy);
                case "toString": return "Owned subscription fixture remote";
                case "queryLocalInterface": return this; case "isBinderAlive": case "pingBinder": return alive;
                case "linkToDeath": deaths.add((IBinder.DeathRecipient)args[0]); return null;
                case "unlinkToDeath": return deaths.remove(args[0]);
                default: throw new AssertionError(method);
            }
        }
        public void register(INsdSubscription.IClient token, int id, NsdServiceInfo selector) throws RemoteException {
            registerCalls++; lastId = id; lastToken = token;
            if (permissionDenied != null) throw permissionDenied; // Actual authority rejects before admission.
            requests.putIfAbsent(id, token);
            if (ambiguousOnce) { ambiguousOnce = false; throw new RemoteException("accepted transaction with lost reply"); }
            if (ready) token.event(id, INsdSubscription.READY, 0, 0, null);
        }
        public void unregister(INsdSubscription.IClient token, int id) throws RemoteException {
            unregisterCalls++; if (deferStop) return;
            if (requests.remove(id) != null) token.event(id, INsdSubscription.UNREGISTERED, 0, 0, null);
        }
        void event(int kind, long scope) throws RemoteException { lastToken.event(lastId, kind, 0, scope, kind == INsdSubscription.UPDATED ? info() : null); }
        void die() { alive = false; for (IBinder.DeathRecipient death : new ArrayList<>(deaths)) death.binderDied(); }
    }
    static void clientContracts() throws Exception {
        Queue retry = new Queue(); Remote[] selected = {null};
        NsdSubscriptionClient.Factory factory = NsdSubscriptionClient.withConnector(() -> selected[0] == null ? null : selected[0].binder, retry);
        Listener missing = new Listener(); factory.start(info(), missing);
        check(missing.events.equals(List.of("failed:0")) && retry.work.isEmpty(), "missing initial owner produces one initial failure");
        Remote first = new Remote(); selected[0] = first; Listener listener = new Listener();
        NsdSubscriptionClient.Session session = factory.start(info(), listener);
        check(listener.events.isEmpty(), "private readiness is not a public success callback");
        int firstId = first.lastId; INsdSubscription.IClient firstToken = first.lastToken;
        first.event(INsdSubscription.LOST, 99); check(listener.events.isEmpty(), "loss before observed update suppressed");
        first.event(INsdSubscription.UPDATED, 11); first.event(INsdSubscription.UPDATED, 12); first.die();
        check(listener.events.equals(List.of("updated", "updated", "lost", "lost")), "owner death loses exactly previously observed scopes without registration failure");
        Remote second = new Remote(); selected[0] = second; retry.tick();
        check(second.lastId > firstId && second.lastToken != firstToken, "replacement has fresh token and monotonic wire ID");
        firstToken.event(firstId, INsdSubscription.UPDATED, 0, 11, info()); check(listener.events.size() == 4, "late old connection callback suppressed");
        second.event(INsdSubscription.UPDATED, 11); second.event(INsdSubscription.LOST, 11); second.event(INsdSubscription.LOST, 11);
        check(listener.events.subList(4, listener.events.size()).equals(List.of("updated", "lost")), "reappearance and deduplicated observed loss");
        session.stop(); check(listener.events.get(listener.events.size() - 1).equals("unregistered") && second.deaths.isEmpty(), "actual cancellation completion releases idle connection");
        Remote pending = new Remote(); pending.deferStop = true; selected[0] = pending; Listener canceled = new Listener();
        NsdSubscriptionClient.Session stop = factory.start(info(), canceled); pending.event(INsdSubscription.UPDATED, 1); stop.stop();
        pending.event(INsdSubscription.UPDATED, 1); pending.die();
        check(canceled.events.equals(List.of("updated")), "owner death during cancel fabricates neither unregister success nor initial failure");
        Remote replacement = new Remote(); selected[0] = replacement; retry.tick();
        check(canceled.events.equals(List.of("updated", "unregistered")) && replacement.unregisterCalls == 1, "pending cancel recovers through actual new-owner stop");
        Remote ambiguous = new Remote(); ambiguous.ambiguousOnce = true; selected[0] = ambiguous; Listener ambiguousListener = new Listener();
        NsdSubscriptionClient.Session ambiguousSession = factory.start(info(), ambiguousListener); int id = ambiguous.lastId; INsdSubscription.IClient token = ambiguous.lastToken;
        check(ambiguousListener.events.isEmpty(), "ambiguous accepted live transaction does not manufacture failure"); retry.tick();
        check(ambiguous.registerCalls == 2 && ambiguous.lastId == id && ambiguous.lastToken == token && ambiguous.requests.size() == 1,
                "ambiguous live retry retains exact token/ID and one owner"); ambiguousSession.stop();
        Remote notReady = new Remote(); notReady.ready = false; selected[0] = notReady; Listener initial = new Listener(); factory.start(info(), initial); notReady.die();
        check(initial.events.equals(List.of("failed:0")), "actual owner death before readiness remains initial failure");
        Queue deniedRetry = new Queue(); Remote denied = new Remote(); Listener deniedListener = new Listener();
        SecurityException refusal = new SecurityException("owned fixture INTERNET denial"); denied.permissionDenied = refusal;
        NsdSubscriptionClient.Factory deniedFactory = NsdSubscriptionClient.withConnector(() -> denied.binder, deniedRetry);
        try { deniedFactory.start(info(), deniedListener); throw new AssertionError("permission denial missing"); }
        catch (SecurityException expected) { check(expected == refusal, "definite initial denial preserves original synchronous exception"); }
        check(deniedFactory.retainedCount() == 0 && denied.requests.isEmpty(), "denied admission retains no local or remote request");
        check(denied.deaths.isEmpty() && deniedRetry.work.isEmpty(), "denied admission unlinks idle death owner and schedules no retry");
        for (int i = 0; i < 3; ++i) deniedRetry.tick();
        check(denied.registerCalls == 1 && denied.unregisterCalls == 0 && deniedListener.events.isEmpty(),
                "finite later ticks neither reenroll denied request nor fabricate callback");
        denied.permissionDenied = null;
        NsdSubscriptionClient.Session allowed = deniedFactory.start(info(), deniedListener);
        check(denied.registerCalls == 2 && denied.requests.size() == 1 && denied.deaths.size() == 1,
                "subsequent permitted request establishes a fresh real owner");
        allowed.stop();
        check(deniedListener.events.equals(List.of("unregistered")) && denied.deaths.isEmpty()
                && deniedFactory.retainedCount() == 0, "subsequent permitted request cancels and releases ownership");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); managerContracts(); clientContracts();
        System.out.println("NSD_SUBSCRIPTION_MANAGER_CLIENT_OK checks=" + checks + " windows=false remote_binder=false");
    }
}
