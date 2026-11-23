/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.view.textservice.SentenceSuggestionsInfo;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;
import android.view.textservice.SuggestionsInfo;
import android.view.textservice.TextInfo;

import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.android.internal.textservice.ISpellCheckerService;
import com.android.internal.textservice.ISpellCheckerServiceCallback;
import com.android.internal.textservice.ISpellCheckerSession;
import com.android.internal.textservice.ISpellCheckerSessionListener;
import com.android.internal.textservice.ITextServicesSessionListener;
import com.gesher.server.ApkPackage;

import java.lang.ref.WeakReference;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Actual selected-provider session manager and ActiveServices, with an explicit fixture provider. */
public final class SpellCheckerBindingContractTest {
    private static final String PACKAGE = "fixture.spell.bindings";
    private static final int UID = 12841, PID = 884721;
    private static final AtomicInteger CHECKS = new AtomicInteger();
    private static boolean parcels;
    private static final Unsafe U;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true);
            U = (Unsafe) field.get(null);
        } catch (ReflectiveOperationException e) { throw new ExceptionInInitializerError(e); }
    }
    private interface Action { void run() throws Exception; }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        CHECKS.incrementAndGet();
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> owner, String name) throws Exception {
        Field value = owner.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static Object get(Object owner, String name) throws Exception { return field(owner.getClass(), name).get(owner); }
    private static void set(Object owner, String name, Object value) throws Exception { field(owner.getClass(), name).set(owner, value); }
    private static Object construct(Class<?> owner, Class<?>[] args, Object... values) throws Exception {
        Constructor<?> constructor = owner.getDeclaredConstructor(args); constructor.setAccessible(true);
        try { return constructor.newInstance(values); }
        catch (InvocationTargetException e) { return rethrow(e.getCause()); }
    }
    private static Object invoke(Object owner, String name, Class<?>[] args, Object... values) throws Exception {
        Method method = owner.getClass().getDeclaredMethod(name, args); method.setAccessible(true);
        try { return method.invoke(owner, values); }
        catch (InvocationTargetException e) { return rethrow(e.getCause()); }
    }
    private static Object rethrow(Throwable error) throws Exception {
        if (error instanceof Error e) throw e;
        if (error instanceof Exception e) throw e;
        throw new AssertionError(error);
    }
    private static ComponentName component(String suffix) { return new ComponentName(PACKAGE, PACKAGE + "." + suffix); }
    private static IBinder wire(IBinder target) {
        if (!parcels) return target;
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, data, reply, flags); }
        };
    }

    /** Controlled death delivery; Binder payloads still use the real generated interfaces. */
    private static final class Life {
        final Set<IBinder.DeathRecipient> owners = new LinkedHashSet<>();
        int links, unlinks;
        boolean alive = true;
        synchronized void link(IBinder.DeathRecipient owner) throws RemoteException {
            if (!alive) throw new RemoteException("Fixture binder is dead");
            links++; owners.add(owner);
        }
        synchronized boolean unlink(IBinder.DeathRecipient owner) { unlinks++; return owners.remove(owner); }
        void die() {
            List<IBinder.DeathRecipient> callbacks;
            synchronized (this) { alive = false; callbacks = new ArrayList<>(owners); owners.clear(); }
            for (IBinder.DeathRecipient callback : callbacks) callback.binderDied();
        }
        @SuppressWarnings("unchecked")
        private static <T extends Throwable> void raise(Throwable error) throws T { throw (T) error; }
        IBinder binder(IBinder actual) {
            // A real Binder subclass is required by native Parcel.writeStrongBinder;
            // an arbitrary Java IBinder proxy is not a native Binder transport.
            return new Binder() {
                @Override public IInterface queryLocalInterface(String descriptor) {
                    return parcels ? null : actual.queryLocalInterface(descriptor);
                }
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                        throws RemoteException { return actual.transact(code, data, reply, flags); }
                @Override public void linkToDeath(IBinder.DeathRecipient owner, int flags) {
                    try { link(owner); }
                    catch (RemoteException failure) { Life.<RuntimeException>raise(failure); }
                }
                @Override public boolean unlinkToDeath(IBinder.DeathRecipient owner, int flags) { return unlink(owner); }
                @Override public boolean isBinderAlive() { return alive; }
                @Override public boolean pingBinder() { return alive; }
            };
        }
    }

    private static final class Client {
        final int pid;
        final Life life = new Life();
        final IBinder token;
        final ISpellCheckerSessionListener listener;
        final ITextServicesSessionListener ready;
        final List<ISpellCheckerSession> delivered = new ArrayList<>();
        final AtomicInteger replies = new AtomicInteger();
        final AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
        boolean failReady;
        Action onReady;
        Client(int pid) {
            this.pid = pid;
            ISpellCheckerSessionListener endpoint = new ISpellCheckerSessionListener.Stub() {
                @Override public IBinder asBinder() { return token; }
                @Override public void onGetSuggestions(SuggestionsInfo[] values) { replies.incrementAndGet(); }
                @Override public void onGetSentenceSuggestions(SentenceSuggestionsInfo[] values) { replies.incrementAndGet(); }
            };
            token = life.binder((Binder) endpoint);
            listener = ISpellCheckerSessionListener.Stub.asInterface(token);
            ITextServicesSessionListener callback = new ITextServicesSessionListener.Stub() {
                @Override public void onServiceConnected(ISpellCheckerSession session) throws RemoteException {
                    if (failReady) throw new RemoteException("Fixture client callback failure");
                    delivered.add(session);
                    if (onReady != null) {
                        try { onReady.run(); }
                        catch (Throwable e) {
                            callbackFailure.compareAndSet(null, e);
                            throw new IllegalStateException("Fixture ready callback failed", e);
                        }
                    }
                }
            };
            ready = ITextServicesSessionListener.Stub.asInterface(wire(callback.asBinder()));
        }
    }

    private static final class Session extends ISpellCheckerSession.Stub {
        final ISpellCheckerSessionListener listener;
        int closes, cancels, words, sentences;
        Session(ISpellCheckerSessionListener listener) { this.listener = listener; }
        @Override public void onGetSuggestionsMultiple(TextInfo[] texts, int limit, boolean sequential)
                throws RemoteException { words++; listener.onGetSuggestions(new SuggestionsInfo[0]); }
        @Override public void onGetSentenceSuggestionsMultiple(TextInfo[] texts, int limit)
                throws RemoteException { sentences++; listener.onGetSentenceSuggestions(new SentenceSuggestionsInfo[0]); }
        @Override public void onCancel() { cancels++; }
        @Override public void onClose() { closes++; }
    }
    private static final class Call {
        final String locale;
        final Bundle extras;
        final int attributes;
        final ISpellCheckerServiceCallback callback;
        final Session session;
        Call(String locale, ISpellCheckerSessionListener listener, Bundle extras, int attributes,
                ISpellCheckerServiceCallback callback) {
            this.locale = locale; this.extras = extras; this.attributes = attributes; this.callback = callback;
            session = new Session(listener);
        }
        void complete() throws RemoteException {
            callback.onSessionCreated(ISpellCheckerSession.Stub.asInterface(wire(session.asBinder())));
        }
    }
    private static final class Provider extends ISpellCheckerService.Stub {
        final Life life = new Life();
        final IBinder token = life.binder(this);
        final List<Call> calls = new ArrayList<>();
        boolean failRemote, failRuntime, automatic;
        @Override public IBinder asBinder() { return token; }
        @Override public void getISpellCheckerSession(String locale, ISpellCheckerSessionListener listener,
                Bundle extras, int attributes, ISpellCheckerServiceCallback callback) throws RemoteException {
            if (failRemote) throw new RemoteException("Fixture provider failure");
            if (failRuntime) throw new IllegalArgumentException("Fixture request rejection");
            Call call = new Call(locale, listener, extras, attributes, callback); calls.add(call);
            if (automatic) call.complete();
        }
    }
    private record Bind(IBinder service, IBinder token, ComponentName component) {}
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder token = new Binder();
        final List<Bind> binds = new ArrayList<>();
        int creates, stops, unbinds;
        RuntimeException failBind;
        @Override public IBinder asBinder() { return token; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compatibility, int processState) { creates++; }
        @Override public void scheduleBindService(IBinder token, IBinder binding, Intent intent,
                boolean rebind, int processState, long sequence) {
            binds.add(new Bind(token, binding, intent.getComponent()));
            if (failBind != null) throw failBind;
        }
        @Override public void scheduleUnbindService(IBinder token, IBinder binding, Intent intent) { unbinds++; }
        @Override public void scheduleStopService(IBinder token) { stops++; }
    }

    private static ApkPackage code() throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(PACKAGE, "/fixture/spell/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String suffix : List.of("English", "French")) {
            ParsedServiceImpl service = new ParsedServiceImpl();
            service.setPackageName(PACKAGE); service.setName(component(suffix).getClassName());
            service.setEnabled(true); service.setExported(true); service.setProcessName(PACKAGE);
            service.setPermission(Manifest.permission.BIND_TEXT_SERVICE);
            parsed.addService(service);
        }
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage) U.allocateInstance(ApkPackage.class);
        set(result, "packageName", PACKAGE); set(result, "parsed", parsed); set(result, "mUid", UID);
        set(result, "apkPath", "/fixture/spell/base.apk"); set(result, "mDataDir", "/fixture/spell/data");
        set(result, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        return result;
    }
    private static Object entry(ApkPackage owner, String suffix) throws Exception {
        ResolveInfo resolve = new ResolveInfo(); resolve.serviceInfo = owner.serviceInfo(component(suffix).getClassName(), 0);
        SpellCheckerInfo info;
        List<SpellCheckerSubtype> subtypes = List.of(new SpellCheckerSubtype(0, "en_US", "en-US", "", 1));
        if (parcels) {
            Method method = type("SpellCheckerCatalog").getDeclaredMethod("checkerInfo", ResolveInfo.class,
                    int.class, String.class, List.class); method.setAccessible(true);
            info = (SpellCheckerInfo) method.invoke(null, resolve, 0, null, subtypes);
        } else {
            // Host cannot parse an Android Parcel. Only selected manifest metadata is constructed.
            info = (SpellCheckerInfo) U.allocateInstance(SpellCheckerInfo.class);
            set(info, "mService", resolve); set(info, "mId", component(suffix).flattenToShortString());
            set(info, "mSubtypes", new ArrayList<>(subtypes));
        }
        return construct(type("SpellCheckerCatalog$Entry"), new Class<?>[]{ApkPackage.class, SpellCheckerInfo.class}, owner, info);
    }

    private static final class Harness implements AutoCloseable {
        final Object packages, atm, active, catalog, sessions;
        final IActivityManager activity;
        final ScheduledThreadPoolExecutor worker;
        AppThread app = new AppThread();
        final List<Client> clients = new ArrayList<>();
        final Set<String> started = new LinkedHashSet<>();
        ApkPackage code = code();
        Map<String, Object> entries;
        Object process;
        Harness() throws Exception {
            Class<?> pm = type("PackageManagerService"), at = type("ActivityTaskManagerService");
            packages = construct(pm, new Class<?>[]{Collection.class}, List.of(code));
            atm = construct(at, new Class<?>[]{pm}, packages);
            activity = (IActivityManager) construct(type("ActivityManagerService"),
                    new Class<?>[]{at, pm, type("NotificationManagerService")}, atm, packages, null);
            active = get(activity, "mServices");
            catalog = construct(type("SpellCheckerCatalog"), new Class<?>[]{pm}, packages);
            sessions = construct(type("SpellCheckerSessions"),
                    new Class<?>[]{type("ActivityManagerService"), type("SpellCheckerCatalog")}, activity, catalog);
            worker = (ScheduledThreadPoolExecutor) get(sessions, "mWorker");
            installProcess(code);
            entries = Map.of("English", entry(code, "English"), "French", entry(code, "French"));
        }
        @SuppressWarnings("unchecked") void installProcess(ApkPackage owner) throws Exception {
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | PID);
                process = construct(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{type("ActivityTaskManagerService"), int.class, IApplicationThread.class}, atm, PID, app);
            } finally { Binder.restoreCallingIdentity(identity); }
            set(process, "packageName", PACKAGE); set(process, "processName", PACKAGE); set(process, "ready", true);
            set(process, "packageCode", owner);
            ((List<Object>) get(atm, "mProcesses")).add(process);
        }
        Client client(int pid) { Client client = new Client(pid); clients.add(client); return client; }
        void request(Client client, String suffix, String locale, Bundle extras, int attributes) throws Exception {
            invoke(sessions, "request", new Class<?>[]{type("SpellCheckerCatalog$Entry"), int.class, String.class,
                    ITextServicesSessionListener.class, ISpellCheckerSessionListener.class, Bundle.class, int.class},
                    entries.get(suffix), client.pid, locale, client.ready, client.listener, extras, attributes);
        }
        void request(Client client) throws Exception { request(client, "English", "en_US", null, 0); }
        void finish(Client client) throws Exception { finish(client, client.pid); }
        void finish(Client client, int pid) throws Exception {
            invoke(sessions, "finish", new Class<?>[]{int.class, IBinder.class}, pid, client.token);
        }
        void drain() throws Exception {
            // Admission, connection, creation and reentrant finish are separate owned queue turns.
            for (int i = 0; i < 4; i++) worker.submit(() -> { }).get(5, TimeUnit.SECONDS);
            for (Client client : clients) {
                if (client.callbackFailure.get() != null) rethrow(client.callbackFailure.get());
            }
        }
        void publish(String suffix, IBinder binder) throws Exception {
            for (int i = app.binds.size() - 1; i >= 0; i--) {
                Bind bind = app.binds.get(i);
                if (bind.component.equals(component(suffix))) {
                    activity.publishService(bind.service, bind.token, binder); drain(); return;
                }
            }
            throw new AssertionError("No actual provider bind " + suffix);
        }
        void publish(Provider provider) throws Exception { publish("English", provider.token); }
        int connections() throws Exception { return ((Map<?, ?>) get(active, "mConnections")).size(); }
        int requests() throws Exception { return worker.submit(() -> ((Map<?, ?>) get(sessions, "mRequests")).size()).get(5, TimeUnit.SECONDS); }
        long workerId() throws Exception { return worker.submit(() -> Thread.currentThread().getId()).get(5, TimeUnit.SECONDS); }
        void keepAlive(String suffix) throws Exception {
            check(component(suffix).equals(activity.startService(null,
                    new Intent("fixture-keep-alive").setComponent(component(suffix)), null,
                    false, "android", null, 0)), "Actual started service did not retain its process");
            started.add(suffix);
        }
        @SuppressWarnings("unchecked") void replaceOwner() throws Exception {
            set(process, "dead", true);
            ((List<Object>) get(atm, "mProcesses")).remove(process);
            code = code();
            invoke(packages, "add", new Class<?>[]{ApkPackage.class}, code);
            app = new AppThread(); installProcess(code);
            entries = Map.of("English", entry(code, "English"), "French", entry(code, "French"));
        }
        void removing() throws Exception {
            invoke(sessions, "packageRemoving", new Class<?>[]{String.class}, PACKAGE);
        }
        WeakReference<Object> requestReference(Client client) throws Exception {
            return worker.submit(() -> new WeakReference<>(((Map<?, ?>) get(sessions, "mRequests")).get(client.token)))
                    .get(5, TimeUnit.SECONDS);
        }
        @Override public void close() throws Exception {
            for (Client client : clients) finish(client);
            drain(); check(requests() == 0 && connections() == 0, "Fixture left owned sessions or actual connections");
            for (String suffix : started) {
                activity.stopService(null, new Intent("fixture-keep-alive").setComponent(component(suffix)), null, 0);
            }
            worker.shutdownNow();
            ((java.util.concurrent.ExecutorService) get(active, "mRestarts")).shutdownNow();
        }
    }

    private static final class PausedWorker implements AutoCloseable {
        final CountDownLatch release = new CountDownLatch(1);
        final java.util.concurrent.Future<?> task;
        PausedWorker(ScheduledThreadPoolExecutor worker) throws Exception {
            CountDownLatch entered = new CountDownLatch(1);
            task = worker.submit(() -> {
                entered.countDown();
                try {
                    if (!release.await(10, TimeUnit.SECONDS)) throw new AssertionError("Fixture worker pause timed out");
                } catch (InterruptedException e) { throw new IllegalStateException(e); }
            });
            check(entered.await(5, TimeUnit.SECONDS), "Fixture worker did not reach the owned pause");
        }
        @Override public void close() throws Exception { release.countDown(); task.get(5, TimeUnit.SECONDS); }
    }

    private static void independentSessions() throws Exception {
        try (Harness h = new Harness()) {
            Client first = h.client(PID + 1), second = h.client(PID + 2);
            Bundle extras = new Bundle(); extras.putInt("value", 73);
            h.request(first, "English", "en_US", extras, 3); extras.putInt("value", 99);
            h.request(second, "English", "fr_FR", null, 7); h.drain();
            check(h.connections() == 1 && h.app.binds.size() == 1, "Two pending sessions did not share the actual provider connection");
            Provider provider = new Provider(); h.publish(provider);
            check(provider.life.links == 1 && provider.calls.size() == 2, "Provider link or session dispatch count changed");
            Call a = provider.calls.get(0), b = provider.calls.get(1);
            check(a.locale.equals("en_US") && a.attributes == 3 && a.extras.getInt("value") == 73,
                    "First request metadata was mixed or not snapshotted");
            check(b.locale.equals("fr_FR") && b.attributes == 7 && b.extras == null, "Second request metadata was mixed");
            b.complete(); h.drain();
            check(second.delivered.size() == 1 && first.delivered.isEmpty(), "Out-of-order callback reached the wrong client");
            a.complete(); h.drain();
            check(first.delivered.size() == 1, "First session callback disappeared");
            first.delivered.get(0).onCancel();
            first.delivered.get(0).onClose(); h.finish(first); h.drain();
            check(a.session.cancels == 1 && a.session.closes == 1 && b.session.closes == 0,
                    "Cancel/finish affected another client's session or doubled onClose");
            check(h.connections() == 1 && provider.life.unlinks == 0, "First client closed the shared provider binding");
            second.delivered.get(0).onGetSuggestionsMultiple(new TextInfo[0], 3, false);
            second.delivered.get(0).onGetSentenceSuggestionsMultiple(new TextInfo[0], 4);
            check(second.replies.get() == 2 && first.replies.get() == 0, "Surviving session stopped routing its own replies");
            h.finish(second, first.pid); h.drain();
            check(h.requests() == 1, "Another PID could finish the surviving session");
            second.delivered.get(0).onClose(); h.finish(second); h.drain();
            check(h.connections() == 0 && provider.life.unlinks == 1 && h.app.unbinds == 1,
                    "Last client did not unbind and unlink exactly once");
            a.complete(); b.complete(); h.drain();
            check(first.delivered.size() == 1 && second.delivered.size() == 1
                    && a.session.closes == 1 && b.session.closes == 1, "Duplicate callbacks changed completed session ownership");
        }
    }

    private static void pendingAndLate() throws Exception {
        try (Harness h = new Harness()) {
            Client canceled = h.client(PID + 1), live = h.client(PID + 2);
            h.request(canceled); h.request(live); h.finish(canceled); h.drain();
            Provider provider = new Provider(); h.publish(provider);
            check(provider.calls.size() == 1 && canceled.delivered.isEmpty(), "Finished pending request reached the provider");
            h.finish(live); h.drain();
            provider.calls.get(0).complete(); h.drain();
            check(live.delivered.isEmpty() && provider.calls.get(0).session.closes == 1,
                    "Late creation escaped retirement instead of closing its result");
            check(h.connections() == 0 && provider.life.unlinks == 1, "Canceled pending group retained its provider");
        }
        try (Harness h = new Harness()) {
            Client unavailable = h.client(PID + 1), live = h.client(PID + 2);
            h.request(unavailable); h.request(live); h.drain();
            Provider provider = new Provider(); h.publish(provider);
            provider.calls.get(0).callback.onSessionCreated(null); h.drain();
            check(unavailable.delivered.isEmpty() && unavailable.life.unlinks == 1
                    && h.requests() == 1 && h.connections() == 1,
                    "Null session result fabricated success or retired the surviving request");
            provider.calls.get(1).complete(); h.drain();
            check(live.delivered.size() == 1 && provider.life.links == 1 && provider.life.unlinks == 0,
                    "Null session result disrupted the independent provider binding");
        }
    }

    private static void isolatedProvidersAndDeath() throws Exception {
        try (Harness h = new Harness()) {
            Client first = h.client(PID + 1), second = h.client(PID + 2), other = h.client(PID + 3);
            h.request(first); h.request(second); h.request(other, "French", "fr_FR", null, 0); h.drain();
            Provider a = new Provider(), b = new Provider(); h.publish(a); h.publish("French", b.token);
            check(h.connections() == 2, "Different components sharing a UID were combined");
            for (Call call : a.calls) call.complete(); for (Call call : b.calls) call.complete(); h.drain();
            first.life.die(); h.drain();
            check(h.requests() == 2 && h.connections() == 2 && a.calls.get(0).session.closes == 1,
                    "Client death retired another owner or left its own session alive");
            a.life.die(); h.drain();
            check(h.requests() == 1 && h.connections() == 1 && a.calls.get(1).session.closes == 1
                    && b.calls.get(0).session.closes == 0, "Provider death crossed the component boundary");
            other.delivered.get(0).onCancel();
            check(b.calls.get(0).session.cancels == 1, "Unaffected provider stopped serving its session");
        }
    }

    private static void failuresAndReentry() throws Exception {
        try (Harness h = new Harness()) {
            Client dead = h.client(PID + 1); dead.life.die(); h.request(dead); h.drain();
            check(h.requests() == 0 && h.connections() == 0 && h.app.creates == 0,
                    "Already-dead listener created a provider binding");
            Client one = h.client(PID + 2), two = h.client(PID + 3);
            h.request(one); h.request(two); h.drain(); h.publish("English", null);
            check(h.requests() == 0 && h.connections() == 0 && one.delivered.isEmpty() && two.delivered.isEmpty(),
                    "Null provider binding invented a result or retained clients");
        }
        try (Harness h = new Harness()) {
            Client one = h.client(PID + 1), two = h.client(PID + 2);
            h.request(one); h.request(two); h.drain();
            Provider provider = new Provider(); provider.life.die(); h.publish(provider);
            check(h.requests() == 0 && h.connections() == 0 && provider.calls.isEmpty(),
                    "Provider death during link left pending clients or dispatched work");
        }
        try (Harness h = new Harness()) {
            Client live = h.client(PID + 1), failed = h.client(PID + 2), immediate = h.client(PID + 3);
            h.request(live); h.drain(); Provider provider = new Provider(); provider.automatic = true; h.publish(provider);
            failed.failReady = true; h.request(failed); h.drain();
            check(live.delivered.size() == 1 && failed.delivered.isEmpty()
                    && provider.calls.get(1).session.closes == 1 && provider.calls.get(0).session.closes == 0,
                    "Failing client callback retired another session or leaked its result");
            immediate.onReady = () -> { immediate.delivered.get(0).onClose(); h.finish(immediate); };
            h.request(immediate); h.drain();
            check(immediate.delivered.size() == 1 && provider.calls.get(2).session.closes == 1 && h.requests() == 1,
                    "Synchronous creation/reentrant client close lost ownership");
            provider.failRuntime = true; Client invalid = h.client(PID + 4); h.request(invalid); h.drain();
            check(h.requests() == 1 && h.connections() == 1, "A provider's request error retired an unrelated active session");
            provider.failRuntime = false; provider.failRemote = true;
            Client disconnected = h.client(PID + 5); h.request(disconnected); h.drain();
            check(h.requests() == 0 && h.connections() == 0 && provider.calls.get(0).session.closes == 1,
                    "Provider transport failure retained the shared group");
        }
        try (Harness h = new Harness()) {
            h.app.failBind = new IllegalStateException("Fixture publication failure");
            Client client = h.client(PID + 1); h.request(client); h.drain();
            check(h.requests() == 0 && h.connections() == 0 && client.life.unlinks == 1,
                    "Partially published failed binding retained its owner");
        }
    }

    private static void lastOwnerAndLateGeneration() throws Exception {
        try (Harness h = new Harness()) {
            h.keepAlive("English");
            Client reused = h.client(PID + 1); h.request(reused); h.drain();
            Object connection = ((Map<?, ?>) get(h.active, "mConnections")).keySet().iterator().next();
            IBinder.DeathRecipient oldClientDeath = reused.life.owners.iterator().next();
            Provider provider = new Provider(); h.publish(provider);
            Call old = provider.calls.get(0); h.finish(reused); h.drain();
            check(h.connections() == 0 && provider.life.unlinks == 1, "Last owner retained an idle provider connection");
            h.request(reused); h.drain();
            check(h.connections() == 1 && provider.life.links == 2 && provider.calls.size() == 2,
                    "New admission did not acquire a new group after final release");
            provider.calls.get(1).complete(); old.complete(); oldClientDeath.binderDied();
            android.app.IServiceConnection.Stub.asInterface((IBinder) connection)
                    .connected(component("English"), null, null, true);
            h.drain();
            check(reused.delivered.size() == 1 && old.session.closes == 1
                    && provider.calls.get(1).session.closes == 0 && h.requests() == 1,
                    "Late prior callback/death/connection retired the new generation of a listener");
            h.finish(reused); h.drain();
            check(((Map<?, ?>) get(h.sessions, "mBindings")).isEmpty() && h.worker.getQueue().isEmpty(),
                    "Empty provider group retained an owner or idle timer");
        }
    }

    private static void concurrentRetirement() throws Exception {
        try (Harness h = new Harness()) {
            h.keepAlive("English");
            for (int i = 0; i < 48; i++) h.client(PID + 1 + i);
            java.util.concurrent.ExecutorService callers = java.util.concurrent.Executors.newFixedThreadPool(4);
            try {
                List<java.util.concurrent.Future<?>> pending = new ArrayList<>();
                for (Client client : h.clients) {
                    pending.add(callers.submit(() -> {
                        try { h.request(client); h.finish(client); }
                        catch (Exception e) { throw new IllegalStateException(e); }
                    }));
                }
                for (java.util.concurrent.Future<?> future : pending) future.get(5, TimeUnit.SECONDS);
            } finally { callers.shutdownNow(); }
            h.drain();
            check(h.requests() == 0 && h.connections() == 0 && h.app.unbinds > 0,
                    "Concurrent request/finish ordering leaked provider ownership");
            for (Client client : h.clients) {
                check(client.life.links == 1 && client.life.unlinks == 1 && client.delivered.isEmpty(),
                        "Concurrent retirement lost or duplicated a listener lifetime");
            }
        }
    }

    private static void installedGenerationAndRemoval() throws Exception {
        try (Harness h = new Harness()) {
            Client old = h.client(PID + 1); h.request(old); h.drain();
            Provider oldProvider = new Provider(); h.publish(oldProvider);
            Client replacement = h.client(PID + 2);
            try (PausedWorker pause = new PausedWorker(h.worker)) {
                h.removing();
                // Model preemption between capturing the retiring package and submitting
                // its task: preserve the actual queued task, then submit it after a newer
                // admission. No production removal callback or predicate is substituted.
                Runnable delayedRemoval = h.worker.getQueue().poll();
                check(delayedRemoval != null && h.worker.getQueue().isEmpty(), "Removal task was not isolated");
                h.replaceOwner(); h.request(replacement);
                h.worker.execute(delayedRemoval);
            }
            h.drain();
            check(h.requests() == 1 && h.connections() == 1 && old.life.unlinks == 1,
                    "Late old-package removal retired the new provider generation or retained the old one");
            Provider current = new Provider(); h.publish(current);
            oldProvider.calls.get(0).complete(); current.calls.get(0).complete(); h.drain();
            check(old.delivered.isEmpty() && oldProvider.calls.get(0).session.closes == 1
                    && replacement.delivered.size() == 1 && current.calls.get(0).session.closes == 0,
                    "Replacement mixed provider generations or delivered a stale result");
            invoke(h.packages, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.code);
            try {
                h.removing(); h.drain();
                check(h.requests() == 0 && h.connections() == 0 && current.calls.get(0).session.closes == 1,
                        "Actual removal-pending owner kept its sessions or provider connection");
                Client denied = h.client(PID + 3); h.request(denied); h.drain();
                check(denied.life.links == 0 && h.connections() == 0, "Removal-pending provider admitted new work");
            } finally { invoke(h.packages, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.code); }
        }
    }

    private static void retainedCallbackDoesNotRetainClient() throws Exception {
        try (Harness h = new Harness()) {
            Client first = h.client(PID + 1), retained = h.client(PID + 2);
            h.request(first); h.request(retained); h.drain();
            Provider provider = new Provider(); h.publish(provider);
            WeakReference<Object> request = h.requestReference(first);
            check(!request.refersTo(null), "No production request was observed before retirement");
            h.finish(first); h.drain();
            // The provider deliberately keeps the actual creation callback alive. The
            // other client keeps the group and its real ActiveServices lifetime alive.
            // refersTo does not load the request into an interpreter GC root slot.
            for (int i = 0; i < 30 && !request.refersTo(null); i++) {
                System.gc(); Thread.sleep(10);
            }
            check(request.refersTo(null), "Finished first client retained by callback or shared binding lifetime");
            provider.calls.get(0).complete(); provider.calls.get(1).complete(); h.drain();
            check(first.delivered.isEmpty() && provider.calls.get(0).session.closes == 1
                    && retained.delivered.size() == 1 && h.connections() == 1,
                    "Collected request's late result escaped cleanup or affected the surviving client");
        }
    }

    public static int contracts(boolean realParcels) throws Exception {
        parcels = realParcels; CHECKS.set(0);
        independentSessions(); pendingAndLate(); isolatedProvidersAndDeath();
        failuresAndReentry(); lastOwnerAndLateGeneration(); concurrentRetirement();
        installedGenerationAndRemoval(); retainedCallbackDoesNotRetainClient();
        System.out.println("SpellCheckerBindingContractTest passed: " + CHECKS.get() + "; real_parcel=" + parcels);
        return CHECKS.get();
    }

    public interface Sample { long[] read(long workerId); }
    /** Work counters observe actual controller records and death registrations, not substitute binds. */
    public static String work(int count, boolean expectShared, Sample sample) throws Exception {
        parcels = false;
        try (Harness h = new Harness()) {
            for (int i = 0; i < count; i++) h.client(PID + 1 + i);
            long workerId = h.workerId();
            long[] before = sample.read(workerId); long started = System.nanoTime();
            for (Client client : h.clients) h.request(client);
            h.drain(); int connections = h.connections();
            Provider provider = new Provider(); h.publish(provider);
            for (Call call : provider.calls) call.complete(); h.drain();
            long elapsed = System.nanoTime() - started; long[] after = sample.read(workerId);
            int delivered = h.clients.stream().mapToInt(client -> client.delivered.size()).sum();
            check(connections == (expectShared ? 1 : count), "Actual connection resource bound failed: expected "
                    + (expectShared ? 1 : count) + ", observed " + connections);
            check(provider.life.links == connections && provider.calls.size() == count && delivered == count,
                    "Resource reduction lost independent sessions or death ownership");
            check(h.app.binds.size() == 1 && h.app.creates == 1, "Actual service instance was not already shared");
            return "{\"workload\":\"concurrent\",\"sessions\":" + count + ",\"connections\":" + connections
                    + ",\"provider_death_links\":" + provider.life.links + ",\"provider_session_calls\":" + provider.calls.size()
                    + ",\"delivered_sessions\":" + delivered + ",\"service_on_bind_calls\":" + h.app.binds.size()
                    + ",\"allocated_bytes\":" + (after[0] - before[0]) + ",\"thread_cpu_ns\":" + (after[1] - before[1])
                    + ",\"elapsed_ns\":" + elapsed + "}";
        }
    }

    /** Repeated short-lived sessions while one independent client still owns the provider. */
    public static String churn(int count, boolean expectShared, Sample sample) throws Exception {
        parcels = false;
        try (Harness h = new Harness()) {
            Client anchor = h.client(PID + 1); h.request(anchor); h.drain();
            Provider provider = new Provider(); provider.automatic = true; h.publish(provider);
            for (int i = 0; i < count; i++) h.client(PID + 2 + i);
            long workerId = h.workerId();
            long[] before = sample.read(workerId); long started = System.nanoTime();
            int peak = h.connections();
            for (int i = 1; i < h.clients.size(); i++) {
                Client client = h.clients.get(i);
                h.request(client); h.drain();
                peak = Math.max(peak, h.connections());
                check(client.delivered.size() == 1, "Repeated session was not independently delivered");
                client.delivered.get(0).onClose(); h.finish(client); h.drain();
                check(h.requests() == 1 && h.connections() == 1, "Repeated retirement lost the independent anchor");
            }
            long elapsed = System.nanoTime() - started; long[] after = sample.read(workerId);
            check(peak == (expectShared ? 1 : 2) && provider.life.links == (expectShared ? 1 : count + 1)
                    && provider.life.unlinks == (expectShared ? 0 : count), "Repeated connection ownership bound failed");
            check(provider.calls.size() == count + 1 && provider.calls.get(0).session.closes == 0,
                    "Repeated creation or anchor session ownership changed");
            for (int i = 1; i < provider.calls.size(); i++) {
                check(provider.calls.get(i).session.closes == 1, "Repeated close was lost or duplicated");
            }
            check(h.app.binds.size() == 1 && h.app.creates == 1, "Repeated workload changed the actual service instance count");
            return "{\"workload\":\"churn\",\"sessions\":" + count + ",\"anchor_sessions\":1,\"peak_connections\":" + peak
                    + ",\"provider_death_links\":" + provider.life.links + ",\"provider_death_unlinks\":" + provider.life.unlinks
                    + ",\"provider_session_calls\":" + provider.calls.size() + ",\"service_on_bind_calls\":" + h.app.binds.size()
                    + ",\"allocated_bytes\":" + (after[0] - before[0]) + ",\"thread_cpu_ns\":" + (after[1] - before[1])
                    + ",\"elapsed_ns\":" + elapsed + "}";
        }
    }
}
