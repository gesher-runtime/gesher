/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.IBinder;
import android.os.RemoteException;
import com.gesher.net.INsdSubscription;
import com.gesher.net.WindowsNsdSubscriber;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Real process/package/permission policy with explicit Binder, dispatch and backend boundaries. */
public final class NsdSubscriptionServiceHost {
    static int checks;
    static void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks++; }
    static Object get(Object object, String name) throws Exception { return NsdRegistrationServiceHost.get(object, name); }
    static void set(Object object, String name, Object value) throws Exception { NsdRegistrationServiceHost.set(object, name, value); }
    static final class Dispatch implements NsdRegistrationService.Dispatcher {
        final ArrayDeque<Runnable> jobs = new ArrayDeque<>(), delayed = new ArrayDeque<>();
        int rejects;
        public synchronized void execute(Runnable action) {
            if (rejects > 0) { rejects--; throw new RejectedExecutionException("Explicit finite intake rejection"); }
            jobs.add(action);
        }
        public synchronized void later(Runnable action, long delay) {
            check(delay == 1000, "retirement retry has one-second spacing"); delayed.add(action);
        }
        void drain() {
            for (int count = 0; count < 10000; count++) {
                Runnable job; synchronized (this) { job = jobs.poll(); }
                if (job == null) return; job.run();
            }
            throw new AssertionError("dispatch did not settle");
        }
        void retry() { synchronized (this) { jobs.addAll(delayed); delayed.clear(); } drain(); }
    }
    record Event(int id, int kind, int error, long scope, NsdServiceInfo info) {}
    static final class Client implements INsdSubscription.IClient {
        final NsdRegistrationServiceHost.Token token;
        final List<Event> events = new ArrayList<>();
        boolean fail;
        Client(Object lock) { token = new NsdRegistrationServiceHost.Token(lock); }
        public IBinder asBinder() { return token.binder; }
        public void event(int id, int kind, int error, long scope, NsdServiceInfo info) throws RemoteException {
            check(!Thread.holdsLock(token.serviceLock), "no callback under intake lock");
            if (fail) throw new RemoteException("Explicit callback death");
            events.add(new Event(id, kind, error, scope, info));
        }
        Event last() { return events.get(events.size() - 1); }
    }
    static final class Backend implements WindowsNsdSubscriber.Backend {
        final Map<Integer, WindowsNsdSubscriber.Callback> active = new LinkedHashMap<>();
        final Map<Integer, Runnable> stopping = new LinkedHashMap<>();
        final List<Integer> withdrawn = new ArrayList<>();
        int starts, withdrawRejects, stops;
        boolean deferStop;
        NsdServiceInfo selector;
        public void register(int id, NsdServiceInfo info, WindowsNsdSubscriber.Callback callback) {
            starts++; selector = info; active.put(id, callback);
        }
        public void unregister(int id, Runnable done) {
            stops++; active.remove(id);
            if (deferStop) stopping.put(id, done); else done.run();
        }
        public void withdraw(int id) {
            if (withdrawRejects-- > 0) throw new RejectedExecutionException("Explicit cleanup rejection");
            active.remove(id); stopping.remove(id); withdrawn.add(id);
        }
        public long nextDueElapsed() { return active.isEmpty() ? Long.MAX_VALUE : 1234; }
        public int[] nativeState() { return new int[]{1, 2, 2, 0, 1, 17}; }
        int onlyId() { check(active.size() == 1, "one actual fixture backend owner"); return active.keySet().iterator().next(); }
        WindowsNsdSubscriber.Callback only() { return active.get(onlyId()); }
        void finish(int id) { stopping.remove(id).run(); }
    }
    static final class Harness {
        final NsdRegistrationServiceHost.Harness base = new NsdRegistrationServiceHost.Harness();
        final Dispatch dispatch = new Dispatch();
        final Backend backend = new Backend();
        final NsdRegistrationService.ProcessAuthority authority;
        final NsdSubscriptionService service;
        final Object lock;
        Harness() throws Exception {
            authority = new NsdRegistrationService.ProcessAuthority(base.pm, base.atm, base.am.callers(), false);
            service = new NsdSubscriptionService(authority, backend, dispatch);
            base.am.onProcessDeath(service::processDied); base.pm.onPackageRemoval(service::packageRemoved);
            lock = get(service, "lock");
        }
        Client client() { return new Client(lock); }
        void register(Client client, int id, NsdServiceInfo selector) throws Exception {
            NsdRegistrationServiceHost.as(base.process.pid, base.process.uid, () -> service.register(client, id, selector));
        }
        void unregister(Client client, int id) throws Exception {
            NsdRegistrationServiceHost.as(base.process.pid, base.process.uid, () -> service.unregister(client, id));
        }
        int clients() throws Exception { synchronized (lock) { return ((Map<?, ?>)get(service, "clients")).size(); } }
    }
    static NsdServiceInfo info() { return NsdRegistrationServiceHost.info(); }

    static void ownership() throws Exception {
        Harness h = new Harness(); Client client = h.client(); NsdServiceInfo input = info();
        h.register(client, 1, input); input.getAttributes().get("opaque")[2] = 99;
        check(h.backend.starts == 0 && client.token.links == 1, "strong owner enrolled before queued backend");
        h.dispatch.drain(); int id = h.backend.onlyId(); WindowsNsdSubscriber.Callback callback = h.backend.only();
        check(h.backend.selector.getAttributes().get("opaque")[2] == 8, "owned binary selector snapshot");
        callback.ready(); callback.updated(11, info()); h.dispatch.drain();
        check(client.events.size() == 2 && client.last().kind == INsdSubscription.UPDATED && client.last().scope == 11,
                "private readiness then complete scope update");
        h.register(client, 1, input); h.dispatch.drain();
        check(h.backend.starts == 1 && client.last().kind == INsdSubscription.READY, "ambiguous enrollment retry is idempotent");
        int before = client.events.size(); callback.failed(NsdManager.FAILURE_INTERNAL_ERROR); h.dispatch.drain();
        check(client.events.size() == before, "established subscription never gets new registration failure");
        h.backend.deferStop = true; h.unregister(client, 1); callback.updated(11, info()); h.dispatch.drain();
        check(client.events.size() == before && h.backend.stopping.containsKey(id), "cancel suppresses late events before real native retirement");
        h.unregister(client, 1); h.dispatch.drain(); check(h.backend.stops == 1, "repeated cancel retains one completion");
        h.backend.finish(id); h.dispatch.drain();
        check(client.last().kind == INsdSubscription.UNREGISTERED && h.clients() == 0 && client.token.deaths.isEmpty(), "completion follows actual backend release and drops idle token");
        callback.updated(11, info()); h.dispatch.drain(); check(client.events.size() == before + 1, "old backend callback cannot reach reused owner");
        h.register(client, 2, info()); h.dispatch.drain(); check(h.backend.onlyId() > id, "backend IDs never reused");
        client.token.die(); h.dispatch.drain(); check(h.clients() == 0 && h.backend.active.isEmpty(), "callback death withdraws native owner");
    }

    static void admissionAndRevocation() throws Exception {
        Harness h = new Harness(); Client client = h.client();
        NsdRegistrationServiceHost.rejects(SecurityException.class, () -> NsdRegistrationServiceHost.as(
                h.base.process.pid, h.base.process.uid + 1, () -> h.service.register(client, 1, info())), "wrong authenticated UID denied");
        h.dispatch.rejects = 1;
        NsdRegistrationServiceHost.rejects(RejectedExecutionException.class, () -> h.register(client, 1, info()), "finite rejected intake");
        check(h.clients() == 0 && (int)get(h.service, "pendingAdmissions") == 0 && client.token.deaths.isEmpty(), "rejected intake releases reservation and death recipient");
        client.token.rejectLink = true;
        NsdRegistrationServiceHost.rejects(android.os.ServiceSpecificException.class,
                () -> h.register(client, 2, info()), "real rejected callback link remains distinguishable");
        check((int)get(h.service, "pendingAdmissions") == 0, "link exception releases global admission");
        client.token.rejectLink = false;
        h.register(client, 3, info()); h.dispatch.drain(); WindowsNsdSubscriber.Callback retained = h.backend.only();
        h.backend.withdrawRejects = 2; h.base.die(h.base.process); h.dispatch.drain();
        check(h.clients() == 0 && h.backend.active.size() == 1, "dead app references retired while backend cleanup stays owned");
        retained.updated(1, info()); h.dispatch.drain(); check(client.events.isEmpty(), "dead process cannot be kept alive by callback token");
        h.dispatch.retry(); check(!h.backend.active.isEmpty(), "first failed withdrawal retains actual ID");
        h.dispatch.retry(); check(h.backend.active.isEmpty() && ((java.util.Set<?>)get(h.service, "withdrawals")).isEmpty(), "withdrawal eventually releases only retained ID");
        Harness failed = new Harness(); Client c = failed.client(); failed.register(c, 1, info()); failed.dispatch.drain();
        failed.backend.only().failed(6); failed.dispatch.drain();
        check(c.last().kind == INsdSubscription.FAILED && c.last().error == 6 && failed.clients() == 0,
                "initial native rejection remains initial API failure");
        Harness pending = new Harness(); Client p = pending.client(); pending.register(p, 1, info()); pending.unregister(p, 1);
        pending.dispatch.drain(); check(p.events.size() == 1 && p.last().kind == INsdSubscription.UNREGISTERED, "cancel while enrollment queued has one genuine completion");
    }

    static void intakeRace() throws Exception {
        Harness h = new Harness(); Client client = h.client(); h.register(client, 1, info()); h.dispatch.drain();
        int old = h.backend.onlyId(); h.backend.deferStop = true; h.unregister(client, 1); h.dispatch.drain();
        AtomicReference<NsdRegistrationService.Owner> owner = new AtomicReference<>();
        NsdRegistrationServiceHost.as(h.base.process.pid, h.base.process.uid, () -> owner.set(h.authority.capture()));
        Method acquire = NsdSubscriptionService.class.getDeclaredMethod("acquire", INsdSubscription.IClient.class, NsdRegistrationService.Owner.class);
        acquire.setAccessible(true);
        CountDownLatch acquired = new CountDownLatch(1), resume = new CountDownLatch(1);
        AtomicReference<Object> reservation = new AtomicReference<>(); AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread incoming = new Thread(() -> {
            try { reservation.set(acquire.invoke(h.service, client, owner.get())); acquired.countDown();
                if (!resume.await(3, TimeUnit.SECONDS)) throw new AssertionError("finite intake barrier"); }
            catch (Throwable error) { failure.set(error); acquired.countDown(); }
        }, "subscription-intake-reservation-fixture");
        incoming.start(); check(acquired.await(3, TimeUnit.SECONDS), "concurrent intake acquired actual reservation");
        h.backend.finish(old); h.dispatch.drain();
        check(h.clients() == 1 && client.token.unlinks == 0, "idle completion cannot remove client between acquisition and enrollment");
        resume.countDown(); incoming.join(3000); check(!incoming.isAlive() && failure.get() == null, "finite concurrent intake completed");
        Object reserved = reservation.get();
        Method enroll = NsdSubscriptionService.class.getDeclaredMethod("registerOnDispatcher", reserved.getClass(), int.class, NsdServiceInfo.class);
        enroll.setAccessible(true); enroll.invoke(h.service, reserved, 2, info());
        Field pending = reserved.getClass().getDeclaredField("pending"); pending.setAccessible(true);
        synchronized (h.lock) { pending.setInt(reserved, pending.getInt(reserved) - 1); }
        check(h.backend.starts == 2 && h.backend.onlyId() > old, "reserved client remains published for actual enrollment");
        h.unregister(client, 2); h.dispatch.drain(); int id = h.backend.stopping.keySet().iterator().next(); h.backend.finish(id); h.dispatch.drain();
        check(h.clients() == 0, "final release balances concurrent intake reservation");
    }

    static void backpressure() throws Exception {
        Harness h = new Harness(); Client client = h.client(); h.register(client, 1, info()); h.dispatch.drain();
        WindowsNsdSubscriber.Callback callback = h.backend.only(); AtomicInteger posted = new AtomicInteger();
        Thread producer = new Thread(() -> { for (int n = 0; n < 130; n++) { callback.updated(1, info()); posted.incrementAndGet(); } });
        producer.start(); long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (posted.get() < 128 && System.nanoTime() < end) Thread.sleep(2);
        check(posted.get() == 128 && (int)get(h.service, "queuedEvents") == 128, "bounded ordinary event queue applies producer backpressure");
        h.unregister(client, 1); producer.join(3000); h.dispatch.drain();
        check(!producer.isAlive() && (int)get(h.service, "queuedEvents") == 0, "cancel reservation wakes full producer queue");
        check(client.events.size() == 1 && client.last().kind == INsdSubscription.UNREGISTERED, "capacity never fabricates failure or loses stop completion");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // Unrelated locale settings need the declared host initialization seam;
        // no Android ICU data image is installed in the JVM fixture process.
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        Field hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        Field list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        ownership(); admissionAndRevocation(); intakeRace(); backpressure();
        System.out.println("NSD_SUBSCRIPTION_SERVICE_OK checks=" + checks + " windows=false remote_binder=false");
    }
}
