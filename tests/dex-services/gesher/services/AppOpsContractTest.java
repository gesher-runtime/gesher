/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.app.SyncNotedAppOp;
import android.content.AttributionSourceState;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.permission.IPermissionChecker;
import com.android.internal.app.IAppOpsCallback;
import com.android.internal.app.IAppOpsService;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedAttributionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.AppOpsService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;
import sun.misc.Unsafe;
import static android.app.AppOpsManager.*;
import static android.content.PermissionChecker.*;

/** AOSP Android 16 contracts, using real installed metadata, caller policy and delivery code.
 * Only APK construction and explicit Binder/native boundary fixtures are synthetic. */
public final class AppOpsContractTest {
    private static final int UID = 12772, PID = 887720;
    private static final String NAME = "fixture.appops.owner", PEER = "fixture.appops.peer";
    private static int checks;
    private static boolean binder;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    private interface Checked { void run() throws Exception; }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message); checks++;
    }
    private static void rejects(Class<? extends Throwable> expected, Checked action) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            if (failure instanceof InvocationTargetException wrapped) failure = wrapped.getCause();
            if (expected.isInstance(failure)) { checks++; return; }
            throw new AssertionError("Expected " + expected + ", got " + failure, failure);
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> type, String name) throws Exception { Field f = type.getDeclaredField(name); f.setAccessible(true); return f; }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Object make(Class<?> type, Class<?>[] signature, Object... args) throws Exception {
        Constructor<?> ctor = type.getDeclaredConstructor(signature); ctor.setAccessible(true); return ctor.newInstance(args);
    }
    private static Object call(Object target, String name, Class<?>[] signature, Object... args) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, signature); method.setAccessible(true); return method.invoke(target, args);
    }
    private static ApkPackage pkg(String name, int uid, byte certificate) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        parsed.addUsesPermission(new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(Manifest.permission.CAMERA, 0, java.util.Set.of()));
        parsed.addUsesPermission(new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(Manifest.permission.VIBRATE, 0, java.util.Set.of()));
        parsed.addAttribution(new ParsedAttributionImpl("one", 1, List.of()));
        parsed.addAttribution(new ParsedAttributionImpl("two", 2, List.of()));
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", name); set(code, "parsed", parsed); set(code, "mUid", uid);
        set(code, "apkPath", "/fixture/" + name + "/base.apk"); set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(code, "mSigningIdentityChecked", true);
        set(code, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{certificate})});
        return code;
    }
    private static final class Token extends Binder {
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        @Override public synchronized void linkToDeath(IBinder.DeathRecipient recipient, int flags) { recipients.add(recipient); }
        @Override public synchronized boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) { return recipients.remove(recipient); }
        void die() {
            List<IBinder.DeathRecipient> saved;
            synchronized (this) { saved = new ArrayList<>(recipients); recipients.clear(); }
            for (IBinder.DeathRecipient recipient : saved) recipient.binderDied();
        }
    }
    private static IBinder parcelTransport(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String name) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        };
    }
    private static final class Harness {
        final ApkPackage owner = pkg(NAME, UID, (byte)7), peer = pkg(PEER, UID + 1, (byte)8);
        final PackageManagerService pm;
        final Object atm, am, process;
        final AppOpsService service;
        final IAppOpsService ops;
        final IPermissionChecker checker;
        @SuppressWarnings("unchecked") Harness() throws Exception {
            pm = (PackageManagerService)make(PackageManagerService.class, new Class<?>[]{Collection.class, boolean.class}, List.of(owner, peer), false);
            // This isolated PM has no remote broadcast transport; keep its production
            // publication executor and lifecycle listeners, replacing only that boundary.
            set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>)event -> { });
            atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
            am = make(type("ActivityManagerService"), new Class<?>[]{atm.getClass(), PackageManagerService.class, type("NotificationManagerService")}, atm, pm, null);
            if (!binder) {
                // Host-only AssetManager boundary: the cached API archive has no framework-res.
                // These Android manifest definitions do not change permission decisions or ops.
                Object catalog = get(pm.permissions(), "mCatalog");
                java.util.Map<String, android.content.pm.PermissionInfo> definitions =
                        (java.util.Map<String, android.content.pm.PermissionInfo>)get(catalog, "mFramework");
                for (String name : List.of(Manifest.permission.CAMERA, Manifest.permission.VIBRATE)) {
                    android.content.pm.PermissionInfo info = new android.content.pm.PermissionInfo();
                    info.name = name; info.packageName = "android";
                    info.protectionLevel = name.equals(Manifest.permission.CAMERA)
                            ? android.content.pm.PermissionInfo.PROTECTION_DANGEROUS : android.content.pm.PermissionInfo.PROTECTION_NORMAL;
                    definitions.put(name, info);
                }
                set(catalog, "mLoaded", true);
            }
            service = pm.appOps();
            ops = binder ? IAppOpsService.Stub.asInterface(parcelTransport(StandIn.of(IAppOpsService.class, service).asBinder())) : service;
            IPermissionChecker real = (IPermissionChecker)make(type("AppOpsPermissionChecker"), new Class<?>[]{PackageManagerService.class}, pm);
            checker = binder ? IPermissionChecker.Stub.asInterface(parcelTransport(StandIn.of(IPermissionChecker.class, real).asBinder())) : real;
            IApplicationThread thread = new IApplicationThread.Default() { final Binder token = new Binder(); @Override public IBinder asBinder() { return token; } };
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)UID << 32) | PID);
                process = make(type("ActivityTaskManagerService$ProcessRecord"), new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, PID, thread);
            } finally { Binder.restoreCallingIdentity(identity); }
            set(process, "packageName", NAME); set(process, "processName", NAME); set(process, "packageCode", owner); set(process, "ready", true);
            ((List<Object>)get(atm, "mProcesses")).add(process);
            check(pm.appOps() == service, "The server must share one AppOps policy ledger");
        }
        void asApp(Checked action) throws Exception {
            long identity = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long)UID << 32) | PID); action.run(); }
            finally { Binder.restoreCallingIdentity(identity); }
        }
        @SuppressWarnings("unchecked") void visible(boolean visible) throws Exception {
            List<Object> activities = (List<Object>)get(process, "activities");
            activities.clear();
            if (visible) {
                ActivityInfo info = new ActivityInfo(); info.packageName = NAME; info.name = NAME + ".Activity";
                Object activity = make(type("ActivityTaskManagerService$ActivityRecord"),
                        new Class<?>[]{atm.getClass(), ActivityInfo.class, Intent.class, int.class, int.class, String.class,
                            type("ActivityTaskManagerService$ActivityRecord"), String.class, int.class},
                        atm, info, new Intent(), 1, PID, NAME, null, null, 0);
                set(activity, "app", process);
                Class state = type("ActivityTaskManagerService$State");
                set(activity, "state", Enum.valueOf(state, "RESUMED")); activities.add(activity);
            }
        }
        AttributionSourceState source(ApkPackage pkg, Token token) {
            AttributionSourceState source = new AttributionSourceState(); source.uid = pkg.uid(); source.pid = -1;
            source.packageName = pkg.packageName; source.token = token; source.attributionTag = "one";
            source.next = new AttributionSourceState[0]; return source;
        }
    }
    private static SyncNotedAppOp start(IAppOpsService ops, Token token, String tag, boolean startDefault) throws Exception {
        return ops.startOperation(token, OP_VIBRATE, UID, NAME, tag, startDefault, false, null, false, 0, ATTRIBUTION_CHAIN_ID_NONE);
    }
    private static void modes(Harness h) throws Exception {
        check(h.ops.checkOperationRaw(OP_VIBRATE, UID, NAME, null) == AppOpsManager.opToDefaultMode(OP_VIBRATE), "Initial raw mode is the AOSP op default");
        check(h.ops.checkOperationRaw(OP_SCHEDULE_EXACT_ALARM, UID, NAME, null) == MODE_DEFAULT, "Exact alarm preserves its distinct default mode");
        rejects(IllegalArgumentException.class, () -> h.ops.checkOperation(OP_NONE, UID, NAME));
        rejects(IllegalArgumentException.class, () -> h.ops.setMode(OP_VIBRATE, UID, NAME, 5));
        check(h.ops.checkPackage(UID + 1, NAME) == MODE_ERRORED, "Package checks use the full installed UID");
        h.asApp(() -> rejects(SecurityException.class, () -> h.ops.setMode(OP_VIBRATE, UID, NAME, MODE_IGNORED)));
        h.asApp(() -> rejects(SecurityException.class, () -> h.ops.getPackagesForOps(null)));
        h.ops.setMode(OP_VIBRATE, UID, NAME, MODE_DEFAULT);
        check(h.ops.checkOperation(OP_VIBRATE, UID, NAME) == MODE_DEFAULT, "Explicit MODE_DEFAULT must remain explicit");
        Token token = new Token();
        check(start(h.ops, token, "one", false).getOpMode() == MODE_DEFAULT, "Default starts require startIfModeDefault");
        check(!h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "Rejected default start has no active entry");
        check(start(h.ops, token, "one", true).getOpMode() == MODE_ALLOWED, "Accepted default start reports started allowed");
        h.ops.finishOperation(token, OP_VIBRATE, UID, NAME, "one");
        h.ops.setMode(OP_VIBRATE, UID, NAME, MODE_ALLOWED);
        h.ops.setUidMode(OP_VIBRATE, UID, MODE_IGNORED);
        check(h.ops.checkOperation(OP_VIBRATE, UID, NAME) == MODE_IGNORED, "Nondefault UID mode precedes package mode");
        h.ops.setUidMode(OP_VIBRATE, UID, MODE_ALLOWED);
        h.ops.setMode(OP_READ_ICC_SMS, UID, NAME, MODE_ERRORED);
        check(h.ops.checkOperationRaw(OP_READ_SMS, UID, NAME, null) == MODE_ERRORED, "Alias operations share the AOSP switch mode");
        h.ops.setMode(OP_READ_SMS, UID, NAME, MODE_ALLOWED);
        check(h.ops.checkOperationForDevice(OP_VIBRATE, UID, NAME, null, 42) == MODE_IGNORED, "Unknown virtual devices are unavailable");
        check(h.service.setUidModeIfCurrent(OP_SCHEDULE_EXACT_ALARM, h.owner, MODE_ALLOWED), "Current package generation may commit a special access decision");
        ApkPackage otherGeneration = pkg(NAME, UID, (byte)7);
        check(!h.service.setUidModeIfCurrent(OP_SCHEDULE_EXACT_ALARM, otherGeneration, MODE_ERRORED), "Obsolete package generation cannot commit a decision");
        check(h.service.checkRawMode(OP_SCHEDULE_EXACT_ALARM, UID, NAME) == MODE_ALLOWED, "Rejected generation leaves the prior decision intact");
        h.ops.setUidMode(OP_SCHEDULE_EXACT_ALARM, UID, MODE_DEFAULT);
    }
    private static void lifetimes(Harness h) throws Exception {
        Token first = new Token(), second = new Token();
        h.asApp(() -> {
            check(start(h.ops, first, "one", false).getOpMode() == MODE_ALLOWED, "Normal initial-allowed starts need no default flag");
            start(h.ops, first, "one", false); start(h.ops, first, "two", false);
        });
        check(first.recipients.size() == 1, "Nested and attributed starts share one client death registration");
        h.ops.finishOperation(second, OP_VIBRATE, UID, NAME, "one");
        h.ops.finishOperation(first, OP_VIBRATE, UID, NAME, "two");
        h.ops.finishOperation(first, OP_VIBRATE, UID, NAME, "one");
        check(h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "Only matching token/tag nesting is decremented");
        h.ops.finishOperation(first, OP_VIBRATE, UID, NAME, "one");
        check(!h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "Final balanced finish ends activity");
        check(first.recipients.isEmpty(), "An empty client releases its death registration");
        start(h.ops, first, "one", false); start(h.ops, second, "one", false); first.die();
        check(h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "Client death preserves another client's operation");
        second.die(); check(!h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "Last client death retires the operation");
        var packages = h.ops.getPackagesForOps(new int[]{OP_VIBRATE});
        check(packages != null && packages.stream().anyMatch(p -> p.getUid() == UID && NAME.equals(p.getPackageName())), "Statistics report actual package UIDs");
        var entries = h.ops.getOpsForPackage(UID, NAME, new int[]{OP_VIBRATE});
        check(entries.get(0).getOps().get(0).getAttributedOpEntries().containsKey("two"), "Operation statistics retain declared attribution tags");
    }
    private static final class Watcher extends IAppOpsCallback.Default {
        final Token token = new Token(); final AtomicInteger calls = new AtomicInteger();
        @Override public IBinder asBinder() { return token; }
        @Override public void opChanged(int op, int uid, String pkg, String device) { calls.incrementAndGet(); }
    }
    private static void awaitCount(AtomicInteger count, int expected) throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (count.get() < expected && System.nanoTime() < end) Thread.sleep(5);
        check(count.get() == expected, "Expected " + expected + " callbacks, got " + count.get());
    }
    private static void watchers(Harness h) throws Exception {
        Watcher watcher = new Watcher();
        // Callback Binder stubs are generated in production. A local callback fixture exercises
        // exact registration identity and death accounting without claiming a remote callback.
        h.service.startWatchingMode(OP_VIBRATE, NAME, watcher); h.service.startWatchingMode(OP_VIBRATE, NAME, watcher);
        check(watcher.token.recipients.size() == 1, "Repeated watch registration has one death recipient");
        h.ops.setMode(OP_VIBRATE, UID, NAME, MODE_IGNORED); awaitCount(watcher.calls, 1);
        h.ops.resetAllModes(0, NAME); awaitCount(watcher.calls, 2);
        h.service.stopWatchingMode(watcher); check(watcher.token.recipients.isEmpty(), "Unwatch releases the death recipient");
    }
    private static void foreground(Harness h) throws Exception {
        h.ops.setMode(OP_CAMERA, UID, NAME, MODE_FOREGROUND);
        check(h.ops.checkOperation(OP_CAMERA, UID, NAME) == MODE_IGNORED, "A cached registered process has no camera foreground capability");
        h.visible(true);
        check(h.ops.checkOperation(OP_CAMERA, UID, NAME) == MODE_ALLOWED, "A resumed activity contributes actual foreground capability");
        Token token = new Token();
        check(h.ops.startOperation(token, OP_CAMERA, UID, NAME, "one", false, false, null, false, 0, -1).getOpMode() == MODE_ALLOWED, "Visible foreground camera operation starts");
        h.visible(false);
        check(h.ops.checkOperation(OP_CAMERA, UID, NAME) == MODE_ALLOWED, "AOSP top-state settling preserves access briefly");
        Thread.sleep(5150);
        check(h.ops.checkOperation(OP_CAMERA, UID, NAME) == MODE_IGNORED, "Foreground capability expires after the AOSP settle interval");
        check(!h.ops.isOperationActive(OP_CAMERA, UID, NAME), "Running operation pauses when foreground mode is no longer allowed");
        h.visible(true);
        check(h.ops.checkOperation(OP_CAMERA, UID, NAME) == MODE_ALLOWED, "Returning to foreground commits immediately");
        check(h.ops.isOperationActive(OP_CAMERA, UID, NAME), "A balanced live start resumes with foreground access");
        h.ops.finishOperation(token, OP_CAMERA, UID, NAME, "one"); h.visible(false);
        h.ops.setMode(OP_CAMERA, UID, NAME, MODE_ALLOWED);
    }
    private static void restrictions(Harness h) throws Exception {
        Token operation = new Token(), restriction = new Token();
        start(h.ops, operation, "one", false);
        h.ops.setUserRestriction(OP_VIBRATE, true, restriction, 0, null);
        check(h.ops.checkOperation(OP_VIBRATE, UID, NAME) == MODE_IGNORED, "A live user restriction overrides allowed modes");
        check(!h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "A user restriction pauses an existing start");
        restriction.die();
        check(h.ops.checkOperation(OP_VIBRATE, UID, NAME) == MODE_ALLOWED, "Restriction token death releases its restriction");
        check(h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "Removing a restriction resumes the balanced start");
        h.ops.finishOperation(operation, OP_VIBRATE, UID, NAME, "one");
        h.ops.setAudioRestriction(OP_PLAY_AUDIO, new int[]{android.media.AudioAttributes.USAGE_NOTIFICATION}, MODE_IGNORED, null);
        check(h.ops.checkAudioOperation(OP_PLAY_AUDIO, android.media.AudioAttributes.USAGE_NOTIFICATION, UID, NAME) == MODE_IGNORED,
                "Audio usage restrictions participate in audio checks");
        h.ops.setAudioRestriction(OP_PLAY_AUDIO, new int[]{android.media.AudioAttributes.USAGE_NOTIFICATION}, MODE_ALLOWED, null);
    }
    private static void delivery(Harness h) throws Exception {
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.CAMERA, "default:0", 0, "fixture");
        Token token = new Token(); AttributionSourceState own = h.source(h.owner, token);
        h.asApp(() -> check(h.checker.checkPermission(Manifest.permission.CAMERA, own, null, true, true, false, OP_NONE) == PERMISSION_HARD_DENIED,
                "PermissionChecker first enforces the runtime permission"));
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.CAMERA, "default:0", 0);
        h.ops.setMode(OP_CAMERA, UID, NAME, MODE_IGNORED);
        h.asApp(() -> check(h.checker.checkPermission(Manifest.permission.CAMERA, own, null, true, true, false, OP_NONE) == PERMISSION_SOFT_DENIED,
                "A granted permission with ignored AppOps returns soft denial"));
        h.ops.setMode(OP_CAMERA, UID, NAME, MODE_ALLOWED);
        h.asApp(() -> check(h.checker.checkPermission(Manifest.permission.CAMERA, own, null, true, true, false, OP_NONE) == PERMISSION_GRANTED,
                "PermissionChecker starts an authorized delivery"));
        check(h.ops.isOperationActive(OP_CAMERA, UID, NAME), "PermissionChecker data delivery reaches AppOps start accounting");
        h.asApp(() -> h.checker.finishDataDelivery(OP_CAMERA, own, false));
        check(!h.ops.isOperationActive(OP_CAMERA, UID, NAME), "finishDataDelivery balances the exact recorded start");
        h.asApp(() -> h.checker.checkPermission(Manifest.permission.CAMERA, own, null, true, true, false, OP_NONE));
        token.die(); check(!h.ops.isOperationActive(OP_CAMERA, UID, NAME), "Attribution token death ends delivery");
        check(h.checker.checkOp(OP_VIBRATE, own, null, false, false) == PERMISSION_GRANTED, "checkOp supports operations without an associated permission");
        Token receiverToken = new Token(); AttributionSourceState receiver = h.source(h.peer, receiverToken);
        receiver.token = h.pm.permissions().registerAttributionSource(receiver);
        AttributionSourceState chain = h.source(h.owner, new Token()); chain.next = new AttributionSourceState[]{receiver};
        chain.token = h.pm.permissions().registerAttributionSource(chain);
        check(h.pm.permissions().isRegisteredAttributionSource(chain), "Registered attribution includes its exact chain");
        h.ops.setMode(OP_VIBRATE, UID + 1, PEER, MODE_IGNORED);
        h.asApp(() -> check(h.ops.startProxyOperationWithState(chain.token, OP_VIBRATE, chain, false, false, null, false, false, 0, 0, -1).getOpMode() == MODE_IGNORED,
                "Proxy start checks the receiving package mode"));
        check(!h.ops.isOperationActive(OP_VIBRATE, UID, NAME), "A failed receiver leaves no proxy operation behind");
        h.ops.setMode(OP_VIBRATE, UID + 1, PEER, MODE_ALLOWED);
        h.asApp(() -> check(h.checker.checkOp(OP_VIBRATE, chain, null, true, true) == PERMISSION_GRANTED, "Trusted chain delivery starts both real participants"));
        check(h.ops.isOperationActive(OP_VIBRATE, UID, NAME) && h.ops.isOperationActive(OP_VIBRATE, UID + 1, PEER), "Both proxy and receiver are accounted for");
        h.asApp(() -> check(h.ops.isProxying(OP_VIBRATE, NAME, "one", UID + 1, PEER),
                "The real proxy caller sees its running attributed operation"));
        check(!h.ops.isProxying(OP_VIBRATE, NAME, "one", UID + 1, PEER),
                "A different caller does not own the live proxy attribution");
        receiverToken.die();
        check(!h.ops.isOperationActive(OP_VIBRATE, UID, NAME) && !h.ops.isOperationActive(OP_VIBRATE, UID + 1, PEER), "Receiver death cleans up the whole attributed delivery");
        h.asApp(() -> check(!h.ops.isProxying(OP_VIBRATE, NAME, "one", UID + 1, PEER),
                "Finished proxy history is not reported as a running proxy"));

        Token replacementRoot = new Token(), replacementReceiver = new Token();
        AttributionSourceState next = h.source(h.peer, replacementReceiver);
        next.token = h.pm.permissions().registerAttributionSource(next);
        AttributionSourceState active = h.source(h.owner, replacementRoot); active.next = new AttributionSourceState[]{next};
        active.token = h.pm.permissions().registerAttributionSource(active);
        h.asApp(() -> check(h.checker.checkOp(OP_VIBRATE, active, null, true, true) == PERMISSION_GRANTED,
                "A fresh delivery owns the installed participant generations"));
        ApkPackage replacement = pkg(PEER, UID + 1, (byte)8);
        java.util.concurrent.CountDownLatch notified = new java.util.concurrent.CountDownLatch(1);
        AutoCloseable observer = h.pm.onPackageChange(value -> { if (value == replacement) notified.countDown(); });
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
        call(h.pm, "packageInstalled", new Class<?>[]{ApkPackage.class, boolean.class, Object.class}, replacement, true, new Object());
        check(notified.await(5, TimeUnit.SECONDS), "The production package publication notifies lifecycle owners");
        observer.close();
        check(!h.ops.isOperationActive(OP_VIBRATE, UID, NAME) && !h.ops.isOperationActive(OP_VIBRATE, UID + 1, PEER),
                "Replacing a receiver retires the whole old data delivery");
        check(replacementRoot.recipients.isEmpty() && replacementReceiver.recipients.isEmpty(),
                "Replacement releases delivery tokens even while the old token objects remain alive");
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, h.peer);
    }
    private static void persistence(Harness h) throws Exception {
        File directory = new File(System.getenv("ANDROID_DATA"), "appops-contract");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new AssertionError("Cannot create isolated state directory");
        File file = File.createTempFile("appops-", ".xml", directory); if (!file.delete()) throw new AssertionError("Cannot prepare state path");
        AppOpsService first = persistent(h, file);
        first.setUidMode(OP_SCHEDULE_EXACT_ALARM, UID, MODE_ALLOWED);
        first.setMode(OP_VIBRATE, UID, NAME, MODE_DEFAULT);
        first.reloadNonHistoricalState();
        check(first.checkRawMode(OP_VIBRATE, UID, NAME) == MODE_DEFAULT,
                "Flushing and reading the persisted mode preserves explicit default");
        AppOpsService second = persistent(h, file);
        check(second.checkRawMode(OP_SCHEDULE_EXACT_ALARM, UID, NAME) == MODE_ALLOWED, "UID grant survives restart for the same installed signer");
        check(second.checkRawMode(OP_VIBRATE, UID, NAME) == MODE_DEFAULT, "Explicit default mode survives restart");
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, pkg(NAME, UID, (byte)9));
        check(second.checkRawMode(OP_SCHEDULE_EXACT_ALARM, UID, NAME) == MODE_DEFAULT, "Signing replacement cannot inherit a UID mode");
        check(second.checkRawMode(OP_VIBRATE, UID, NAME) == MODE_ALLOWED, "Signing replacement cannot inherit package modes");
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, h.owner);
        call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, NAME);
        check(new File(file.getPath()).exists(), "Uninstall cleanup persists the retired ownership");
        check(persistent(h, file).checkRawMode(OP_SCHEDULE_EXACT_ALARM, UID, NAME) == MODE_DEFAULT, "Reinstall cannot recover retired UID grants");
        if (!file.delete()) throw new AssertionError("Cannot remove test state");
    }
    private static AppOpsService persistent(Harness h, File file) throws Exception {
        AppOpsService service = (AppOpsService)make(AppOpsService.class, new Class<?>[]{PackageManagerService.class, File.class}, h.pm, file);
        Supplier<Object> states = () -> { try { return call(h.am, "appOpsUidStates", new Class<?>[0]); } catch (Exception e) { throw new AssertionError(e); } };
        call(service, "attach", new Class<?>[]{type("CallerIdentity"), Supplier.class}, get(h.am, "mCallers"), states);
        return service;
    }
    public static int run(boolean parcel) throws Exception {
        binder = parcel; checks = 0;
        Harness h = new Harness(); modes(h); lifetimes(h); watchers(h); foreground(h); restrictions(h); delivery(h); persistence(h);
        return checks;
    }
    public static void main(String[] args) throws Exception {
        int total = run(true);
        System.out.println("AppOpsContractTest all passed: " + total + " checks; binder=true");
    }
}
