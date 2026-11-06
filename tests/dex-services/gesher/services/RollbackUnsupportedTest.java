/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.ActivityThread;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.res.AssetManager;
import android.content.rollback.IRollbackManager;
import android.content.rollback.RollbackInfo;
import android.content.rollback.RollbackManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.ServiceManager;

import com.gesher.server.Services;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

/** Offline real-manager, unavailable-history and ordinary caller-denial controls. */
public final class RollbackUnsupportedTest {
    private static int checks;

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private interface Operation { void run() throws Exception; }

    private static void rejected(Operation operation, Class<? extends Throwable> type) throws Exception {
        try { operation.run(); }
        catch (Throwable failure) {
            if (failure instanceof java.lang.reflect.InvocationTargetException) {
                failure = failure.getCause();
            }
            check(type.isInstance(failure), "Wrong rejection: " + failure.getClass().getName());
            return;
        }
        throw new AssertionError("Expected " + type.getSimpleName());
    }

    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(parameters);
        constructor.setAccessible(true);
        return constructor.newInstance(values);
    }

    public static void main(String[] args) throws Exception {
        check(args.length == 1, "Pass the committed unsigned synthetic APK fixture");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Map<String, IBinder> created = Services.create(Collections.emptyList());
        IBinder binder = created.get(Context.ROLLBACK_SERVICE);
        check(binder != null, "rollback Binder service is registered");
        check(binder.getClass().getName().equals("com.gesher.server.stub.IRollbackManagerStub"),
                "rollback uses the generated cross-process Binder stub");
        Field cacheField = ServiceManager.class.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) cacheField.get(null);
        cache.putAll(created);
        Context context = ((ActivityThread) construct(ActivityThread.class, new Class<?>[0]))
                .getSystemContext();
        Object named = context.getSystemService(Context.ROLLBACK_SERVICE);
        check(named instanceof RollbackManager, "real ContextImpl constructs RollbackManager");
        RollbackManager manager = (RollbackManager) named;
        check(manager == context.getSystemService(RollbackManager.class), "typed and named lookup agree");
        check(manager == context.getSystemService(Context.ROLLBACK_SERVICE), "manager is cached");
        check(manager.getClass() == RollbackManager.class, "unmodified framework manager is used");
        check(manager.getClass().getClassLoader() == Object.class.getClassLoader(), "manager is on boot classpath");
        List<RollbackInfo> available = manager.getAvailableRollbacks();
        check(available != null && available.isEmpty(), "no available rollback snapshots exist");
        check(manager.getRecentlyCommittedRollbacks().isEmpty(), "no committed rollback history exists");
        try { available.add(null); } catch (UnsupportedOperationException expected) { }
        check(manager.getAvailableRollbacks().isEmpty(), "caller list mutation cannot create rollback history");

        AtomicInteger callbacks = new AtomicInteger();
        AtomicReference<Intent> result = new AtomicReference<>();
        int callerUid = Binder.getCallingUid(), callerPid = Binder.getCallingPid();
        IntentSender receiver = new IntentSender((IIntentSender) new IIntentSender.Stub() {
            @Override public void send(int code, Intent intent, String type, IBinder token,
                    IIntentReceiver finished, String permission, Bundle options) {
                check(Binder.getCallingUid() == callerUid && Binder.getCallingPid() == callerPid,
                        "result dispatch preserves the calling Binder identity");
                check(code == 0 && type == null && token == null && finished == null
                        && permission == null && options == null, "result adds no dispatch authority");
                callbacks.incrementAndGet();
                result.set(new Intent(intent));
            }
        });
        manager.commitRollback(731, Collections.emptyList(), receiver);
        check(callbacks.get() == 1, "unknown ID completes exactly one failure callback");
        check(result.get().getIntExtra(RollbackManager.EXTRA_STATUS, -1)
                        == RollbackManager.STATUS_FAILURE_ROLLBACK_UNAVAILABLE,
                "unknown ID returns the platform rollback-unavailable status");
        check("Rollback unavailable".equals(result.get().getStringExtra(RollbackManager.EXTRA_STATUS_MESSAGE))
                        && result.get().getExtras().size() == 2,
                "result contains only fixed failure extras");
        check(manager.getAvailableRollbacks().isEmpty() && manager.getRecentlyCommittedRollbacks().isEmpty(),
                "failed commit creates no rollback history");

        IRollbackManager service = IRollbackManager.Stub.asInterface(binder);
        rejected(() -> service.commitRollback(731, new ParceledListSlice<>(Collections.emptyList()),
                "com.gesher.missing.rollback.caller", receiver), SecurityException.class);
        check(callbacks.get() == 1, "unknown claimed caller cannot dispatch a callback");
        rejected(() -> service.commitRollback(731, null, "android", receiver), NullPointerException.class);
        rejected(() -> service.commitRollback(731, new ParceledListSlice<>(Collections.emptyList()),
                "android", null), NullPointerException.class);
        IntentSender gone = new IntentSender((IIntentSender) new IIntentSender.Stub() {
            @Override public void send(int code, Intent intent, String type, IBinder token,
                    IIntentReceiver finished, String permission, Bundle options) throws RemoteException {
                throw new RemoteException("synthetic receiver disappeared");
            }
        });
        service.commitRollback(731, new ParceledListSlice<>(Collections.emptyList()), "android", gone);
        checks++;

        rejected(manager::reloadPersistedData, UnsupportedOperationException.class);
        rejected(() -> manager.expireRollbackForPackage("com.gesher.synthetic"), UnsupportedOperationException.class);
        rejected(() -> manager.blockRollbackManager(0), UnsupportedOperationException.class);
        rejected(() -> service.snapshotAndRestoreUserData("com.gesher.synthetic", new int[] {0},
                10000, 0, "synthetic", 1), UnsupportedOperationException.class);
        rejected(() -> service.notifyStagedSession(731), UnsupportedOperationException.class);

        // Only this server-local constructor can supply the identity seam. Actual
        // Binder registrations always use CallerIdentity and current PM metadata.
        Class<?> apkType = Class.forName("com.gesher.server.ApkPackage");
        Object apk = apkType.getMethod("parse", String.class).invoke(null, args[0]);
        String pkg = (String) apkType.getField("packageName").get(apk);
        Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        Object pm = construct(pmType, new Class<?>[] {Collection.class}, Collections.singletonList(apk));
        Object policy = pmType.getMethod("permissions").invoke(pm);
        java.lang.reflect.Method permission = policy.getClass().getMethod("checkPermission", String.class, String.class);
        check((int) permission.invoke(policy, pkg, Manifest.permission.MANAGE_ROLLBACKS) == PackageManager.PERMISSION_DENIED
                        && (int) permission.invoke(policy, pkg, Manifest.permission.TEST_MANAGE_ROLLBACKS) == PackageManager.PERMISSION_DENIED,
                "ordinary synthetic app has neither official rollback permission");
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> serviceType = Class.forName("com.gesher.server.RollbackManagerService");
        AtomicReference<String> identity = new AtomicReference<>(pkg);
        IRollbackManager ordinary = (IRollbackManager) construct(serviceType,
                new Class<?>[] {pmType, atmType, Supplier.class}, pm, null, (Supplier<String>) identity::get);
        rejected(ordinary::getAvailableRollbacks, SecurityException.class);
        rejected(ordinary::getRecentlyCommittedRollbacks, SecurityException.class);
        rejected(() -> ordinary.commitRollback(731, new ParceledListSlice<>(Collections.emptyList()),
                pkg, receiver), SecurityException.class);
        rejected(() -> ordinary.commitRollback(731, new ParceledListSlice<>(Collections.emptyList()),
                "android", receiver), SecurityException.class);
        check(callbacks.get() == 1, "missing permission and forged package cannot invoke the result capability");
        rejected(ordinary::reloadPersistedData, SecurityException.class);
        rejected(() -> ordinary.expireRollbackForPackage(pkg), SecurityException.class);
        rejected(() -> ordinary.blockRollbackManager(0), SecurityException.class);
        rejected(() -> ordinary.snapshotAndRestoreUserData(pkg, new int[] {0}, 10000, 0,
                "synthetic", 1), SecurityException.class);
        rejected(() -> ordinary.notifyStagedSession(731), SecurityException.class);
        identity.set("com.gesher.unknown.rollback.caller");
        rejected(ordinary::getAvailableRollbacks, SecurityException.class);
        check(manager.getAvailableRollbacks().isEmpty() && manager.getRecentlyCommittedRollbacks().isEmpty(),
                "all rejected operations leave actual history empty");
        System.out.println("RollbackUnsupportedTest all passed: " + checks + " synthetic checks");
        System.exit(0);
    }
}
