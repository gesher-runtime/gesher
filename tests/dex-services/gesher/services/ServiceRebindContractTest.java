/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Production ActiveServices lifecycle and LoadedApk ServiceConnection contracts. */
public final class ServiceRebindContractTest {
    private static final String OWNER = "fixture.rebinding.owner";
    private static final int PID = 884241, UID = 12622;
    private static final ComponentName SERVICE = new ComponentName(OWNER, OWNER + ".BoundService");
    private static final Unsafe UNSAFE;
    private static int checks;
    private static boolean binder;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true);
            UNSAFE = (Unsafe) field.get(null);
        } catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Operation { void run() throws Exception; }
    private interface BindOperation { void run(Bind bind) throws Exception; }
    private static void check(boolean okay, String reason) {
        if (!okay) throw new AssertionError(reason);
        checks++;
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Object get(Object object, String name) throws Exception { return field(object.getClass(), name).get(object); }
    private static void set(Object object, String name, Object value) throws Exception { field(object.getClass(), name).set(object, value); }
    private static Object construct(Class<?> type, Class<?>[] parameters, Object... arguments) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(parameters); constructor.setAccessible(true);
        try { return constructor.newInstance(arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error error) throw error;
        if (failure instanceof Exception exception) throw exception;
        throw new AssertionError(failure);
    }
    private static IBinder remote(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, data, reply, flags); }
        };
    }
    private static ApkPackage code() throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(OWNER, "/fixture/rebinding/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        ParsedServiceImpl service = new ParsedServiceImpl();
        service.setPackageName(OWNER); service.setName(SERVICE.getClassName());
        service.setEnabled(true); service.setExported(true); service.setProcessName(OWNER);
        parsed.addService(service); parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", OWNER); set(code, "parsed", parsed); set(code, "mUid", UID);
        set(code, "apkPath", "/fixture/rebinding/base.apk"); set(code, "mDataDir", "/fixture/rebinding/data");
        set(code, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        return code;
    }
    private static final class Bind {
        final IBinder service, binding;
        final boolean rebind;
        Bind(IBinder service, IBinder binding, boolean rebind) { this.service = service; this.binding = binding; this.rebind = rebind; }
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        final List<Bind> binds = new ArrayList<>();
        int creates, stops, exits, unbinds;
        BindOperation onBind, onUnbind;
        RuntimeException bindFailure;
        final List<Bind> unbound = new ArrayList<>();
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compatibility, int processState) { creates++; }
        @Override public void scheduleBindService(IBinder token, IBinder bindToken, Intent intent,
                boolean rebind, int processState, long sequence) {
            Bind call = new Bind(token, bindToken, rebind); binds.add(call);
            invoke(onBind, call);
            if (bindFailure != null) throw bindFailure;
        }
        @Override public void scheduleStopService(IBinder token) { stops++; }
        @Override public void scheduleUnbindService(IBinder token, IBinder bindToken, Intent intent) {
            unbinds++; Bind call = new Bind(token, bindToken, false); unbound.add(call);
            invoke(onUnbind, call);
        }
        @Override public void scheduleExit() { exits++; }
        private static void invoke(BindOperation operation, Bind bind) {
            if (operation == null) return;
            try { operation.run(bind); }
            catch (Exception failure) { throw new AssertionError("Application callback failed", failure); }
        }
    }
    private static final class Connection implements ServiceConnection {
        final Object dispatcher; // InnerConnection deliberately holds only a weak reference.
        final IServiceConnection callback;
        int nulls, connected, disconnected, died;
        IBinder value;
        Operation onNull, onConnected;
        Connection(Executor executor) throws Exception {
            Class<?> type = Class.forName("android.app.LoadedApk$ServiceDispatcher");
            dispatcher = construct(type, new Class<?>[]{ServiceConnection.class, Context.class,
                    Executor.class, long.class}, this, new ContextWrapper(null), executor, 0L);
            Method method = type.getDeclaredMethod("getIServiceConnection"); method.setAccessible(true);
            IServiceConnection local = (IServiceConnection) method.invoke(dispatcher);
            callback = binder ? IServiceConnection.Stub.asInterface(remote(local.asBinder())) : local;
        }
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            check(SERVICE.equals(name) && service != null, "Wrong service identity in connected callback");
            connected++; value = service;
            if (onConnected != null) try { onConnected.run(); }
            catch (Exception failure) { throw new AssertionError("Connected callback reentry failed", failure); }
        }
        @Override public void onServiceDisconnected(ComponentName name) { disconnected++; }
        @Override public void onBindingDied(ComponentName name) { died++; }
        @Override public void onNullBinding(ComponentName name) {
            check(SERVICE.equals(name), "Wrong service identity in null callback"); nulls++;
            if (onNull != null) try { onNull.run(); }
            catch (Exception failure) { throw new AssertionError("Null callback reentry failed", failure); }
        }
        void nullOnce() {
            check(nulls == 1 && connected == 0 && disconnected == 0 && died == 0,
                    "Published null binding did not invoke onNullBinding exactly once");
        }
    }
    private static final class Harness {
        final Object services;
        final IActivityManager am;
        AppThread thread = new AppThread();
        final Object atm;
        Object process;
        int pid = PID;
        @SuppressWarnings("unchecked")
        Harness() throws Exception {
            Class<?> pmType = type("PackageManagerService"), atmType = type("ActivityTaskManagerService");
            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            Object pm = construct(pmType, new Class<?>[]{Collection.class}, List.of(code()));
            atm = construct(atmType, new Class<?>[]{pmType}, pm);
            Object implementation = construct(type("ActivityManagerService"), new Class<?>[]{atmType,
                    pmType, type("NotificationManagerService")}, atm, pm, null);
            services = get(implementation, "mServices");
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | pid);
                process = construct(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class}, atm, PID, thread);
            } finally { Binder.restoreCallingIdentity(original); }
            set(process, "packageName", OWNER); set(process, "processName", OWNER); set(process, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(process);
            if (binder) {
                StandIn.setStrict(true);
                am = IActivityManager.Stub.asInterface(remote(
                        StandIn.of(IActivityManager.class, (IActivityManager) implementation).asBinder()));
            } else am = (IActivityManager) implementation;
        }
        void asApp(Operation action) throws Exception {
            asCaller(pid, action);
        }
        void asCaller(int callerPid, Operation action) throws Exception {
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | callerPid);
                action.run();
                check(Binder.getCallingPid() == callerPid && Binder.getCallingUid() == UID,
                        "Binding operation did not restore the app's calling identity");
            } finally { Binder.restoreCallingIdentity(original); }
        }
        void bind(Connection connection, String action, long flags) throws Exception {
            asApp(() -> check(am.bindService(thread, null, new Intent(action).setComponent(SERVICE), null,
                    connection.callback, flags, OWNER, 0) == 1, "Binding failed to resolve its real parsed service"));
        }
        void publish(int index, IBinder value) throws Exception {
            Bind bind = thread.binds.get(index);
            asApp(() -> am.publishService(bind.service, bind.binding, value));
        }
        void unbind(Connection connection) throws Exception {
            asApp(() -> check(am.unbindService(connection.callback), "Recorded binding could not be unbound"));
        }
        void start() throws Exception {
            asApp(() -> check(SERVICE.equals(am.startService(thread, new Intent("start").setComponent(SERVICE),
                    null, false, OWNER, null, 0)), "Started service did not resolve"));
        }
        void stop() throws Exception {
            asApp(() -> check(am.stopService(thread, new Intent("start").setComponent(SERVICE), null, 0) == 1,
                    "Started service did not stop"));
        }
        void acknowledge(int index, boolean rebind) throws Exception {
            Bind call = thread.unbound.get(index);
            asApp(() -> {
                if (rebind) am.unbindFinished(call.service, call.binding);
                else am.serviceDoneExecuting(call.service, 4, 0, 0);
            });
        }
        @SuppressWarnings("unchecked")
        void restartProcess() throws Exception {
            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            Method died = services.getClass().getDeclaredMethod("processDied", processType);
            died.setAccessible(true); died.invoke(services, process);
            // Keep the fixture deterministic: admission below drives the real bringUp path.
            for (Object service : ((Map<?, ?>) get(services, "mServices")).values()) {
                java.util.concurrent.ScheduledFuture<?> restart =
                        (java.util.concurrent.ScheduledFuture<?>) get(service, "restart");
                if (restart != null) { restart.cancel(false); set(service, "restart", null); }
            }
            ((List<Object>) get(atm, "mProcesses")).remove(process);
            pid++; thread = new AppThread();
            asApp(() -> process = construct(processType, new Class<?>[]{type("ActivityTaskManagerService"),
                    int.class, IApplicationThread.class}, atm, pid, thread));
            set(process, "packageName", OWNER); set(process, "processName", OWNER); set(process, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(process);
        }
        int connections() throws Exception { return ((Map<?, ?>) get(services, "mConnections")).size(); }
        @SuppressWarnings("unchecked")
        Object clientProcess(int clientPid, AppThread client) throws Exception {
            AtomicReference<Object> holder = new AtomicReference<>();
            asCaller(clientPid, () -> holder.set(construct(type("ActivityTaskManagerService$ProcessRecord"),
                    new Class<?>[]{type("ActivityTaskManagerService"), int.class, IApplicationThread.class},
                    atm, clientPid, client)));
            Object record = holder.get();
            set(record, "packageName", OWNER); set(record, "processName", OWNER + ":client" + clientPid);
            set(record, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(record);
            return record;
        }
        void bindFrom(int clientPid, AppThread client, Connection connection, String action) throws Exception {
            asCaller(clientPid, () -> check(am.bindService(client, null,
                    new Intent(action).setComponent(SERVICE), null, connection.callback,
                    (long) Context.BIND_AUTO_CREATE, OWNER, 0) == 1, "External client did not bind"));
        }
        @SuppressWarnings("unchecked")
        void clientDied(Object record) throws Exception {
            ((List<Object>) get(atm, "mProcesses")).remove(record);
            Method died = services.getClass().getDeclaredMethod("processDied",
                    type("ActivityTaskManagerService$ProcessRecord"));
            died.setAccessible(true); died.invoke(services, record);
        }
        void forceStopBindings() throws Exception {
            Method stop = services.getClass().getDeclaredMethod("stopPackage", String.class);
            stop.setAccessible(true); stop.invoke(services, OWNER);
        }
    }

    private static void retained() throws Exception {
        Harness h = new Harness(); h.start();
        Connection one = new Connection(Runnable::run);
        h.bind(one, "binder", Context.BIND_AUTO_CREATE);
        IBinder published = new Binder(); h.publish(0, published);
        h.unbind(one); h.acknowledge(0, true);
        Connection two = new Connection(Runnable::run);
        h.bind(two, "binder", Context.BIND_AUTO_CREATE);
        check(h.thread.binds.size() == 2 && h.thread.binds.get(1).rebind,
                "onUnbind(true) did not request onRebind for the surviving service");
        check(two.connected == 1 && two.value == published,
                "onRebind must reuse and deliver the original published Binder");
        Connection three = new Connection(Runnable::run); h.bind(three, "binder", 0);
        check(three.value == published && h.thread.binds.size() == 2,
                "Another client duplicated onRebind or lost the cached Binder");
        h.acknowledge(0, true);
        check(h.thread.binds.size() == 2, "Duplicate unbind acknowledgement repeated onRebind");
        h.unbind(two);
        check(h.thread.unbinds == 1, "A remaining client did not keep the binding active");
        h.unbind(three); h.acknowledge(1, true);
        Connection four = new Connection(Runnable::run); h.bind(four, "binder", 0);
        check(h.thread.binds.size() == 3 && h.thread.binds.get(2).rebind && four.value == published,
                "A second unbind/rebind cycle was not retained");
        check(h.thread.creates == 1 && h.thread.stops == 0, "Rebinding recreated a started service");
        h.unbind(four); h.acknowledge(2, false); h.stop();
        check(h.thread.stops == 1 && h.connections() == 0, "Rebound service did not clean up");
    }

    private static void declined() throws Exception {
        Harness h = new Harness(); h.start();
        Connection one = new Connection(Runnable::run); h.bind(one, "declined", Context.BIND_AUTO_CREATE);
        IBinder published = new Binder(); h.publish(0, published); h.unbind(one); h.acknowledge(0, false);
        Connection two = new Connection(Runnable::run); h.bind(two, "declined", 0);
        check(two.value == published && two.connected == 1 && h.thread.binds.size() == 1,
                "AOSP keeps the original Binder after onUnbind(false); it does not call onBind again");
        h.unbind(two);
        check(h.thread.unbinds == 1, "A declined rebind created an unmatched onUnbind callback");
        h.stop();
    }

    private static void delayed(boolean nullBinding) throws Exception {
        Harness h = new Harness(); h.start();
        Connection one = new Connection(Runnable::run); h.bind(one, "delayed", Context.BIND_AUTO_CREATE);
        IBinder published = nullBinding ? null : new Binder(); h.publish(0, published); h.unbind(one);
        Connection two = new Connection(Runnable::run); h.bind(two, "delayed", 0);
        check(h.thread.binds.size() == 1 && (nullBinding ? two.nulls == 1 : two.value == published),
                "A client arriving before onUnbind completes did not receive the cached publication");
        h.acknowledge(0, true);
        check(h.thread.binds.size() == 2 && h.thread.binds.get(1).rebind,
                "A delayed true acknowledgement did not rebind already waiting clients");
        check(two.connected + two.nulls == 1, "onRebind republished a binding to the same client");
        h.unbind(two);
        Connection brief = new Connection(Runnable::run); h.bind(brief, "delayed", 0); h.unbind(brief);
        check(h.thread.unbinds == 2 && h.thread.binds.size() == 2,
                "An acknowledgement still in flight caused duplicate lifecycle callbacks");
        h.acknowledge(1, true);
        Connection last = new Connection(Runnable::run); h.bind(last, "delayed", 0);
        check(h.thread.binds.size() == 3 && h.thread.binds.get(2).rebind,
                "True acknowledgement without current clients was not remembered");
        if (nullBinding) last.nullOnce(); else check(last.value == published, "Delayed publication changed");
        h.unbind(last); h.acknowledge(2, false); h.stop();
    }

    private static void independentIntents() throws Exception {
        Harness h = new Harness();
        Connection one = new Connection(Runnable::run), keeper = new Connection(Runnable::run);
        h.bind(one, "one", Context.BIND_AUTO_CREATE); IBinder first = new Binder(); h.publish(0, first);
        h.bind(keeper, "keeper", Context.BIND_AUTO_CREATE); IBinder other = new Binder(); h.publish(1, other);
        h.unbind(one); h.acknowledge(0, true);
        Connection next = new Connection(Runnable::run); h.bind(next, "one", 0);
        check(next.value == first && keeper.value == other && h.thread.binds.size() == 3
                        && h.thread.binds.get(2).rebind && h.thread.stops == 0,
                "An independent auto-create binding did not preserve the service and per-intent Binder");
        h.unbind(next); h.acknowledge(1, false); h.unbind(keeper);
        check(h.thread.stops == 1 && h.connections() == 0, "Last independent binding did not destroy service");
    }

    private static void reentrant() throws Exception {
        Harness h = new Harness(); h.start();
        Connection one = new Connection(Runnable::run); h.bind(one, "reentrant", Context.BIND_AUTO_CREATE);
        IBinder value = new Binder(); h.publish(0, value); h.unbind(one); h.acknowledge(0, true);
        Connection departing = new Connection(Runnable::run);
        departing.onConnected = () -> h.unbind(departing);
        h.bind(departing, "reentrant", 0);
        check(h.thread.binds.size() == 1 && h.connections() == 0,
                "A synchronous connected callback that unbinds must not cause a spurious onRebind");
        Connection live = new Connection(Runnable::run); h.bind(live, "reentrant", 0);
        check(live.value == value && h.thread.binds.size() == 2 && h.thread.binds.get(1).rebind,
                "Reentrant departure lost the remembered rebind request");
        h.thread.onUnbind = call -> h.am.unbindFinished(call.service, call.binding);
        h.unbind(live);
        Connection after = new Connection(Runnable::run); h.bind(after, "reentrant", 0);
        check(h.thread.binds.size() == 3 && h.thread.binds.get(2).rebind,
                "Synchronous onUnbind acknowledgement was lost");
        h.unbind(after); h.stop();
    }

    private static void orderedCalls() throws Exception {
        Harness h = new Harness(); h.start();
        Connection client = new Connection(Runnable::run);
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        IBinder value = new Binder();
        h.thread.onBind = call -> {
            entered.countDown();
            check(release.await(5, TimeUnit.SECONDS), "Timed out waiting for concurrent unbind");
            h.publish(0, value);
        };
        Thread binding = new Thread(() -> {
            try { h.bind(client, "ordered", Context.BIND_AUTO_CREATE); }
            catch (Throwable problem) { failure.set(problem); }
        }, "rebind-contract-worker");
        binding.start();
        try {
            check(entered.await(5, TimeUnit.SECONDS), "Initial bind was not dispatched");
            h.unbind(client);
            check(h.thread.unbinds == 0,
                    "Concurrent unbind overtook an in-flight bind application call");
        } finally { release.countDown(); binding.join(5000); }
        check(!binding.isAlive(), "Binding callback did not finish");
        if (failure.get() != null) throw new AssertionError("Binding worker failed", failure.get());
        check(h.thread.unbinds == 1, "Serialized unbind was not delivered after bind returned");
        h.thread.onBind = null; h.acknowledge(0, true);
        Connection next = new Connection(Runnable::run); h.bind(next, "ordered", 0);
        check(next.value == value && h.thread.binds.get(1).rebind,
                "Serialized unbind discarded the in-flight bind publication");
        h.unbind(next); h.acknowledge(1, false); h.stop();
    }

    private static void orderedDestruction() throws Exception {
        Harness h = new Harness();
        Connection client = new Connection(Runnable::run);
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        h.thread.onBind = call -> {
            entered.countDown();
            check(release.await(5, TimeUnit.SECONDS), "Timed out waiting for last-client departure");
        };
        Thread binding = new Thread(() -> {
            try { h.bind(client, "ordered-destroyed", Context.BIND_AUTO_CREATE); }
            catch (Throwable problem) { failure.set(problem); }
        }, "rebind-destroy-worker");
        binding.start();
        try {
            check(entered.await(5, TimeUnit.SECONDS), "Initial bind was not dispatched");
            h.unbind(client);
            check(h.thread.unbinds == 0 && h.thread.stops == 0,
                    "Destruction overtook an in-flight bind and discarded its queued unbind");
        } finally { release.countDown(); binding.join(5000); }
        check(!binding.isAlive(), "Binding callback did not finish before destruction");
        if (failure.get() != null) throw new AssertionError("Binding worker failed", failure.get());
        check(h.thread.unbinds == 1 && h.thread.stops == 1,
                "Final unbind must be submitted before the service is destroyed");
        check(h.connections() == 0, "Destroyed service leaked its final connection");
    }

    private static void replacingOwner() throws Exception {
        Harness h = new Harness();
        Connection departing = new Connection(Runnable::run), replacement = new Connection(Runnable::run);
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        h.thread.onBind = call -> {
            if (call.rebind) return;
            entered.countDown();
            check(release.await(5, TimeUnit.SECONDS), "Replacement owner did not finish admission");
            h.publish(0, new Binder());
        };
        Thread binding = new Thread(() -> {
            try { h.bind(departing, "replacement", Context.BIND_AUTO_CREATE); }
            catch (Throwable problem) { failure.set(problem); }
        }, "rebind-replacement-worker");
        binding.start();
        try {
            check(entered.await(5, TimeUnit.SECONDS), "Initial owner bind did not dispatch");
            h.unbind(departing);
            h.bind(replacement, "replacement", Context.BIND_AUTO_CREATE);
        } finally { release.countDown(); binding.join(5000); }
        check(!binding.isAlive(), "Replacement-owner binding worker did not finish");
        if (failure.get() != null) throw new AssertionError("Replacement owner worker failed", failure.get());
        check(h.thread.stops == 0 && h.thread.exits == 0 && replacement.connected == 1,
                "A replacement auto-create owner was destroyed while dispatch drained");
        h.thread.onBind = null; h.acknowledge(0, true);
        check(h.thread.binds.size() == 2 && h.thread.binds.get(1).rebind,
                "Replacement owner did not receive delayed onRebind");
        h.unbind(replacement);
        check(h.thread.stops == 1, "Replacement owner's final departure did not destroy service");
    }

    private static void failedLocalDispatch() throws Exception {
        Harness h = new Harness(); h.start();
        Connection client = new Connection(Runnable::run);
        IllegalStateException expected = new IllegalStateException("local binding submission failed");
        h.thread.bindFailure = expected;
        h.thread.onBind = call -> h.unbind(client);
        try {
            h.bind(client, "failed-local", Context.BIND_AUTO_CREATE);
            throw new AssertionError("Local dispatcher failure was lost");
        } catch (IllegalStateException actual) {
            check(actual.getMessage() != null && actual.getMessage().contains(expected.getMessage()),
                    "Local dispatcher changed the original failure");
        }
        check(h.thread.unbinds == 1 && h.thread.stops == 0,
                "Local dispatcher failure stranded a reentrantly queued unbind");
        check(h.connections() == 0, "Failed local dispatcher leaked a client");
        h.thread.bindFailure = null; h.thread.onBind = null;
        h.acknowledge(0, false); h.stop();
    }

    private static void interleavedAcknowledgements() throws Exception {
        for (boolean firstTrue : new boolean[]{false, true}) {
            for (boolean secondTrue : new boolean[]{false, true}) {
                Harness h = new Harness(); h.start();
                Connection first = new Connection(Runnable::run), second = new Connection(Runnable::run);
                h.bind(first, "first", Context.BIND_AUTO_CREATE); IBinder a = new Binder(); h.publish(0, a);
                h.bind(second, "second", Context.BIND_AUTO_CREATE); IBinder b = new Binder(); h.publish(1, b);
                h.unbind(first); h.unbind(second);
                Connection newFirst = new Connection(Runnable::run), newSecond = new Connection(Runnable::run);
                h.bind(newFirst, "first", 0); h.bind(newSecond, "second", 0);
                check(newFirst.value == a && newSecond.value == b, "Interleaved acknowledgements lost cached values");
                h.acknowledge(0, firstTrue); h.acknowledge(1, secondTrue);
                check(h.thread.binds.size() == 2 + (firstTrue ? 1 : 0) + (secondTrue ? 1 : 0),
                        "False acknowledgement retired another binding's pending true acknowledgement");
                for (int i = 2; i < h.thread.binds.size(); ++i) {
                    Bind call = h.thread.binds.get(i);
                    check(call.rebind && ((firstTrue && call.binding == h.thread.binds.get(0).binding)
                            || (secondTrue && call.binding == h.thread.binds.get(1).binding)),
                            "Interleaved acknowledgement rebound the wrong intent");
                }
                h.unbind(newFirst); h.unbind(newSecond); h.stop();
            }
        }
    }

    private static void externalClientDeath() throws Exception {
        Harness h = new Harness(); h.start();
        AppThread client = new AppThread(), unrelated = new AppThread();
        Object owner = h.clientProcess(PID + 100, client);
        Object other = h.clientProcess(PID + 101, unrelated);
        Connection connection = new Connection(Runnable::run);
        h.bindFrom(PID + 100, client, connection, "external"); h.publish(0, new Binder());
        h.clientDied(other);
        check(h.thread.unbinds == 0 && h.connections() == 1,
                "Unrelated client death changed a live service binding");
        h.clientDied(owner);
        check(h.thread.unbinds == 1 && h.connections() == 0 && h.thread.stops == 0,
                "Dead external client's last binding did not unbind a surviving started service");
        h.acknowledge(0, true);
        Connection replacement = new Connection(Runnable::run); h.bind(replacement, "external", 0);
        check(h.thread.binds.size() == 2 && h.thread.binds.get(1).rebind && replacement.connected == 1,
                "Client-death acknowledgement did not preserve cached rebinding");
        h.unbind(replacement); h.acknowledge(1, false); h.stop();
    }

    private static void forceStopQueued() throws Exception {
        Harness h = new Harness(); h.start();
        Connection first = new Connection(Runnable::run), queued = new Connection(Runnable::run);
        h.thread.onBind = call -> {
            h.bind(queued, "queued-after-first", Context.BIND_AUTO_CREATE);
            h.forceStopBindings();
            check(h.thread.stops == 0, "Reentrant force-stop overtook its current binding submission");
        };
        h.bind(first, "before-force-stop", Context.BIND_AUTO_CREATE);
        check(h.thread.binds.size() == 1 && h.thread.stops == 1,
                "Force-stop did not discard queued callbacks before orderly destruction");
    }

    private static void retiredQueued() throws Exception {
        Harness h = new Harness();
        Connection first = new Connection(Runnable::run), queued = new Connection(Runnable::run);
        h.thread.onBind = call -> {
            h.bind(queued, "queued-before-retirement", Context.BIND_AUTO_CREATE);
            set(h.process, "exiting", true); set(h.process, "reaped", true);
            Method retire = h.services.getClass().getDeclaredMethod("retireCachedServices",
                    type("ActivityTaskManagerService$ProcessRecord"));
            retire.setAccessible(true); retire.invoke(h.services, h.process);
        };
        h.bind(first, "before-retirement", Context.BIND_AUTO_CREATE);
        check(h.thread.binds.size() == 1 && h.connections() == 0,
                "Cached-process retirement did not discard queued binding authority");
        check(((Map<?, ?>) get(h.services, "mServices")).isEmpty(),
                "Cached-process retirement retained a stale service record");
    }

    private static void destroyed() throws Exception {
        Harness h = new Harness();
        Connection first = new Connection(Runnable::run); h.bind(first, "destroy", Context.BIND_AUTO_CREATE);
        IBinder old = new Binder(); h.publish(0, old); h.unbind(first);
        check(h.thread.stops == 1, "Unstarted service survived its last auto-create client");
        h.acknowledge(0, true);
        h.restartProcess();
        Connection second = new Connection(Runnable::run); h.bind(second, "destroy", Context.BIND_AUTO_CREATE);
        check(h.thread.binds.size() == 1 && !h.thread.binds.get(0).rebind && second.connected == 0,
                "Destroyed service reused an old Binder or rebind acknowledgement");
        IBinder fresh = new Binder(); h.publish(0, fresh);
        check(second.value == fresh && h.thread.creates == 1, "Destroyed service did not get a fresh onBind");
        h.unbind(second);
    }

    private static void repeatedPublication() throws Exception {
        for (boolean publishNull : List.of(false, true)) {
            Harness h = new Harness();
            h.start();
            Connection first = new Connection(Runnable::run);
            h.bind(first, "cached", Context.BIND_AUTO_CREATE);
            IBinder original = publishNull ? null : new Binder();
            IBinder duplicate = publishNull ? new Binder() : null;
            h.publish(0, original);
            h.publish(0, duplicate);
            if (publishNull) first.nullOnce();
            else check(first.connected == 1 && first.value == original && first.nulls == 0
                            && first.disconnected == 0,
                    "Repeated publication replaced an active cached Binder");
            h.unbind(first);
            h.acknowledge(0, true);
            h.publish(0, duplicate);

            Connection rebound = new Connection(Runnable::run);
            h.bind(rebound, "cached", Context.BIND_AUTO_CREATE);
            check(h.thread.binds.size() == 2 && h.thread.binds.get(1).rebind,
                    "Cached publication did not preserve onRebind");
            if (publishNull) rebound.nullOnce();
            else check(rebound.connected == 1 && rebound.value == original && rebound.nulls == 0,
                    "Later bind did not receive the first publication");
            h.unbind(rebound);
            h.acknowledge(1, false);
            h.stop();
        }
    }

    private static void restarted() throws Exception {
        Harness h = new Harness(); h.start();
        Connection oldClient = new Connection(Runnable::run); h.bind(oldClient, "restart", Context.BIND_AUTO_CREATE);
        IBinder old = new Binder(); h.publish(0, old); h.unbind(oldClient); h.acknowledge(0, true);
        h.restartProcess();
        Connection freshClient = new Connection(Runnable::run); h.bind(freshClient, "restart", Context.BIND_AUTO_CREATE);
        check(h.thread.binds.size() == 1 && !h.thread.binds.get(0).rebind && freshClient.connected == 0,
                "Restarted service reused the dead process's Binder or doRebind state");
        IBinder fresh = new Binder(); h.publish(0, fresh);
        check(freshClient.value == fresh, "Restart did not publish the new process's Binder");
        h.unbind(freshClient); h.acknowledge(0, false); h.stop();
    }

    private static void retainedAcrossServiceDeath() throws Exception {
        for (boolean published : List.of(true, false)) {
            Harness h = new Harness();
            int clientPid = PID + 100;
            AppThread client = new AppThread();
            h.clientProcess(clientPid, client);
            Connection existing = new Connection(Runnable::run);
            h.bindFrom(clientPid, client, existing, "provider-death");
            if (published) h.publish(0, new Binder());

            h.restartProcess();
            check(existing.nulls == 0 && existing.died == 0,
                    "Recovering service reported a null publication or terminal binding death");
            check(h.connections() == 1, "Service-process death lost the surviving client");
            Connection trigger = new Connection(Runnable::run);
            h.bind(trigger, "provider-death", Context.BIND_AUTO_CREATE);
            check(h.thread.binds.size() == 1 && !h.thread.binds.get(0).rebind,
                    "Replacement service did not receive a fresh onBind");
            IBinder replacement = new Binder();
            h.publish(0, replacement);
            check(existing.value == replacement && existing.connected == (published ? 2 : 1)
                            && existing.nulls == 0 && existing.died == 0,
                    "Replacement publication did not reach the existing client");
            h.asCaller(clientPid, () -> check(h.am.unbindService(existing.callback),
                    "Surviving external connection could not unbind"));
            h.unbind(trigger);
        }
    }

    public static int contracts(boolean realBinder, String scenario) throws Exception {
        binder = realBinder; checks = 0;
        if (scenario.equals("all") || scenario.equals("retained")) retained();
        if (scenario.equals("all") || scenario.equals("retained") || scenario.equals("publication")) {
            repeatedPublication();
        }
        if (scenario.equals("all") || scenario.equals("declined")) declined();
        if (scenario.equals("all") || scenario.equals("delayed")) delayed(false);
        if (scenario.equals("all") || scenario.equals("null")) delayed(true);
        if (scenario.equals("all") || scenario.equals("intents")) independentIntents();
        if (scenario.equals("all") || scenario.equals("reentrant")) reentrant();
        if (scenario.equals("all") || scenario.equals("ordered")) orderedCalls();
        if (scenario.equals("all") || scenario.equals("ordered-destroyed")) orderedDestruction();
        if (scenario.equals("all") || scenario.equals("replacement")) replacingOwner();
        if (scenario.equals("all") || scenario.equals("failed-local")) failedLocalDispatch();
        if (scenario.equals("all") || scenario.equals("interleaved")) interleavedAcknowledgements();
        if (scenario.equals("all") || scenario.equals("client-death")) externalClientDeath();
        if (scenario.equals("all") || scenario.equals("force-stop")) forceStopQueued();
        if (scenario.equals("all") || scenario.equals("retired")) retiredQueued();
        if (scenario.equals("all") || scenario.equals("destroyed")) destroyed();
        if (scenario.equals("all") || scenario.equals("restarted")) restarted();
        if (scenario.equals("all") || scenario.equals("provider-death")) retainedAcrossServiceDeath();
        check(checks > 0, "No service rebind scenario selected");
        System.out.println("Service rebinding contracts passed: " + checks
                + " checks; cached/null Binders, true/false/delayed acknowledgements, reentry, ordering, cleanup and restart");
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime"); contracts(true, args.length == 0 ? "all" : args[0]);
    }
}
