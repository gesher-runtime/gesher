/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.IApplicationThread;
import android.content.pm.PermissionInfo;
import android.net.nsd.AdvertisingRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceSpecificException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.net.INsdRegistration;
import com.gesher.net.NsdRegistrationClient;
import com.gesher.net.WindowsNsdAdvertiser;
import com.gesher.net.mdns.MdnsAdvertisingOptions;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.lang.ref.WeakReference;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.RejectedExecutionException;
import sun.misc.Unsafe;

/**
 * Real service/identity/permission policy with explicit host boundaries for
 * parsed installation metadata, native identity, Binder death, dispatch and the
 * protocol backend. This does not execute remote Binder or any network transport.
 */
public final class NsdRegistrationServiceHost {
    private static final Unsafe UNSAFE;
    private static int checks;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            UNSAFE = (Unsafe) field.get(null);
        } catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    interface Work { void run() throws Exception; }
    static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    static void rejects(Class<? extends Throwable> type, Work action, String reason) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            check(type.isInstance(failure), reason + ": " + failure);
            return;
        }
        throw new AssertionError(reason + ": no exception");
    }
    static void set(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }
    static Object get(Object target, String name) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }
    static final class Dispatch implements NsdRegistrationService.Dispatcher {
        final ArrayDeque<Runnable> jobs = new ArrayDeque<>();
        final ArrayDeque<Runnable> delayed = new ArrayDeque<>();
        public void execute(Runnable action) { jobs.add(action); }
        public void later(Runnable action, long delayMillis) {
            check(delayMillis == 1000, "Cleanup retry has a bounded one-second delay");
            delayed.add(action);
        }
        void retry() {
            int count = delayed.size();
            for (int i = 0; i < count; i++) jobs.add(delayed.remove());
            drain();
        }
        void drain() {
            for (int count = 0; !jobs.isEmpty(); count++) {
                if (count > 10000) throw new AssertionError("Dispatcher did not settle");
                jobs.remove().run();
            }
        }
    }
    static final class Token implements InvocationHandler {
        final IBinder binder = (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(),
                new Class<?>[]{IBinder.class}, this);
        final List<IBinder.DeathRecipient> deaths = new ArrayList<>();
        final Object serviceLock;
        boolean alive = true, dieInLink, rejectLink;
        Runnable afterLink;
        int links, unlinks;
        Token(Object serviceLock) { this.serviceLock = serviceLock; }
        public Object invoke(Object proxy, Method method, Object[] args) throws Exception {
            switch (method.getName()) {
                case "equals": return proxy == args[0];
                case "hashCode": return System.identityHashCode(proxy);
                case "toString": return "Owned NSD host callback token";
                case "isBinderAlive": case "pingBinder": return alive;
                case "linkToDeath":
                    check(!Thread.holdsLock(serviceLock), "Death link under service lock");
                    links++;
                    if (rejectLink) throw new RemoteException("Controlled link failure");
                    deaths.add((IBinder.DeathRecipient) args[0]);
                    if (dieInLink) die();
                    if (afterLink != null) afterLink.run();
                    return null;
                case "unlinkToDeath":
                    check(!Thread.holdsLock(serviceLock), "Death unlink under service lock");
                    unlinks++;
                    return deaths.remove(args[0]);
                default: throw new AssertionError("Unexpected host Binder operation " + method);
            }
        }
        void die() {
            alive = false;
            for (IBinder.DeathRecipient death : new ArrayList<>(deaths)) death.binderDied();
        }
    }
    record Event(int id, int event, int error, boolean retained, NsdServiceInfo info) {}
    static final class Client implements INsdRegistration.IClient {
        final Token token;
        final List<Event> events = new ArrayList<>();
        Work duringEvent;
        boolean failEvent;
        Client(Token token) { this.token = token; }
        public IBinder asBinder() { return token.binder; }
        public void event(int id, int event, int error, boolean retained, NsdServiceInfo info)
                throws RemoteException {
            check(!Thread.holdsLock(token.serviceLock), "Application callback under service lock");
            if (failEvent) throw new RemoteException("Controlled callback retirement");
            events.add(new Event(id, event, error, retained, info));
            if (duringEvent != null) {
                Work action = duringEvent;
                duringEvent = null;
                try { action.run(); }
                catch (Exception failure) { throw new AssertionError(failure); }
            }
        }
        Event last() { return events.get(events.size() - 1); }
    }
    static final class Pending {
        final int id, uid;
        final NsdServiceInfo info;
        final MdnsAdvertisingOptions options;
        WindowsNsdAdvertiser.Callback callback;
        Pending(int id, int uid, NsdServiceInfo info, MdnsAdvertisingOptions options,
                WindowsNsdAdvertiser.Callback callback) {
            this.id = id; this.uid = uid; this.info = info; this.options = options;
            this.callback = callback;
        }
    }
    static final class Backend implements WindowsNsdAdvertiser.Backend {
        final Map<Integer, Pending> active = new LinkedHashMap<>();
        final Map<Integer, Runnable> withdrawing = new HashMap<>();
        final List<Integer> removed = new ArrayList<>();
        final List<NsdServiceInfo> retiredValues = new ArrayList<>();
        boolean deferUnregister, failRegister, rejectUpdate;
        boolean diagnosticUnavailable;
        int diagnosticReads;
        int starts, updates;
        int rejectWithdrawals, withdrawalAttempts;
        NsdServiceInfo lastUpdate;
        public void register(int id, NsdServiceInfo info, MdnsAdvertisingOptions options,
                int uid, WindowsNsdAdvertiser.Callback callback) {
            starts++;
            if (failRegister) throw new IllegalStateException("Controlled backend construction failure");
            active.put(id, new Pending(id, uid, info, options, callback));
        }
        public void update(int id, NsdServiceInfo info, MdnsAdvertisingOptions options) {
            updates++;
            lastUpdate = info;
            if (rejectUpdate) active.get(id).callback.failed(NsdManager.FAILURE_INTERNAL_ERROR, true);
        }
        public void unregister(int id, Runnable done) {
            withdrawalAttempts++;
            if (rejectWithdrawals > 0) {
                rejectWithdrawals--;
                throw new RejectedExecutionException("Controlled withdrawal admission rejection");
            }
            removed.add(id);
            Pending old = active.remove(id);
            if (old != null) { old.callback = null; retiredValues.add(old.info); }
            if (deferUnregister) withdrawing.put(id, done);
            else done.run();
        }
        public void withdraw(int id) {
            if (withdrawing.containsKey(id)) return; // accepted logical stop already owns its slot
            unregister(id, () -> {});
        }
        Pending only() {
            if (active.size() != 1) throw new AssertionError("Expected one owned backend request");
            return active.values().iterator().next();
        }
        void finish(int id) { withdrawing.remove(id).run(); }
        public int[] nativeState() {
            diagnosticReads++;
            if (diagnosticUnavailable) throw new UnsupportedOperationException("Explicit host native boundary");
            return new int[]{1, 2, 2, 0, 1, 9};
        }
    }
    static ApkPackage code(String name, int uid, boolean internet) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + ".apk");
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(36);
        if (internet) parsed.addUsesPermission(new ParsedUsesPermissionImpl(
                Manifest.permission.INTERNET, 0, Set.of()));
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", name); set(code, "parsed", parsed); set(code, "mUid", uid);
        return code;
    }
    static final class Harness {
        final List<ActivityTaskManagerService.ProcessRecord> processes = new ArrayList<>();
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final ActivityManagerService am;
        final Dispatch dispatch = new Dispatch();
        final Backend backend = new Backend();
        final NsdRegistrationService service;
        final ApkPackage pkg;
        final ActivityTaskManagerService.ProcessRecord process;
        final Object lock;
        Harness() throws Exception {
            pkg = code("fixture.nsd.registration", 12351, true);
            pm = new PackageManagerService(List.of(pkg), false);
            PermissionPolicy permissions = pm.permissions();
            PermissionCatalog catalog = (PermissionCatalog) get(permissions, "mCatalog");
            PermissionInfo internet = new PermissionInfo();
            internet.name = Manifest.permission.INTERNET; internet.packageName = "android";
            internet.protectionLevel = PermissionInfo.PROTECTION_NORMAL;
            // Explicit parsed-framework-metadata boundary. PermissionPolicy still
            // checks the actual fixture manifest and authenticated PID/UID.
            set(catalog, "mFramework", new LinkedHashMap<>(Map.of(internet.name, internet)));
            set(catalog, "mLoaded", true);
            atm = (ActivityTaskManagerService) UNSAFE.allocateInstance(ActivityTaskManagerService.class);
            set(atm, "mPm", pm); set(atm, "mProcesses", processes);
            am = new ActivityManagerService(atm, pm, null);
            process = attach(861001, pkg);
            service = new NsdRegistrationService(
                    new NsdRegistrationService.ProcessAuthority(pm, atm, am.callers(), false),
                    backend, dispatch);
            Object observerLock = get(am, "mProcessDeathObservers");
            am.onProcessDeath(dead -> check(dead.dead && !Thread.holdsLock(atm)
                    && !Thread.holdsLock(observerLock), "Death observer ran before retirement or under a lifecycle lock"));
            am.onProcessDeath(service::processDied);
            pm.onPackageRemoval(service::packageRemoved);
            lock = get(service, "mLock");
        }
        ActivityTaskManagerService.ProcessRecord attach(int pid, ApkPackage pkg) throws Exception {
            final ActivityTaskManagerService.ProcessRecord[] result = new ActivityTaskManagerService.ProcessRecord[1];
            as(pid, pkg.uid(), () -> result[0] = atm.new ProcessRecord(pid, new IApplicationThread.Default()));
            result[0].packageName = pkg.packageName;
            result[0].packageCode = pkg;
            processes.add(result[0]);
            return result[0];
        }
        Client client() { return new Client(new Token(lock)); }
        void register(Client client, int id, AdvertisingRequest request, int other) throws Exception {
            as(process.pid, process.uid, () -> service.register(client, id, request, other));
        }
        void unregister(Client client, int id) throws Exception {
            as(process.pid, process.uid, () -> service.unregister(client, id));
        }
        void die(ActivityTaskManagerService.ProcessRecord app) {
            synchronized (atm) { app.dead = true; processes.remove(app); }
            am.processDied(app);
        }
        int clients() throws Exception { return ((Map<?, ?>) get(service, "mClients")).size(); }
    }
    static void as(int pid, int uid, Work work) throws Exception {
        long previous = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) uid << 32) | (pid & 0xffffffffL));
            work.run();
        } finally { Binder.restoreCallingIdentity(previous); }
    }
    static NsdServiceInfo info() {
        NsdServiceInfo info = new NsdServiceInfo("Owned service", "_grreg._tcp");
        info.setPort(34211);
        info.setAttribute("opaque", new byte[]{0, (byte) 0xff, 8});
        info.setPublicKey(new byte[]{0, 0, 3, 0, 77});
        return info;
    }
    static AdvertisingRequest request() { return new AdvertisingRequest.Builder(info()).build(); }
    static AdvertisingRequest update(NsdServiceInfo info) {
        return new AdvertisingRequest.Builder(info).setFlags(AdvertisingRequest.NSD_ADVERTISING_UPDATE_ONLY).build();
    }

    static void valuesAndWithdrawal() throws Exception {
        Harness h = new Harness(); Client c = h.client(); NsdServiceInfo supplied = info();
        h.register(c, 1, new AdvertisingRequest.Builder(supplied).build(), 0);
        supplied.getAttributes().get("opaque")[2] = 99;
        supplied.setPublicKey(new byte[]{0, 0, 3, 0, 88});
        check(h.backend.starts == 0 && c.token.deaths.size() == 1, "Owner must precede queued backend work");
        h.dispatch.drain(); Pending p = h.backend.only();
        check(p.uid == h.process.uid, "Queued backend lost captured calling UID");
        check(p.info.getAttributes().get("opaque")[2] == 8, "Local Binder TXT snapshot shares caller bytes");
        check(p.info.getPublicKey()[4] == 77, "Initial Binder snapshot lost/aliased the key");
        ApkPackage sibling = code("fixture.nsd.other", 12355, true);
        h.pm.add(sibling);
        ActivityTaskManagerService.ProcessRecord foreign = h.attach(861005, sibling);
        rejects(SecurityException.class, () -> as(foreign.pid, foreign.uid,
                () -> h.service.register(c, 9, request(), 0)), "A callback token cannot change its authenticated owner");
        check(h.backend.starts == 1, "Foreign callback claimant changed backend ownership");
        NsdServiceInfo registered = new NsdServiceInfo(p.info);
        registered.setServiceName("Owned service (2)"); registered.setHostname("actual-host");
        WindowsNsdAdvertiser.Callback late = p.callback;
        p.callback.registered(registered); h.dispatch.drain();
        Event event = c.last();
        check(event.event == INsdRegistration.REGISTERED && event.info.getServiceName().endsWith("(2)"), "Registered name callback");
        check(event.info.getServiceType() == null && event.info.getPort() == 0
                && event.info.getAttributes().isEmpty() && event.info.getNetwork() == null
                && "actual-host".equals(event.info.getHostname()), "Callback must expose the pinned minimal result");
        h.backend.deferUnregister = true;
        h.unregister(c, 1); h.dispatch.drain();
        check(c.events.size() == 1, "Unregister callback preceded logical withdrawal");
        late.registered(registered); h.dispatch.drain();
        check(c.events.size() == 1, "Late registration crossed a pending unregister");
        h.register(c, 2, request(), 0); h.dispatch.drain(); int next = h.backend.only().id;
        check(next != p.id, "Retired backend ID reused by new advertisement");
        h.backend.finish(p.id); h.dispatch.drain();
        check(c.last().event == INsdRegistration.UNREGISTERED && h.backend.active.containsKey(next), "Old withdrawal removed new owner");
        h.unregister(c, 2); h.dispatch.drain(); h.backend.finish(next); h.dispatch.drain();
        check(h.clients() == 0 && c.token.deaths.isEmpty(), "Idle client/token retained after withdrawal");
        late.registered(registered); h.dispatch.drain();
        check(c.events.size() == 3, "Retired callback reached a later connection");
    }

    static void admission() throws Exception {
        Harness h = new Harness(); Client c = h.client();
        NsdRegistrationService.ProcessAuthority collapsed =
                new NsdRegistrationService.ProcessAuthority(h.pm, h.atm, h.am.callers(), false);
        rejects(SecurityException.class, collapsed::capture,
                "Unassigned collapsed app must not acquire a system-owned lifetime");
        NsdRegistrationService.Owner server = new NsdRegistrationService.ProcessAuthority(
                h.pm, h.atm, h.am.callers(), true).capture();
        check(server.pkg == null && server.process == null
                && server.pid == android.os.Process.myPid(), "Actual local server profile owner");
        rejects(SecurityException.class, () -> as(h.process.pid, h.process.uid + 1,
                () -> h.service.register(c, 1, request(), 0)), "Wrong real UID");
        rejects(SecurityException.class, () -> as(861999, 1000,
                () -> h.service.register(c, 1, request(), 0)), "Unknown remote system-looking caller");
        ApkPackage denied = code("fixture.nsd.denied", 12352, false);
        h.pm.add(denied);
        ActivityTaskManagerService.ProcessRecord deniedProcess = h.attach(861002, denied);
        rejects(SecurityException.class, () -> as(deniedProcess.pid, deniedProcess.uid,
                () -> h.service.register(c, 1, request(), 0)), "INTERNET absent from actual manifest");
        for (String flag : List.of("dead", "exiting", "pidSuperseded")) {
            set(h.process, flag, true);
            rejects(SecurityException.class, () -> h.register(c, 1, request(), 0), flag + " owner admitted");
            set(h.process, flag, false);
        }
        ApkPackage replacement = code(h.pkg.packageName, h.pkg.uid(), true);
        h.pm.add(replacement);
        rejects(SecurityException.class, () -> h.register(c, 1, request(), 0), "Replaced package object admitted");
        h.pm.add(h.pkg);
        h.pm.beginPackageRemoval(h.pkg);
        rejects(SecurityException.class, () -> h.register(c, 1, request(), 0), "Removing package admitted");
        h.pm.endPackageRemoval(h.pkg);
        rejects(IllegalArgumentException.class, () -> h.register(c, 1, request(), -1), "Negative quota contribution");
        NsdServiceInfo zero = info(); zero.setPort(0); zero.setPublicKey(null);
        rejects(IllegalArgumentException.class, () -> h.register(c, 1,
                new AdvertisingRequest.Builder(zero).build(), 200), "Synchronous combination validation");
        check(h.backend.starts == 0 && c.token.links == 0, "Denied admission allocated a client/backend");
        h.register(c, 1, request(), Integer.MAX_VALUE); h.dispatch.drain();
        check(c.last().error == NsdManager.FAILURE_MAX_LIMIT && !c.last().retained
                && h.backend.starts == 0 && h.clients() == 0, "Combined quota overflow/admission cleanup");
        NsdServiceInfo malformedType = info(); malformedType.setServiceType("invalid-type");
        h.register(c, 2, new AdvertisingRequest.Builder(malformedType).build(), 200); h.dispatch.drain();
        check(c.last().error == NsdManager.FAILURE_MAX_LIMIT, "Quota must precede normalization");
        h.register(c, 3, new AdvertisingRequest.Builder(malformedType).build(), 0); h.dispatch.drain();
        check(c.last().error == NsdManager.FAILURE_INTERNAL_ERROR, "Malformed type error profile");
        h.register(c, 4, update(info()), 0); h.dispatch.drain();
        check(c.last().error == NsdManager.FAILURE_INTERNAL_ERROR && !c.last().retained, "Update without existing request");
    }

    static void lifetimes() throws Exception {
        for (boolean beforeSubmission : List.of(false, true)) {
            Harness h = new Harness(); Client c = h.client();
            h.register(c, 1, request(), 0);
            if (!beforeSubmission) h.dispatch.drain();
            WindowsNsdAdvertiser.Callback late = beforeSubmission ? null : h.backend.only().callback;
            h.die(h.process);
            if (late != null) late.registered(info());
            h.dispatch.drain();
            check(c.token.alive && c.token.deaths.isEmpty(), "Process death did not release a still-live foreign callback token");
            check(c.events.isEmpty() && h.backend.active.isEmpty() && h.clients() == 0, "Process death left advertisement/callback ownership");
            check(h.backend.starts == (beforeSubmission ? 0 : 1), "Dead queued owner reached the backend");
            h.service.processDied(h.process); h.dispatch.drain();
            check(h.backend.removed.size() == (beforeSubmission ? 0 : 1), "Repeated retirement withdrew twice");
        }
        Harness removed = new Harness(); Client c = removed.client();
        removed.register(c, 1, request(), 0); removed.dispatch.drain();
        removed.pm.beginPackageRemoval(removed.pkg);
        removed.pm.revokePackageCapabilities(removed.pkg.packageName); removed.dispatch.drain();
        check(removed.backend.active.isEmpty() && c.token.deaths.isEmpty(), "Package removal did not retire active advertisement");
        Harness queued = new Harness(); Client q = queued.client();
        queued.register(q, 1, request(), 0);
        queued.pm.beginPackageRemoval(queued.pkg);
        queued.pm.revokePackageCapabilities(queued.pkg.packageName); queued.dispatch.drain();
        check(queued.backend.starts == 0 && queued.clients() == 0, "Uninstall race admitted queued work");
        Harness token = new Harness(); Client t = token.client();
        token.register(t, 1, request(), 0); token.dispatch.drain(); t.token.die(); token.dispatch.drain();
        check(token.backend.active.isEmpty() && !token.process.dead, "Callback death must also withdraw a live process request");
        Harness link = new Harness(); Client l = link.client(); l.token.dieInLink = true;
        rejects(ServiceSpecificException.class, () -> link.register(l, 1, request(), 0), "Death during token publication");
        link.dispatch.drain();
        check(link.clients() == 0 && link.backend.starts == 0 && l.token.deaths.isEmpty(), "Link race retained ownership");
        Client rejected = link.client(); rejected.token.rejectLink = true;
        rejects(ServiceSpecificException.class, () -> link.register(rejected, 2, request(), 0), "Rejected death link");
        check(link.clients() == 0, "Failed token link published a client");
        Harness duringLink = new Harness(); Client racing = duringLink.client();
        racing.token.afterLink = () -> duringLink.die(duringLink.process);
        rejects(ServiceSpecificException.class, () -> duringLink.register(racing, 1, request(), 0),
                "Process retired after death link but before client publication");
        duringLink.dispatch.drain();
        check(duringLink.clients() == 0 && duringLink.backend.starts == 0
                && racing.token.deaths.isEmpty(), "Process publication race leaked ownership");
        Harness replacement = new Harness(); Client old = replacement.client();
        replacement.register(old, 1, request(), 0); replacement.dispatch.drain();
        Pending previous = replacement.backend.only(); WindowsNsdAdvertiser.Callback late = previous.callback;
        replacement.process.pidSuperseded = true;
        ActivityTaskManagerService.ProcessRecord current = replacement.attach(replacement.process.pid, replacement.pkg);
        Client fresh = replacement.client();
        as(current.pid, current.uid, () -> replacement.service.register(fresh, 1, request(), 0));
        replacement.dispatch.drain();
        replacement.die(replacement.process); late.registered(info()); replacement.dispatch.drain();
        check(replacement.backend.active.size() == 1 && !replacement.backend.active.containsKey(previous.id)
                && old.events.isEmpty(), "Old PID-generation retirement reached the new owner");
        replacement.die(current); replacement.dispatch.drain();
    }

    static void updatesAndErrors() throws Exception {
        Harness accepted = new Harness(); Client listener = accepted.client();
        accepted.register(listener, 1, request(), 0); accepted.dispatch.drain();
        Pending first = accepted.backend.only(); first.callback.registered(info()); accepted.dispatch.drain();
        NsdServiceInfo subtypes = info(); subtypes.setSubtypes(Set.of("_owned"));
        accepted.register(listener, 1, update(subtypes), 0); accepted.dispatch.drain();
        check(accepted.backend.updates == 1 && accepted.backend.lastUpdate.getSubtypes().contains("_owned")
                && accepted.backend.only().id == first.id && listener.events.size() == 1,
                "Accepted update must reuse ownership without inventing a success callback");
        accepted.die(accepted.process); accepted.dispatch.drain();
        for (boolean backendFailure : List.of(false, true)) {
            Harness h = new Harness(); Client c = h.client();
            h.register(c, 1, request(), 0); h.dispatch.drain(); Pending old = h.backend.only();
            WindowsNsdAdvertiser.Callback late = old.callback;
            NsdServiceInfo next = info(); next.setSubtypes(Set.of("_owned"));
            if (backendFailure) h.backend.rejectUpdate = true;
            else next.setHostname("bad.hostname");
            h.register(c, 1, update(next), 0); h.dispatch.drain();
            check(c.last().event == INsdRegistration.FAILED && c.last().retained
                    && h.backend.active.containsKey(old.id) && c.token.deaths.size() == 1,
                    "Rejected update lost retained transaction ownership");
            int before = c.events.size(); late.registered(info()); h.dispatch.drain();
            check(c.events.size() == before, "Failed-update listener received a later success");
            h.die(h.process); h.dispatch.drain();
            check(h.backend.active.isEmpty(), "Dead failed-update transaction survived cleanup");
        }
        Harness full = new Harness(); Client c = full.client();
        for (int id = 1; id <= 200; id++) full.register(c, id, request(), 0);
        full.dispatch.drain();
        check(full.backend.active.size() == 200, "Real per-client registration limit is not 200");
        full.register(c, 201, request(), 0); full.dispatch.drain();
        check(c.last().error == NsdManager.FAILURE_MAX_LIMIT && !c.last().retained, "Caller bypassed actual registration limit");
        full.register(c, 1, update(info()), 0); full.dispatch.drain();
        check(c.last().error == NsdManager.FAILURE_MAX_LIMIT && c.last().retained
                && full.backend.active.size() == 200, "Full-map update did not retain its old transaction");
        full.die(full.process); full.dispatch.drain();
        check(full.backend.removed.size() == 200 && c.token.deaths.isEmpty(), "Quota map leaked on process death");
        Harness failed = new Harness(); Client f = failed.client(); failed.backend.failRegister = true;
        failed.register(f, 1, request(), 0); failed.dispatch.drain();
        check(f.last().error == NsdManager.FAILURE_INTERNAL_ERROR && !f.last().retained
                && failed.clients() == 0, "Backend construction failure fabricated success/retained token");
        Harness callback = new Harness(); Client bad = callback.client(); bad.failEvent = true;
        callback.register(bad, 1, request(), 0); callback.dispatch.drain();
        callback.backend.only().callback.registered(info()); callback.dispatch.drain();
        check(callback.backend.active.isEmpty() && callback.clients() == 0, "Unavailable callback retained advertisement");
        Harness reentrant = new Harness(); Client r = reentrant.client();
        reentrant.register(r, 1, request(), 0); reentrant.dispatch.drain();
        r.duringEvent = () -> { reentrant.unregister(r, 1); reentrant.register(r, 2, request(), 0); };
        reentrant.backend.only().callback.registered(info()); reentrant.dispatch.drain();
        check(reentrant.backend.active.size() == 1 && r.events.size() == 2
                && r.last().event == INsdRegistration.UNREGISTERED, "Local callback reentry corrupted ownership");
        reentrant.die(reentrant.process); reentrant.dispatch.drain();
    }

    static int owned(Harness h) throws Exception { return ((Set<?>) get(h.service, "mOwnedIds")).size(); }
    static int withdrawals(Harness h) throws Exception { return ((Set<?>) get(h.service, "mWithdrawals")).size(); }

    static final class Bridge implements NsdRegistrationClient.Callback {
        final NsdRegistrationClient.Factory factory = NsdRegistrationClient.factory();
        final List<Event> events = new ArrayList<>();
        final Object connection;
        Bridge(INsdRegistration service) throws Exception {
            // Explicit already-connected service boundary. The actual client
            // connection, typed callback, retention and exception logic execute
            // unchanged; ServiceManager/native Binder transport is not claimed.
            Class<?> type = Class.forName("com.gesher.net.NsdRegistrationClient$Connection");
            var constructor = type.getDeclaredConstructor(); constructor.setAccessible(true);
            connection = constructor.newInstance();
            Class<?> remoteType = Class.forName("com.gesher.net.NsdRegistrationClient$Connection$Remote");
            var remoteConstructor = remoteType.getDeclaredConstructor(type, IBinder.class);
            remoteConstructor.setAccessible(true);
            set(connection, "remote", remoteConstructor.newInstance(connection, service.asBinder()));
            set(factory, "connection", connection);
        }
        public void registered(NsdServiceInfo info) { events.add(new Event(0,INsdRegistration.REGISTERED,0,false,info)); }
        public void failed(int error, boolean retained) { events.add(new Event(0,INsdRegistration.FAILED,error,retained,null)); }
        public void unregistered() { events.add(new Event(0,INsdRegistration.UNREGISTERED,0,false,null)); }
        public void unregisterFailed(int error) { events.add(new Event(0,INsdRegistration.UNREGISTER_FAILED,error,true,null)); }
        Event last() { return events.get(events.size()-1); }
        int operations() throws Exception { return ((Map<?,?>) get(connection,"operations")).size(); }
    }
    static final class BridgeEndpoint extends INsdRegistration.Stub {
        int exceptionCode;
        boolean deny;
        public void register(INsdRegistration.IClient client,int id,AdvertisingRequest request,int other)
                throws RemoteException {
            if (deny) throw new SecurityException("controlled permission boundary");
            if (exceptionCode != 0) throw new ServiceSpecificException(exceptionCode,"controlled enrollment boundary");
            client.event(id,INsdRegistration.REGISTERED,0,false,new NsdServiceInfo("OwnedBridge",null));
        }
        public void unregister(INsdRegistration.IClient client,int id)throws RemoteException {
            client.event(id,INsdRegistration.UNREGISTERED,0,false,null);
        }
    }
    static void bridgeErrors() throws Exception {
        BridgeEndpoint endpoint = new BridgeEndpoint(); Bridge bridge = new Bridge(endpoint);
        endpoint.exceptionCode = INsdRegistration.ERROR_CLIENT_UNAVAILABLE;
        bridge.factory.start(request(),0,bridge);
        check(bridge.events.size()==1 && bridge.last().error==NsdManager.FAILURE_INTERNAL_ERROR
                && !bridge.last().retained && bridge.operations()==0 && bridge.factory.retainedCount()==0,
                "Real initial bridge did not translate the recognized private error");
        endpoint.exceptionCode = 0;
        NsdRegistrationClient.Session session = bridge.factory.start(request(),0,bridge);
        endpoint.exceptionCode = INsdRegistration.ERROR_CLIENT_UNAVAILABLE;
        session.update(request(),0);
        check(bridge.last().event==INsdRegistration.FAILED && bridge.last().retained
                && bridge.operations()==0 && bridge.factory.retainedCount()==1,
                "Real update bridge lost the old transaction's retained accounting");
        Bridge unknown = new Bridge(endpoint); endpoint.exceptionCode = 917;
        rejects(ServiceSpecificException.class,()->unknown.factory.start(request(),0,unknown),"Unknown private error must remain visible");
        check(unknown.events.isEmpty() && unknown.operations()==0,"Unknown private error fabricated an NSD result");
        Bridge permission = new Bridge(endpoint); endpoint.deny = true;
        rejects(SecurityException.class,()->permission.factory.start(request(),0,permission),"Real bridge preserves permission denial");
        check(permission.events.isEmpty()&&permission.operations()==0,"Permission denial became an asynchronous availability result");
    }

    static void retirementCapacity() throws Exception {
        for (String reason : List.of("process", "package", "token")) {
            Harness h = new Harness(); Client c = h.client();
            h.register(c, 1, request(), 0); h.dispatch.drain();
            h.backend.rejectWithdrawals = 2;
            if (reason.equals("process")) h.die(h.process);
            else if (reason.equals("package")) {
                h.pm.beginPackageRemoval(h.pkg); h.pm.revokePackageCapabilities(h.pkg.packageName);
            } else c.token.die();
            h.dispatch.drain();
            check(h.backend.active.size() == 1 && h.clients() == 0 && withdrawals(h) == 1
                    && owned(h) == 1 && h.dispatch.delayed.size() == 1,
                    reason + " rejection lost charged cleanup ownership");
            h.service.processDied(h.process); h.service.packageRemoved(h.pkg.packageName);
            c.token.die(); h.dispatch.drain();
            check(h.backend.withdrawalAttempts == 1 && h.dispatch.delayed.size() == 1,
                    "Repeated retirement duplicated or abandoned the retry");
            h.dispatch.retry();
            check(h.backend.withdrawalAttempts == 2 && withdrawals(h) == 1 && owned(h) == 1,
                    "Second rejected withdrawal cleared ownership");
            h.dispatch.retry();
            check(h.backend.withdrawalAttempts == 3 && h.backend.active.isEmpty()
                    && withdrawals(h) == 0 && owned(h) == 0 && h.dispatch.delayed.isEmpty()
                    && c.events.isEmpty() && c.token.deaths.isEmpty(),
                    "Accepted retry did not release the ID independently of dead callbacks");
        }
        Harness explicit = new Harness(); Client stopped = explicit.client();
        explicit.register(stopped, 1, request(), 0); explicit.dispatch.drain();
        explicit.backend.rejectWithdrawals = 2;
        explicit.unregister(stopped, 1); explicit.unregister(stopped, 1);
        check(explicit.dispatch.jobs.size() == 1, "Repeated public stop has one reserved service job");
        explicit.dispatch.drain();
        check(stopped.last().event == INsdRegistration.UNREGISTER_FAILED && stopped.last().retained
                && explicit.backend.active.size() == 1 && owned(explicit) == 1,
                "Rejected explicit stop lost the old owner");
        explicit.die(explicit.process); explicit.dispatch.drain(); explicit.dispatch.retry();
        check(explicit.backend.active.isEmpty() && owned(explicit) == 0,
                "Explicit stop failure was not retired after process death");

        Harness queued = new Harness(); Client pending = queued.client();
        for (int id = 1; id <= 4096; id++) queued.register(pending, id, request(), 0);
        queued.register(pending, 4097, request(), 0);
        check(pending.last().event == INsdRegistration.FAILED
                && pending.last().error == NsdManager.FAILURE_INTERNAL_ERROR && !pending.last().retained,
                "Pre-queue exhaustion is a typed callback failure, not an application exception");
        check(queued.dispatch.jobs.size() == 4096 && (int) get(queued.service, "mPendingAdmissions") == 4096,
                "Service intake exceeded its reserved job budget");
        Bridge capacity = new Bridge(queued.service);
        as(queued.process.pid,queued.process.uid,()->capacity.factory.start(request(),0,capacity));
        check(capacity.events.size()==1 && capacity.last().event==INsdRegistration.FAILED
                && capacity.last().error==NsdManager.FAILURE_INTERNAL_ERROR && !capacity.last().retained
                && capacity.operations()==0 && capacity.factory.retainedCount()==0,
                "Real service capacity callback did not complete the real initial client bridge");
        queued.unregister(pending, 1); queued.unregister(pending, 1);
        check(queued.dispatch.jobs.size() == 4097, "Full intake queue rejected/co-duplicated stop ownership");
        queued.die(queued.process); queued.dispatch.drain();
        check(queued.backend.starts == 0 && queued.clients() == 0 && owned(queued) == 0
                && (int) get(queued.service, "mPendingAdmissions") == 0,
                "Canceled queued admissions retained permits or reached the backend");

        Harness updated = new Harness(); Bridge kept = new Bridge(updated.service);
        final NsdRegistrationClient.Session[] operation = new NsdRegistrationClient.Session[1];
        as(updated.process.pid,updated.process.uid,()->operation[0]=kept.factory.start(request(),0,kept));
        updated.dispatch.drain();
        for(int i=0;i<4096;i++)as(updated.process.pid,updated.process.uid,()->operation[0].update(update(info()),0));
        as(updated.process.pid,updated.process.uid,()->operation[0].update(update(info()),0));
        as(updated.process.pid,updated.process.uid,()->operation[0].update(update(info()),0));
        check(kept.events.isEmpty()&&updated.dispatch.jobs.size()==4097,
                "Existing-ID rejection is one reserved terminal job after accepted updates");
        updated.dispatch.drain();
        check(updated.backend.updates==4096 && kept.events.size()==1 && kept.last().retained
                && kept.factory.retainedCount()==1 && kept.operations()==0 && updated.backend.active.size()==1,
                "Capacity failure reordered accepted updates or lost retained real-client accounting");
        updated.die(updated.process); updated.dispatch.drain();
        check(updated.backend.active.isEmpty()&&owned(updated)==0,"Retained bridge owner did not retire on process death");

        Harness events = new Harness(); Client notified = events.client();
        events.register(notified, 1, request(), 0); events.dispatch.drain();
        WindowsNsdAdvertiser.Callback callback = events.backend.only().callback;
        for (int i = 0; i <= 4096; i++) callback.registered(info());
        check(events.dispatch.jobs.size() == 1, "Backend notification bursts bypassed the bounded mailbox");
        events.dispatch.drain();
        check(notified.events.size() == 1 && notified.last().event == INsdRegistration.FAILED
                && notified.last().error == NsdManager.FAILURE_INTERNAL_ERROR && !notified.last().retained
                && events.backend.active.isEmpty() && owned(events) == 0
                && (int) get(events.service, "mPendingBackendEvents") == 0,
                "Notification capacity fabricated success or abandoned its cleanup");

        Harness global = new Harness();
        int admitted = 0;
        for (int client = 0; client < 6; client++) {
            Client owner = global.client();
            int count = Math.min(200, 1024 - admitted);
            for (int id = 1; id <= count; id++) global.register(owner, id, request(), 0);
            global.dispatch.drain(); admitted += count;
        }
        Client overflow = global.client(); global.register(overflow, 1, request(), 0); global.dispatch.drain();
        check(global.backend.active.size() == 1024 && owned(global) == 1024
                && overflow.last().error == NsdManager.FAILURE_MAX_LIMIT,
                "Global service ID budget failed asynchronously without discarding active owners");
        global.backend.rejectWithdrawals = 1024;
        global.die(global.process); global.dispatch.drain();
        check(withdrawals(global) == 1024 && owned(global) == 1024
                && global.backend.withdrawalAttempts == 1024 && global.dispatch.delayed.size() == 1,
                "Retirement batch retried earlier IDs or stopped charging their capacity");
        global.dispatch.retry();
        check(global.backend.active.isEmpty() && owned(global) == 0 && withdrawals(global) == 0,
                "Global retirement budget was not released after actual admission");
    }

    record Retired(Harness harness, WeakReference<Client> client, WeakReference<IBinder> token) {}
    static Retired rejectedForCollection() throws Exception {
        Harness h = new Harness(); Client c = h.client();
        h.register(c, 1, request(), 0); h.dispatch.drain();
        h.backend.rejectWithdrawals = 1;
        h.die(h.process); h.dispatch.drain();
        check(h.backend.active.size() == 1 && withdrawals(h) == 1 && h.dispatch.jobs.isEmpty(),
                "Rejected-withdrawal collection fixture did not retain only pending backend ownership");
        for (Object id : (Set<?>) get(h.service, "mWithdrawals")) {
            check(id instanceof Integer, "Retry state retained more than the private backend ID");
        }
        return new Retired(h, new WeakReference<>(c), new WeakReference<>(c.token.binder));
    }
    static Retired stoppingForCollection() throws Exception {
        Harness h = new Harness(); Client c = h.client();
        h.register(c, 1, request(), 0); h.dispatch.drain();
        h.backend.deferUnregister = true;
        h.unregister(c, 1); h.dispatch.drain();
        check(h.backend.withdrawing.size() == 1 && c.events.isEmpty(),
                "Explicit stop completion was not held at the declared backend boundary");
        h.die(h.process); h.dispatch.drain();
        check(h.backend.withdrawing.size() == 1 && h.clients() == 0 && owned(h) == 0,
                "Forced cleanup did not coalesce with accepted stop ownership");
        return new Retired(h, new WeakReference<>(c), new WeakReference<>(c.token.binder));
    }
    static Retired retireForCollection(boolean failedUpdate) throws Exception {
        Harness h = new Harness(); Client c = h.client();
        h.register(c, 1, request(), 0); h.dispatch.drain();
        if (failedUpdate) {
            NsdServiceInfo bad = info(); bad.setHostname("bad.hostname");
            h.register(c, 1, update(bad), 0); h.dispatch.drain();
            h.die(h.process); h.dispatch.drain();
        } else {
            h.unregister(c, 1); h.dispatch.drain();
        }
        check(h.clients() == 0 && h.dispatch.jobs.isEmpty() && h.backend.active.isEmpty()
                && !h.backend.retiredValues.isEmpty(), "Value-only retirement preparation failed");
        return new Retired(h, new WeakReference<>(c), new WeakReference<>(c.token.binder));
    }
    static void collection() throws Exception {
        for (boolean retainedFailure : List.of(false, true)) {
            Retired retired = retireForCollection(retainedFailure);
            for (int i = 0; i < 100 && (retired.client.get() != null || retired.token.get() != null); i++) {
                System.gc();
                Thread.sleep(10);
            }
            check(retired.client.get() == null && retired.token.get() == null,
                    "Copied exit values kept the retired callback/client reachable");
            check(retired.harness.backend.retiredValues.get(0).getAttributes().get("opaque")[1]
                    == (byte) 0xff, "Client release discarded the separately owned exit values");
        }
        Retired rejected = rejectedForCollection();
        for (int i = 0; i < 100 && (rejected.client.get() != null || rejected.token.get() != null); i++) {
            System.gc(); Thread.sleep(10);
        }
        check(rejected.client.get() == null && rejected.token.get() == null,
                "Pending withdrawal/backend callback retained a retired client or token");
        rejected.harness.dispatch.retry();
        check(rejected.harness.backend.active.isEmpty() && withdrawals(rejected.harness) == 0,
                "Value-only retry did not complete after callback collection");
        Retired stopping = stoppingForCollection();
        for (int i = 0; i < 100 && (stopping.client.get() != null || stopping.token.get() != null); i++) {
            System.gc(); Thread.sleep(10);
        }
        check(stopping.client.get() == null && stopping.token.get() == null,
                "Deferred explicit-stop completion kept a dead owner/token reachable");
        int id = stopping.harness.backend.withdrawing.keySet().iterator().next();
        stopping.harness.backend.finish(id); stopping.harness.dispatch.drain();
        check(stopping.harness.backend.withdrawing.isEmpty(), "Retired weak stop completion did not settle");
    }

    static void diagnostics() throws Exception {
        Harness h = new Harness();
        StringWriter output = new StringWriter();
        PrintWriter writer = new PrintWriter(output);
        rejects(SecurityException.class, () -> as(h.process.pid, h.process.uid,
                () -> h.service.dump(null, writer, new String[0])), "Attached application cannot dump owner state");
        rejects(SecurityException.class, () -> as(h.process.pid, android.os.Process.SYSTEM_UID,
                () -> h.service.dump(null, writer, new String[0])), "Attached PID cannot gain diagnostics by a UID claim");
        rejects(SecurityException.class, () -> as(861099, 12399,
                () -> h.service.dump(null, writer, new String[0])), "Unknown application cannot dump owner state");
        check(h.backend.diagnosticReads == 0 && output.toString().isEmpty(), "Denied dump touched native counters");
        as(861098, android.os.Process.SHELL_UID, () -> h.service.dump(null, writer, new String[0]));
        check(output.toString().contains("clients=0 requests=0 native_requests=1 sockets=2 watches=2 callbacks=0 polled_requests=1 sent_datagrams=9"),
                "Authorized owner diagnostic lost the explicit backend counters");
        output.getBuffer().setLength(0); h.backend.diagnosticUnavailable = true;
        as(861098, android.os.Process.SHELL_UID, () -> h.service.dump(null, writer, new String[0]));
        check(output.toString().contains("NSD_REGISTRATION_NATIVE_UNAVAILABLE")
                && !output.toString().contains("native_requests=0"), "Unavailable native provider became fabricated zero counters");
    }

    public static void main(String[] args) throws Throwable {
        System.load(args[0]);
        // Use Android's host test mode for package-cache nonce storage.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        Field cache = android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");
        cache.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?, ?>) cache.get(null)).testPropertyName();
        // Unrelated constructed device settings: host has no Android ICU data image.
        // Real identity, permission, registration and process-death code remain selected.
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        Field hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        Field list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        try {
            valuesAndWithdrawal(); admission(); lifetimes(); updatesAndErrors(); bridgeErrors(); retirementCapacity(); collection(); diagnostics();
            System.out.println("NsdRegistration service host all passed: " + checks + " checks");
            for (Class<?> type : List.of(NsdRegistrationService.class, INsdRegistration.class,
                    WindowsNsdAdvertiser.class, NsdServiceInfo.class, AdvertisingRequest.class,
                    Binder.class, PermissionPolicy.class)) {
                System.out.println("provider " + type.getName() + " "
                        + type.getProtectionDomain().getCodeSource().getLocation());
            }
            System.out.println("Host ownership/policy execution only; explicit parsed metadata, native identity, death, dispatcher and backend seams. No remote Binder, Windows, ART, packet delivery or ordinary-APK permission execution claim.");
        } catch (Throwable failure) { failure.printStackTrace(System.err); throw failure; }
    }
}
