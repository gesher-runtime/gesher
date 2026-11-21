/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.PendingIntent;
import android.content.ContextWrapper;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.pm.PermissionInfo;
import android.location.ILocationCallback;
import android.location.ILocationListener;
import android.location.ILocationManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.location.LocationRequest;
import android.location.provider.ProviderProperties;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ICancellationSignal;
import android.os.IRemoteCallback;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.SystemClock;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.LocationManagerService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Delayed;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Real registration code and mock-provider delivery; no physical geolocation is requested. */
public final class LocationRegistrationContractTest {
    private static final String PKG = "fixture.location.registrations";
    private static final String GPS = "gps", NETWORK = "network";
    private static final Unsafe UNSAFE;
    private static final AtomicInteger checks = new AtomicInteger();
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Work { void run() throws Exception; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks.incrementAndGet();
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String message) throws Exception {
        try { work.run(); }
        catch (Throwable failure) { check(type.isInstance(failure), message + ": " + failure); return; }
        throw new AssertionError(message + ": no exception");
    }
    private static void set(Object object, String name, Object value) throws Exception {
        Field field = object.getClass().getDeclaredField(name); field.setAccessible(true); field.set(object, value);
    }
    private static Object get(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(object);
    }
    private static LocationRequest request() { return new LocationRequest.Builder(0).setMaxUpdates(100).build(); }

    /** Controlled Binder lifetime; calls must never run under the production registry lock. */
    private static final class Token implements InvocationHandler {
        final IBinder binder = (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class}, this);
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        final Object registryLock;
        final AtomicInteger lockedClientCalls = new AtomicInteger();
        boolean rejectLink, dieDuringLink;
        int links, unlinks;
        Runnable afterLink;
        Token(Object registryLock) { this.registryLock = registryLock; }
        void recordLockBoundary() {
            if (Thread.holdsLock(registryLock)) lockedClientCalls.incrementAndGet();
        }
        @Override public Object invoke(Object self, Method method, Object[] args) throws Throwable {
            recordLockBoundary();
            switch (method.getName()) {
                case "equals": return self == args[0];
                case "hashCode": return System.identityHashCode(self);
                case "toString": return "LocationFixtureToken";
                case "isBinderAlive": case "pingBinder": return true;
                case "queryLocalInterface": return null;
                case "linkToDeath": {
                    IBinder.DeathRecipient recipient = (IBinder.DeathRecipient)args[0];
                    synchronized (this) {
                        links++;
                        if (rejectLink) throw new RemoteException("controlled rejected link");
                        recipients.add(recipient);
                    }
                    if (dieDuringLink) recipient.binderDied();
                    if (afterLink != null) afterLink.run();
                    return null;
                }
                case "unlinkToDeath": synchronized (this) { unlinks++; return recipients.remove(args[0]); }
                default: throw new AssertionError("Unexpected Binder operation " + method.getName());
            }
        }
        synchronized int linked() { return recipients.size(); }
        synchronized IBinder.DeathRecipient first() { return recipients.get(0); }
        void die() {
            List<IBinder.DeathRecipient> copy;
            synchronized (this) { copy = new ArrayList<>(recipients); }
            for (IBinder.DeathRecipient recipient : copy) recipient.binderDied();
        }
    }

    private static final class Listener extends ILocationListener.Default {
        final Token token;
        final List<String> fixes = new ArrayList<>();
        Listener(Token token) { this.token = token; }
        @Override public IBinder asBinder() {
            token.recordLockBoundary();
            return token.binder;
        }
        @Override public void onLocationChanged(List<Location> locations, IRemoteCallback completion) {
            check(!Thread.holdsLock(token.registryLock), "location delivered under registry lock");
            for (Location location : locations) fixes.add(location.getProvider());
        }
    }

    private static final class Sender extends IIntentSender.Default {
        final Token token;
        final List<String> fixes = new ArrayList<>();
        Sender(Token token) { this.token = token; }
        @Override public IBinder asBinder() { return token.binder; }
        @Override public void send(int code, Intent intent, String type, IBinder allowlist,
                IIntentReceiver finished, String permission, Bundle options) {
            check(!Thread.holdsLock(token.registryLock), "PendingIntent sent under registry lock");
            Location location = intent.getParcelableExtra(LocationManager.KEY_LOCATION_CHANGED, Location.class);
            if (location != null) fixes.add(location.getProvider());
        }
    }

    /** Timer admission is controlled; production expiry checks still read the real clock. */
    private static final class Timer implements ScheduledFuture<Object> {
        final Runnable command;
        final long delayMillis;
        boolean cancelled;
        Timer(Runnable command, long delay, TimeUnit unit) {
            this.command = command; this.delayMillis = unit.toMillis(delay);
        }
        void fire() { command.run(); } // also models an already admitted stale callback
        @Override public boolean cancel(boolean interrupt) { cancelled = true; return true; }
        @Override public boolean isCancelled() { return cancelled; }
        @Override public boolean isDone() { return cancelled; }
        @Override public long getDelay(TimeUnit unit) { return unit.convert(delayMillis, TimeUnit.MILLISECONDS); }
        @Override public int compareTo(Delayed other) { return Long.compare(getDelay(TimeUnit.MILLISECONDS), other.getDelay(TimeUnit.MILLISECONDS)); }
        @Override public Object get() { throw new UnsupportedOperationException("controlled timer"); }
        @Override public Object get(long timeout, TimeUnit unit) { throw new UnsupportedOperationException("controlled timer"); }
    }
    private static final class Scheduler extends ScheduledThreadPoolExecutor {
        final List<Timer> timers = new ArrayList<>();
        Scheduler() { super(1); }
        @Override public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            Timer timer = new Timer(command, delay, unit); timers.add(timer); return timer;
        }
    }
    private static final class Current extends ILocationCallback.Default {
        final Token token;
        final AtomicInteger calls = new AtomicInteger(), nulls = new AtomicInteger();
        Runnable onFirst;
        Current(Token token) { this.token = token; }
        @Override public IBinder asBinder() { token.recordLockBoundary(); return token.binder; }
        @Override public void onLocation(Location location) {
            check(!Thread.holdsLock(token.registryLock), "current callback remains outside registry lock");
            int n = calls.incrementAndGet();
            if (location == null) nulls.incrementAndGet();
            if (n == 1 && onFirst != null) onFirst.run();
        }
    }

    private static final class Harness implements AutoCloseable {
        final LocationManagerService service;
        final Object lock = new Object();
        final List<Token> tokens = new ArrayList<>();
        final Scheduler executor = new Scheduler();
        Harness() throws Exception {
            // Supply declared package/provider metadata directly, avoiding Windows access
            // initialization and an unrelated APK install. Permission checks remain real.
            PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(PKG, "/fixture/location/base.apk");
            parsed.setTargetSdkVersion(36);
            parsed.addUsesPermission(new ParsedUsesPermissionImpl(Manifest.permission.ACCESS_FINE_LOCATION, 0, Set.of()));
            parsed.hideAsFinal();
            ApkPackage pkg = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
            set(pkg, "packageName", PKG); set(pkg, "parsed", parsed); set(pkg, "mUid", 12241);
            set(pkg, "mSigningIdentityChecked", true);
            PackageManagerService pm = (PackageManagerService)UNSAFE.allocateInstance(PackageManagerService.class);
            set(pm, "mPackages", new LinkedHashMap<>(Map.of(PKG, pkg)));
            Object catalog = UNSAFE.allocateInstance(Class.forName("com.gesher.server.PermissionCatalog"));
            PermissionInfo permission = new PermissionInfo();
            permission.name = Manifest.permission.ACCESS_FINE_LOCATION; permission.packageName = "android";
            permission.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
            set(catalog, "mPm", pm); set(catalog, "mLoaded", true);
            set(catalog, "mFramework", new LinkedHashMap<>(Map.of(permission.name, permission)));
            Object policy = UNSAFE.allocateInstance(Class.forName("com.gesher.server.PermissionPolicy"));
            Properties decisions = new Properties(); decisions.setProperty(PKG + ":unsigned:" + permission.name, "granted");
            set(policy, "mPm", pm); set(policy, "mCatalog", catalog); set(policy, "mDecisions", decisions);
            set(policy, "mForgottenPackages", new java.util.WeakHashMap<>());
            set(pm, "mPermissions", policy);
            service = (LocationManagerService)UNSAFE.allocateInstance(LocationManagerService.class);
            set(service, "mPm", pm); set(service, "mLock", lock); set(service, "mExecutor", executor);
            // The baseline shares the poll executor; the repaired service owns a
            // separate expiry executor. Both use the same deterministic seam here.
            try { set(service, "mExpirationExecutor", executor); }
            catch (NoSuchFieldException baseline) { }
            set(service, "mRegistrations", new ArrayList<>()); set(service, "mLast", new HashMap<>());
            set(service, "mReportedEnabled", new HashMap<>());
            Class<?> type = Class.forName("com.gesher.server.LocationManagerService$TestProvider");
            Constructor<?> constructor = type.getDeclaredConstructor(String.class, ProviderProperties.class);
            constructor.setAccessible(true);
            Map<String,Object> providers = new LinkedHashMap<>();
            for (String name : List.of(GPS, NETWORK, "fused")) {
                Object provider = constructor.newInstance(name, new ProviderProperties.Builder().build());
                set(provider, "enabled", true); providers.put(name, provider);
            }
            set(service, "mTestProviders", providers);
        }
        Token token() { Token token = new Token(lock); tokens.add(token); return token; }
        int count() throws Exception { synchronized (lock) { return ((List<?>)get(service, "mRegistrations")).size(); } }
        void register(String provider, ILocationListener listener) { register(provider, request(), listener); }
        void register(String provider, LocationRequest request, ILocationListener listener) {
            service.registerLocationListener(provider, request, listener, PKG, null, "fixture-listener");
        }
        Object record() throws Exception { synchronized (lock) { return ((List<?>)get(service, "mRegistrations")).get(0); } }
        ICancellationSignal current(Current current) {
            return service.getCurrentLocation(GPS, new LocationRequest.Builder(0).setDurationMillis(30_000).build(),
                    current, PKG, null, "current");
        }
        void register(String provider, PendingIntent pending) {
            service.registerLocationPendingIntent(provider, request(), pending, PKG, null);
        }
        void report(String provider) {
            Location fix = new Location(provider); fix.setLatitude(31); fix.setLongitude(35);
            fix.setAccuracy(3); fix.setTime(System.currentTimeMillis()); fix.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
            service.setTestProviderLocation(provider, fix, PKG, null);
        }
        @Override public void close() {
            executor.shutdownNow();
            for (Token token : tokens) check(token.lockedClientCalls.get() == 0,
                    "client Binder/equality/asBinder calls remain outside registry lock");
        }
    }

    private static void listeners() throws Exception {
        try (Harness h = new Harness()) {
            Listener callback = new Listener(h.token());
            h.register(GPS, callback); IBinder.DeathRecipient old = callback.token.first();
            h.register(NETWORK, callback);
            check(h.count() == 2, "one listener retains registrations for both providers");
            check(callback.token.linked() == 2, "one death link per provider registration");
            h.report(GPS); h.report(NETWORK);
            check(callback.fixes.equals(List.of(GPS, NETWORK)), "both provider callbacks arrive");
            h.register(GPS, callback);
            check(h.count() == 2 && callback.token.linked() == 2, "same-provider registration replaces only itself");
            old.binderDied();
            check(h.count() == 2, "stale death cannot retire replacement or other provider");
            h.report(GPS);
            check(callback.fixes.equals(List.of(GPS, NETWORK, GPS)), "replacement delivers once");
            h.service.unregisterLocationListener(new Listener(callback.token));
            check(h.count() == 0 && callback.token.linked() == 0, "explicit unregister removes all providers by Binder identity");
            h.report(NETWORK);
            check(callback.fixes.size() == 3, "unregister stops further delivery");
        }
    }
    private static void pendingIntents() throws Exception {
        try (Harness h = new Harness()) {
            Sender sender = new Sender(h.token());
            PendingIntent pending = new PendingIntent(sender);
            h.register(GPS, pending); h.register(NETWORK, pending);
            check(h.count() == 2, "one PendingIntent retains both providers");
            h.report(GPS); h.report(NETWORK);
            check(sender.fixes.equals(List.of(GPS, NETWORK)), "both PendingIntent deliveries arrive");
            h.register(GPS, new PendingIntent(sender));
            check(h.count() == 2, "equivalent PendingIntent replaces only selected provider");
            h.report(GPS);
            check(sender.fixes.equals(List.of(GPS, NETWORK, GPS)), "PendingIntent replacement delivers once");
            h.service.unregisterLocationPendingIntent(new PendingIntent(sender));
            check(h.count() == 0, "explicit PendingIntent unregister removes all providers");
        }
    }
    private static void failures() throws Exception {
        try (Harness h = new Harness()) {
            Listener listener = new Listener(h.token()); h.register(GPS, listener);
            rejects(NullPointerException.class, () -> h.service.registerLocationListener(GPS, null, listener, PKG, null, "fixture"), "null request rejected");
            check(h.count() == 1 && listener.token.linked() == 1, "invalid request does not remove old registration");
            rejects(NullPointerException.class, () -> h.register(GPS, (ILocationListener)null), "null listener rejected");
            rejects(IllegalArgumentException.class, () -> h.register("missing-provider", listener), "invalid provider rejected");
            check(h.count() == 1, "invalid listener/provider do not mutate registry");
            listener.token.rejectLink = true; h.register(GPS, listener);
            check(h.count() == 1 && listener.token.linked() == 1, "failed link does not replace an active request");
            listener.token.die(); check(h.count() == 0 && listener.token.linked() == 0, "registered token death retires request");
            Sender sender = new Sender(h.token()); PendingIntent pending = new PendingIntent(sender); h.register(GPS, pending);
            rejects(NullPointerException.class, () -> h.service.registerLocationPendingIntent(GPS, null, pending, PKG, null), "null PendingIntent request rejected");
            rejects(NullPointerException.class, () -> h.register(GPS, (PendingIntent)null), "null PendingIntent rejected");
            check(h.count() == 1, "invalid PendingIntent registration does not remove old request");
            h.service.unregisterLocationPendingIntent(pending);
        }
    }
    private static void deaths() throws Exception {
        try (Harness h = new Harness()) {
            Listener dying = new Listener(h.token()); dying.token.dieDuringLink = true;
            h.register(GPS, dying);
            check(h.count() == 0 && dying.token.linked() == 0, "death during link is never published");
            check(dying.token.unlinks == 1, "completed link is deterministically retired once");
            Listener waiting = new Listener(h.token());
            CountDownLatch linked = new CountDownLatch(1), resume = new CountDownLatch(1);
            waiting.token.afterLink = () -> {
                linked.countDown();
                try { if (!resume.await(5, TimeUnit.SECONDS)) throw new AssertionError("link wait timed out"); }
                catch (InterruptedException e) { throw new AssertionError(e); }
            };
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread thread = new Thread(() -> {
                long old = Binder.clearCallingIdentity();
                try { Binder.restoreCallingIdentity((1000L << 32) | 910007); h.register(GPS, waiting); }
                catch (Throwable error) { failure.set(error); }
                finally { Binder.restoreCallingIdentity(old); }
            });
            thread.setDaemon(true); thread.start();
            try { check(linked.await(5, TimeUnit.SECONDS), "paused at death-link boundary"); waiting.token.die(); }
            finally { resume.countDown(); }
            thread.join(5000);
            check(!thread.isAlive() && failure.get() == null, "concurrent registration completes: " + failure.get());
            check(h.count() == 0 && waiting.token.linked() == 0, "death before publication cannot leak request");
        }
    }
    private static void currentAndOtherClients() throws Exception {
        try (Harness h = new Harness()) {
            Listener first = new Listener(h.token()), other = new Listener(h.token());
            ILocationCallback current = new ILocationCallback.Default() {
                final IBinder binder = h.token().binder;
                @Override public IBinder asBinder() { return binder; }
            };
            ICancellationSignal cancellation = h.service.getCurrentLocation(GPS,
                    new LocationRequest.Builder(0).setDurationMillis(30_000).build(), current, PKG, null, "current");
            h.register(GPS, first); h.register(NETWORK, first); h.register(GPS, other); h.register(GPS, first);
            check(h.count() == 4, "replacement preserves current-location and other-client records");
            h.service.unregisterLocationListener(first);
            check(h.count() == 2 && other.token.linked() == 1, "global client removal preserves unrelated records");
            cancellation.cancel(); check(h.count() == 1, "current cancellation remains independent");
            other.token.die(); check(h.count() == 0, "other client death cleans only its request");
        }
    }

    private static void terminal() throws Exception {
        try (Harness h = new Harness()) {
            Token token = h.token(); AtomicInteger calls = new AtomicInteger();
            ILocationListener listener = new ILocationListener.Default() {
                @Override public IBinder asBinder() { return token.binder; }
                @Override public void onLocationChanged(List<Location> locations, IRemoteCallback done) {
                    check(!Thread.holdsLock(h.lock), "terminal listener outside registry lock");
                    if (calls.incrementAndGet() == 1) h.report(GPS);
                }
            };
            h.register(GPS, new LocationRequest.Builder(0).setMaxUpdates(1).build(), listener);
            h.report(GPS);
            check(calls.get() == 1, "reentrant report cannot exceed maxUpdates=1");
            check(h.count() == 0 && token.linked() == 0, "last update retires and unlinks before callback");
        }
        try (Harness h = new Harness()) {
            Current current = new Current(h.token()); current.onFirst = () -> h.report(GPS);
            h.current(current); h.report(GPS);
            check(current.calls.get() == 1 && current.nulls.get() == 0, "current result claimed before reentrant report");
            check(h.count() == 0 && current.token.unlinks == 1, "successful current result releases one death link");
            check(h.executor.timers.get(0).cancelled, "successful current result cancels its timer");
            h.executor.timers.get(0).fire();
            check(current.calls.get() == 1, "stale timeout cannot add a null result after success");
        }
        lateCurrent();
        try (Harness h = new Harness()) {
            Token token = h.token(); Listener replacement = new Listener(token);
            ILocationListener original = new ILocationListener.Default() {
                @Override public IBinder asBinder() { return token.binder; }
                @Override public void onLocationChanged(List<Location> locations, IRemoteCallback done) {
                    h.register(GPS, replacement);
                    throw new IllegalStateException("controlled old callback failure");
                }
            };
            h.register(GPS, new LocationRequest.Builder(0).setMaxUpdates(1).build(), original);
            h.report(GPS); h.report(GPS);
            check(h.count() == 1 && replacement.fixes.size() == 1,
                    "old terminal callback failure does not retire its replacement");
        }
    }

    private static void concurrent() throws Exception {
        try (Harness h = new Harness()) {
            Current current = new Current(h.token());
            CountDownLatch delivered = new CountDownLatch(1), resume = new CountDownLatch(1);
            current.onFirst = () -> {
                delivered.countDown();
                try { if (!resume.await(5, TimeUnit.SECONDS)) throw new AssertionError("delivery release timeout"); }
                catch (InterruptedException e) { throw new AssertionError(e); }
            };
            h.current(current); Object record = h.record();
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread worker = new Thread(() -> {
                long old = Binder.clearCallingIdentity();
                try { Binder.restoreCallingIdentity((1000L << 32) | 910007); h.report(GPS); }
                catch (Throwable error) { failure.set(error); }
                finally { Binder.restoreCallingIdentity(old); }
            });
            worker.setDaemon(true); worker.start();
            try {
                check(delivered.await(5, TimeUnit.SECONDS), "first callback admitted");
                h.report(GPS);
                set(record, "expiresAt", SystemClock.elapsedRealtime() - 1);
                h.executor.timers.get(0).fire();
            } finally { resume.countDown(); worker.join(5000); }
            check(!worker.isAlive() && failure.get() == null, "concurrent current producer completes: " + failure.get());
            check(current.calls.get() == 1 && current.nulls.get() == 0,
                    "concurrent fix and timeout cannot redeliver an admitted terminal result");
            check(h.count() == 0 && current.token.unlinks == 1, "concurrent terminal claim cleans once");
        }
    }

    private static void expiry() throws Exception {
        try (Harness h = new Harness()) {
            Listener listener = new Listener(h.token());
            h.register(GPS, new LocationRequest.Builder(0).setDurationMillis(60_000).build(), listener);
            check(h.executor.timers.size() == 1, "finite listener receives an expiry timer without a fix");
            Timer first = h.executor.timers.get(0);
            check(first.delayMillis > 0 && first.delayMillis <= 60_000, "timer uses remaining request duration");
            first.fire();
            check(h.count() == 1 && h.executor.timers.size() == 2, "early timer wakeup retains request and rearms");
            set(h.record(), "expiresAt", SystemClock.elapsedRealtime() - 1);
            h.executor.timers.get(1).fire();
            check(h.count() == 0 && listener.token.linked() == 0 && listener.fixes.isEmpty(),
                    "finite listener retires without any location report");
            check(h.executor.timers.get(1).cancelled, "expiry drops retained timer");
        }
        try (Harness h = new Harness()) {
            Sender sender = new Sender(h.token()); PendingIntent pending = new PendingIntent(sender);
            h.service.registerLocationPendingIntent(GPS,
                    new LocationRequest.Builder(0).setDurationMillis(60_000).build(), pending, PKG, null);
            check(h.executor.timers.size() == 1, "finite PendingIntent receives an expiry timer");
            set(h.record(), "expiresAt", SystemClock.elapsedRealtime() - 1);
            h.executor.timers.get(0).fire();
            check(h.count() == 0 && sender.fixes.isEmpty(), "PendingIntent expires without a fix or location delivery");
        }
        try (Harness h = new Harness()) {
            Listener listener = new Listener(h.token());
            LocationRequest finite = new LocationRequest.Builder(0).setDurationMillis(60_000).build();
            h.register(GPS, finite, listener); Object old = h.record(); Timer stale = h.executor.timers.get(0);
            h.register(GPS, finite, listener);
            check(stale.cancelled && h.count() == 1, "replacement cancels previous request timer");
            set(old, "expiresAt", SystemClock.elapsedRealtime() - 1); stale.fire();
            check(h.count() == 1, "admitted old expiry cannot remove a replacement");
            Timer live = h.executor.timers.get(1); h.service.unregisterLocationListener(listener);
            check(live.cancelled && h.count() == 0, "explicit removal cancels current expiry timer");
        }
        durationLimits();
    }

    private static void currentDeaths() throws Exception {
        try (Harness h = new Harness()) {
            Current current = new Current(h.token()); ICancellationSignal cancellation = h.current(current);
            check(current.token.linked() == 1, "current request links callback death");
            current.token.die();
            check(h.count() == 0 && current.token.linked() == 0 && current.token.unlinks == 1,
                    "current callback death retires once");
            check(h.executor.timers.get(0).cancelled, "current death cancels timeout");
            h.executor.timers.get(0).fire(); cancellation.cancel(); h.report(GPS);
            check(current.calls.get() == 0, "death and cancellation produce no terminal callback");
        }
        try (Harness h = new Harness()) {
            Current current = new Current(h.token()); current.token.rejectLink = true;
            h.current(current);
            check(h.count() == 0 && h.executor.timers.isEmpty(), "failed current death link never publishes or starts a timer");
        }
        try (Harness h = new Harness()) {
            Current current = new Current(h.token()); current.token.dieDuringLink = true;
            h.current(current);
            check(h.count() == 0 && current.token.linked() == 0 && current.token.unlinks == 1,
                    "current death during link never publishes and unlinks once");
        }
        try (Harness h = new Harness()) {
            Current current = new Current(h.token()); h.current(current).cancel();
            check(h.count() == 0 && current.token.linked() == 0 && current.token.unlinks == 1,
                    "current cancellation releases callback death link");
            h.executor.timers.get(0).fire(); check(current.calls.get() == 0, "cancellation suppresses timeout callback");
        }
    }

    private static void lateCurrent() throws Exception {
        try (Harness h = new Harness()) {
            Current current = new Current(h.token()); h.current(current);
            set(h.record(), "expiresAt", SystemClock.elapsedRealtime() - 1);
            h.report(GPS); h.executor.timers.get(0).fire();
            check(current.calls.get() == 1 && current.nulls.get() == 1,
                    "late fix and delayed timeout produce exactly one null completion");
            check(h.count() == 0 && current.token.linked() == 0, "expired current result retires and unlinks");
        }
    }

    private static void durationLimits() throws Exception {
        try (Harness h = new Harness()) {
            Listener listener = new Listener(h.token());
            h.register(GPS, new LocationRequest.Builder(0).setDurationMillis(Long.MAX_VALUE - 1).build(), listener);
            h.report(GPS);
            check(listener.fixes.size() == 1 && h.count() == 1, "near-MAX duration saturates instead of expiring by overflow");
            check(h.executor.timers.isEmpty(), "saturated unlimited duration needs no expiry timer");
        }
        try (Harness h = new Harness()) {
            Listener listener = new Listener(h.token());
            h.register(GPS, request().setExpireAt(0), listener); h.report(GPS);
            check(listener.fixes.isEmpty() && h.count() == 0, "legacy absolute expiration remains honored");
        }
        try (Harness h = new Harness()) {
            Current current = new Current(h.token());
            h.service.getCurrentLocation(GPS, request(), current, "android", null, "system-current");
            h.report(GPS);
            check(current.calls.get() == 1 && current.nulls.get() == 0,
                    "admitted system request retains its system identity on delivery");
        }
    }

    private static void disabled() throws Exception {
        try (Harness h = new Harness()) {
            Current current = new Current(h.token()); h.current(current);
            h.service.setTestProviderEnabled(GPS, false, PKG, null);
            check(current.calls.get() == 1 && current.nulls.get() == 1 && h.count() == 0,
                    "provider disabling completes current request with null");
            h.executor.timers.get(0).fire(); check(current.calls.get() == 1, "disabled provider completion wins against old timer");
        }
    }

    private static void initialDisabledNotifications() throws Exception {
        for (boolean initiallyEnabled : new boolean[] {false, true}) {
            for (boolean pending : new boolean[] {false, true}) {
                try (Harness h = new Harness()) {
                    // Constructor metadata is controlled as in the other fixtures:
                    // the asynchronous first provider observation has not run yet.
                    Map<?,?> providers = (Map<?,?>)get(h.service, "mTestProviders");
                    set(providers.get(GPS), "enabled", initiallyEnabled);
                    String scenario = (pending ? "PendingIntent" : "listener")
                            + (initiallyEnabled ? " initially enabled" : " initially disabled");
                    Token token = h.token();
                    List<Boolean> states = new ArrayList<>();
                    ILocationListener listener = new ILocationListener.Default() {
                        @Override public IBinder asBinder() { return token.binder; }
                        @Override public void onProviderEnabledChanged(String provider, boolean enabled) {
                            check(!Thread.holdsLock(h.lock) && GPS.equals(provider),
                                    scenario + " provider callback outside registry lock");
                            states.add(enabled);
                        }
                    };
                    PendingIntent intent = new PendingIntent(new IIntentSender.Default() {
                        @Override public IBinder asBinder() { return token.binder; }
                        @Override public void send(int code, Intent fillIn, String type, IBinder allowlist,
                                IIntentReceiver finished, String permission, Bundle options) {
                            check(!Thread.holdsLock(h.lock)
                                            && fillIn.hasExtra(LocationManager.KEY_PROVIDER_ENABLED),
                                    scenario + " provider PendingIntent outside registry lock");
                            states.add(fillIn.getBooleanExtra(LocationManager.KEY_PROVIDER_ENABLED, true));
                        }
                    });
                    if (pending) h.register(GPS, intent); else h.register(GPS, listener);
                    check(states.equals(initiallyEnabled ? List.of() : List.of(false)),
                            scenario + " initial registration notification");
                    h.service.setTestProviderEnabled(GPS, false, PKG, null);
                    check(states.equals(List.of(false)) && h.count() == 1,
                            scenario + " first disabled observation delivers exactly one false");
                    h.service.setTestProviderEnabled(GPS, false, PKG, null);
                    check(states.equals(List.of(false)), scenario + " unchanged state stays silent");
                    h.service.setTestProviderEnabled(GPS, true, PKG, null);
                    h.service.setTestProviderEnabled(GPS, true, PKG, null);
                    h.service.setTestProviderEnabled(GPS, false, PKG, null);
                    check(states.equals(List.of(false, true, false)),
                            scenario + " later transitions each deliver once");
                    if (pending) h.service.unregisterLocationPendingIntent(intent);
                    else h.service.unregisterLocationListener(listener);
                    h.service.setTestProviderEnabled(GPS, true, PKG, null);
                    check(states.equals(List.of(false, true, false)) && h.count() == 0,
                            scenario + " removal ends provider notifications");
                }
            }
        }
    }

    /** Real generated AIDL request Parcel plus public LocationManager callback bookkeeping. */
    private static void binderAndManager() throws Exception {
        try (Harness h = new Harness()) {
            IBinder target = StandIn.of(ILocationManager.class, h.service).asBinder();
            ILocationManager remote = ILocationManager.Stub.asInterface(new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                    return target.transact(code, data, reply, flags);
                }
            });
            ContextWrapper context = new ContextWrapper(null) {
                @Override public String getPackageName() { return PKG; }
                @Override public String getOpPackageName() { return PKG; }
                @Override public String getAttributionTag() { return "location-contract"; }
            };
            LocationManager manager = new LocationManager(context, remote);
            List<String> fixes = new ArrayList<>();
            LocationListener listener = location -> fixes.add(location.getProvider());
            manager.requestLocationUpdates(GPS, request(), Runnable::run, listener);
            manager.requestLocationUpdates(NETWORK, request(), Runnable::run, listener);
            check(h.count() == 2, "public manager through AIDL retains both provider requests");
            h.report(GPS); h.report(NETWORK);
            check(fixes.equals(List.of(GPS, NETWORK)), "public manager receives both real callback values");
            manager.requestLocationUpdates(GPS, request(), Runnable::run, listener);
            check(h.count() == 2, "AIDL same-provider replacement");
            manager.removeUpdates(listener);
            check(h.count() == 0, "public removeUpdates retires all providers");
            List<String> pendingFixes = new ArrayList<>();
            IIntentSender sender = new IIntentSender.Stub() {
                @Override public void send(int code, Intent intent, String type, IBinder allowlist,
                        IIntentReceiver finished, String permission, Bundle options) {
                    Location location = intent.getParcelableExtra(LocationManager.KEY_LOCATION_CHANGED, Location.class);
                    if (location != null) pendingFixes.add(location.getProvider());
                }
            };
            PendingIntent pending = new PendingIntent(sender);
            remote.registerLocationPendingIntent(GPS, request(), pending, PKG, null);
            remote.registerLocationPendingIntent(NETWORK, request(), pending, PKG, null);
            check(h.count() == 2, "PendingIntent parcel identity retains both providers");
            h.report(GPS); h.report(NETWORK);
            check(pendingFixes.equals(List.of(GPS, NETWORK)), "parceled PendingIntent receives both providers");
            remote.unregisterLocationPendingIntent(new PendingIntent(sender));
            check(h.count() == 0, "PendingIntent parcel identity unregisters both providers");
            AtomicInteger finalUpdates = new AtomicInteger();
            manager.requestLocationUpdates(GPS, new LocationRequest.Builder(0).setMaxUpdates(1).build(),
                    Runnable::run, (LocationListener) location -> {
                        if (finalUpdates.incrementAndGet() == 1) h.report(GPS);
                    });
            h.report(GPS);
            check(finalUpdates.get() == 1 && h.count() == 0,
                    "public maxUpdates request claims retirement before reentrant provider delivery");
            synchronized (h.lock) { ((Map<?,?>)get(h.service, "mLast")).clear(); }
            List<Location> current = new ArrayList<>();
            manager.getCurrentLocation(GPS, request(), null, Runnable::run, current::add);
            check(h.count() == 1, "public current request reaches service registration through AIDL");
            h.report(GPS); h.report(GPS);
            check(current.size() == 1 && current.get(0) != null && h.count() == 0,
                    "public current callback completes once and retires through AIDL");
            synchronized (h.lock) { ((Map<?,?>)get(h.service, "mLast")).clear(); }
            current.clear();
            manager.getCurrentLocation(GPS, request(), null, Runnable::run, current::add);
            set(h.record(), "expiresAt", SystemClock.elapsedRealtime() - 1);
            h.report(GPS);
            check(current.size() == 1 && current.get(0) == null && h.count() == 0,
                    "public current callback receives its null terminal result after expiry");
        }
    }
    public static void contracts(String selected, boolean binder) throws Exception {
        checks.set(0);
        long identity = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity((1000L << 32) | 910006);
            if (selected.equals("all") || selected.equals("listeners")) listeners();
            if (selected.equals("all") || selected.equals("pending")) pendingIntents();
            if (selected.equals("all") || selected.equals("failures")) failures();
            if (selected.equals("all") || selected.equals("deaths")) deaths();
            if (selected.equals("all") || selected.equals("current")) currentAndOtherClients();
            if (selected.equals("all") || selected.equals("terminal")) terminal();
            if (selected.equals("all") || selected.equals("concurrent")) concurrent();
            if (selected.equals("all") || selected.equals("expiry")) expiry();
            if (selected.equals("all") || selected.equals("current-deaths")) currentDeaths();
            if (selected.equals("all") || selected.equals("disabled")) disabled();
            if (selected.equals("all") || selected.equals("initial-disabled")) initialDisabledNotifications();
            if (selected.equals("late")) lateCurrent();
            if (selected.equals("duration")) durationLimits();
            if (binder && (selected.equals("all") || selected.equals("binder"))) binderAndManager();
            System.out.println("LocationRegistrationContractTest all passed: " + checks + "; case=" + selected);
        } finally { Binder.restoreCallingIdentity(identity); }
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        contracts(args.length == 0 ? "all" : args[0], true);
    }
}
