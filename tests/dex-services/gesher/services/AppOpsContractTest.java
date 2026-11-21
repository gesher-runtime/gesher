/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.app.SyncNotedAppOp;
import android.content.AttributionSourceState;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
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
        return pkg(name, uid, certificate, 36);
    }
    private static ApkPackage pkg(String name, int uid, byte certificate, int target) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(target);
        for (String permission : List.of(Manifest.permission.CAMERA, Manifest.permission.VIBRATE,
                Manifest.permission.RECORD_AUDIO, Manifest.permission.READ_CONTACTS,
                Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_BACKGROUND_LOCATION, Manifest.permission.READ_EXTERNAL_STORAGE)) {
            parsed.addUsesPermission(new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(permission, 0, java.util.Set.of()));
        }
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
        int links;
        @Override public synchronized void linkToDeath(IBinder.DeathRecipient recipient, int flags) { links++; recipients.add(recipient); }
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
        Harness() throws Exception { this(null); }
        @SuppressWarnings("unchecked") Harness(Integer ownerTarget, String... permissions) throws Exception {
            // Metadata must be final before PM boot admission reconciles the UID's
            // legacy/runtime grant basis. Mutating an admitted owner is not an install.
            if (ownerTarget != null) requested(owner, ownerTarget, permissions);
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
                for (String name : List.of(Manifest.permission.CAMERA, Manifest.permission.VIBRATE,
                        Manifest.permission.RECORD_AUDIO, Manifest.permission.BACKGROUND_CAMERA,
                        Manifest.permission.RECORD_BACKGROUND_AUDIO, Manifest.permission.READ_CONTACTS, Manifest.permission.ANSWER_PHONE_CALLS,
                        Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION,
                        Manifest.permission.ACCESS_BACKGROUND_LOCATION, Manifest.permission.READ_EXTERNAL_STORAGE)) {
                    String background = name.equals(Manifest.permission.CAMERA) ? Manifest.permission.BACKGROUND_CAMERA
                            : name.equals(Manifest.permission.RECORD_AUDIO) ? Manifest.permission.RECORD_BACKGROUND_AUDIO
                            : name.equals(Manifest.permission.ACCESS_COARSE_LOCATION) || name.equals(Manifest.permission.ACCESS_FINE_LOCATION)
                                    ? Manifest.permission.ACCESS_BACKGROUND_LOCATION : null;
                    android.content.pm.PermissionInfo info = new android.content.pm.PermissionInfo(background);
                    info.name = name; info.packageName = "android";
                    info.protectionLevel = name.equals(Manifest.permission.VIBRATE) ? PermissionInfo.PROTECTION_NORMAL
                            : name.equals(Manifest.permission.BACKGROUND_CAMERA) || name.equals(Manifest.permission.RECORD_BACKGROUND_AUDIO)
                                    ? PermissionInfo.PROTECTION_INTERNAL | PermissionInfo.PROTECTION_FLAG_ROLE
                                    : PermissionInfo.PROTECTION_DANGEROUS;
                    if (name.equals(Manifest.permission.ANSWER_PHONE_CALLS)) info.protectionLevel |= PermissionInfo.PROTECTION_FLAG_RUNTIME_ONLY;
                    if (name.equals(Manifest.permission.ACCESS_BACKGROUND_LOCATION)) info.flags = PermissionInfo.FLAG_HARD_RESTRICTED;
                    if (name.equals(Manifest.permission.READ_EXTERNAL_STORAGE)) {
                        info.flags = PermissionInfo.FLAG_SOFT_RESTRICTED | PermissionInfo.FLAG_IMMUTABLY_RESTRICTED;
                    }
                    definitions.put(name, info);
                }
                set(catalog, "mLoaded", true);
                // Native ART had the real catalog during PM construction. The host
                // boundary must perform that same production boot reconciliation now.
                call(pm.permissions(), "initializePackages", new Class<?>[0]);
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
        h.ops.setMode(OP_WRITE_ICC_SMS, UID, NAME, MODE_ERRORED);
        check(h.ops.checkOperationRaw(OP_WRITE_SMS, UID, NAME, null) == MODE_ERRORED, "Non-runtime aliases share the AOSP switch mode");
        h.ops.setMode(OP_WRITE_SMS, UID, NAME, AppOpsManager.opToDefaultMode(OP_WRITE_SMS));
        check(h.ops.checkOperationForDevice(OP_VIBRATE, UID, NAME, null, 42) == MODE_IGNORED, "Unknown virtual devices are unavailable");
        check(h.service.setUidModeIfCurrent(OP_SCHEDULE_EXACT_ALARM, h.owner, MODE_ALLOWED), "Current package generation may commit a special access decision");
        ApkPackage otherGeneration = pkg(NAME, UID, (byte)7);
        check(!h.service.setUidModeIfCurrent(OP_SCHEDULE_EXACT_ALARM, otherGeneration, MODE_ERRORED), "Obsolete package generation cannot commit a decision");
        check(h.service.checkRawMode(OP_SCHEDULE_EXACT_ALARM, UID, NAME) == MODE_ALLOWED, "Rejected generation leaves the prior decision intact");
        h.ops.setUidMode(OP_SCHEDULE_EXACT_ALARM, UID, MODE_DEFAULT);
        runtimePermissions(h);
    }
    private static void drain(Harness h) throws Exception {
        ((java.util.concurrent.ScheduledThreadPoolExecutor)get(h.service, "mEvents"))
                .submit(() -> { }).get(5, TimeUnit.SECONDS);
    }
    private static void flags(Harness h, String permission, int mask, int values) {
        h.pm.permissions().updatePermissionFlags(NAME, permission, mask, values, true, "default:0", 0);
    }
    private static void runtimePermissions(Harness h) throws Exception {
        String[] permissions = {Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO, Manifest.permission.READ_CONTACTS};
        int[] operations = {OP_CAMERA, OP_RECORD_AUDIO, OP_READ_CONTACTS};
        for (int i = 0; i < permissions.length; i++) {
            String permission = permissions[i]; int op = operations[i];
            h.pm.permissions().revokeRuntimePermission(NAME, permission, "default:0", 0, "runtime contract");
            check(h.ops.checkOperationRaw(op, UID, NAME, null) == MODE_IGNORED, "A revoked runtime permission has ignored raw AppOps");
            h.ops.setMode(op, UID, NAME, MODE_ALLOWED);
            h.ops.setUidMode(op, UID, MODE_ALLOWED);
            check(h.pm.permissions().checkPermission(NAME, permission) == PackageManager.PERMISSION_DENIED
                    && h.ops.checkOperationRaw(op, UID, NAME, null) == MODE_IGNORED,
                    "Neither runtime AppOps setter can grant a denied permission");
            h.pm.permissions().grantRuntimePermission(NAME, permission, "default:0", 0);
            check(h.ops.checkOperationRaw(op, UID, NAME, null) == (op == OP_READ_CONTACTS ? MODE_ALLOWED : MODE_FOREGROUND),
                    "Runtime modes use the real foreground/background permission metadata");
            h.ops.setMode(op, UID, NAME, MODE_ERRORED);
            check(h.ops.checkOperationRaw(op, UID, NAME, null) == (op == OP_READ_CONTACTS ? MODE_ALLOWED : MODE_FOREGROUND),
                    "Android 16 ignores independent package modes for runtime operations");
        }
        h.asApp(() -> rejects(SecurityException.class, () -> flags(h, Manifest.permission.CAMERA,
                PackageManager.FLAG_PERMISSION_REVOKED_COMPAT, PackageManager.FLAG_PERMISSION_REVOKED_COMPAT)));
        rejects(IllegalArgumentException.class, () -> h.pm.permissions().updatePermissionFlags(NAME,
                Manifest.permission.CAMERA, 0, 0, true, "unavailable:1", 0));
        drain(h);
        Watcher watcher = new Watcher(); h.service.startWatchingMode(OP_CAMERA, NAME, watcher);
        h.ops.setUidMode(OP_CAMERA, UID, MODE_ERRORED); awaitCount(watcher.calls, 1);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_CAMERA, UID, NAME, null) == MODE_IGNORED,
                "The UID runtime setter changes compatibility flags, not the binary grant or an arbitrary mode");
        check((h.pm.permissions().getPermissionFlags(NAME, Manifest.permission.CAMERA, "default:0", 0)
                & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) != 0, "Compatibility revocation is observable permission state");
        h.ops.setUidMode(OP_CAMERA, UID, MODE_ALLOWED); awaitCount(watcher.calls, 2);
        check(h.ops.checkOperationRaw(OP_CAMERA, UID, NAME, null) == MODE_FOREGROUND,
                "Allow clears compatibility revocation without inventing background access");
        h.ops.setMode(OP_CAMERA, UID, NAME, MODE_IGNORED); drain(h);
        check(watcher.calls.get() == 2, "An ignored package setter does not notify a policy change");
        flags(h, Manifest.permission.CAMERA, PackageManager.FLAG_PERMISSION_REVOKED_COMPAT,
                PackageManager.FLAG_PERMISSION_REVOKED_COMPAT); awaitCount(watcher.calls, 3);
        flags(h, Manifest.permission.CAMERA, PackageManager.FLAG_PERMISSION_REVOKED_COMPAT, 0); awaitCount(watcher.calls, 4);
        h.service.stopWatchingMode(watcher);
        var uidOps = h.ops.getUidOps(UID, new int[]{OP_CAMERA});
        check(uidOps != null && uidOps.get(0).getOps().get(0).getMode() == MODE_FOREGROUND,
                "UID mode statistics include computed nondefault runtime modes");

        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ACCESS_FINE_LOCATION, "default:0", 0);
        check(h.ops.checkOperationRaw(OP_FINE_LOCATION, UID, NAME, null) == MODE_FOREGROUND
                && h.ops.checkOperationRaw(OP_COARSE_LOCATION, UID, NAME, null) == MODE_FOREGROUND,
                "A fuller location grant supplies coarse AppOps without fabricating a separate grant");
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED,
                "Permission and AppOps queries share the fuller-permission relationship");
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ACCESS_COARSE_LOCATION, "default:0", 0);
        check(h.ops.checkOperationRaw(OP_GPS, UID, NAME, null) == MODE_FOREGROUND,
                "Runtime switch aliases use the permission-derived mode");
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ACCESS_BACKGROUND_LOCATION, "default:0", 0);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ACCESS_BACKGROUND_LOCATION) == PackageManager.PERMISSION_DENIED,
                "A hard-restricted permission is not granted before its restriction is removed");
        flags(h, Manifest.permission.ACCESS_BACKGROUND_LOCATION,
                PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION | PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ACCESS_BACKGROUND_LOCATION, "default:0", 0);
        check(h.ops.checkOperationRaw(OP_FINE_LOCATION, UID, NAME, null) == MODE_ALLOWED,
                "A real background permission grant permits background location");
        h.ops.setUidMode(OP_FINE_LOCATION, UID, MODE_FOREGROUND);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ACCESS_BACKGROUND_LOCATION) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_FINE_LOCATION, UID, NAME, null) == MODE_FOREGROUND,
                "The foreground UID setter preserves grants and revokes only background AppOps compatibility");
        check((h.pm.permissions().getPermissionFlags(NAME, Manifest.permission.ACCESS_BACKGROUND_LOCATION, "default:0", 0)
                & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) != 0, "Background compatibility flags belong to the background permission");
        h.ops.setUidMode(OP_FINE_LOCATION, UID, MODE_ALLOWED);
        check(h.ops.checkOperationRaw(OP_FINE_LOCATION, UID, NAME, null) == MODE_ALLOWED, "Clearing compatibility restores the existing background grant");
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.ACCESS_BACKGROUND_LOCATION, "default:0", 0, "foreground only");
        check(h.ops.checkOperationRaw(OP_FINE_LOCATION, UID, NAME, null) == MODE_FOREGROUND, "Background revocation immediately changes raw location policy");

        flags(h, Manifest.permission.READ_EXTERNAL_STORAGE, PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION, 0);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.READ_EXTERNAL_STORAGE, "default:0", 0);
        flags(h, Manifest.permission.READ_EXTERNAL_STORAGE, PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION,
                PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_READ_EXTERNAL_STORAGE, UID, NAME, null) == MODE_IGNORED,
                "Soft restriction denies AppOps while retaining the runtime permission grant");

        ApkPackage shared = pkg("fixture.appops.shared", UID, (byte)7);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, shared);
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0, "shared UID");
        check(h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, shared.packageName, null) == MODE_IGNORED,
                "Runtime AppOps revocation is shared by the whole admitted UID");
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0);
        check(h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, shared.packageName, null) == MODE_ALLOWED,
                "A shared sibling observes the same granted runtime AppOps state");
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.FLAG_PERMISSION_SYSTEM_FIXED,
                PackageManager.FLAG_PERMISSION_SYSTEM_FIXED);
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0, "fixed grant");
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED,
                "Runtime grant mutation preserves the system-fixed permission flag");
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.FLAG_PERMISSION_SYSTEM_FIXED, 0);
        call(h.pm, "remove", new Class<?>[]{String.class}, shared.packageName);

        ApkPackage legacy = pkg("fixture.appops.legacy", UID + 2, (byte)7, 22);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, legacy);
        h.pm.permissions().revokeRuntimePermission(legacy.packageName, Manifest.permission.CAMERA, "default:0", 0, "legacy contract");
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED,
                "Legacy runtime permission grants cannot be revoked as modern binary grants");
        h.ops.setUidMode(OP_CAMERA, legacy.uid(), MODE_IGNORED);
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_CAMERA, legacy.uid(), legacy.packageName, null) == MODE_IGNORED,
                "Legacy denial uses compatibility flags and preserves hard versus soft denial");
        h.ops.setUidMode(OP_CAMERA, legacy.uid(), MODE_ALLOWED);
        check(h.ops.checkOperationRaw(OP_CAMERA, legacy.uid(), legacy.packageName, null) == MODE_FOREGROUND,
                "Legacy compatibility restoration still requires real background permission");
        h.pm.permissions().forgetPackage(legacy.packageName);
        call(h.pm, "remove", new Class<?>[]{String.class}, legacy.packageName);
        drain(h);
    }
    private static void unavailableDevices(Harness h) throws Exception {
        AtomicInteger noted = new AtomicInteger(), started = new AtomicInteger();
        Token notedToken = new Token(), startedToken = new Token(), client = new Token();
        var notedCallback = new com.android.internal.app.IAppOpsNotedCallback.Default() {
            @Override public IBinder asBinder() { return notedToken; }
            @Override public void opNoted(int op, int uid, String name, String tag, int device, int flags, int mode) { noted.incrementAndGet(); }
        };
        var startedCallback = new com.android.internal.app.IAppOpsStartedCallback.Default() {
            @Override public IBinder asBinder() { return startedToken; }
            @Override public void opStarted(int op, int uid, String name, String tag, int device, int flags,
                    int mode, int type, int attributionFlags, int chain) { started.incrementAndGet(); }
        };
        h.service.startWatchingNoted(new int[]{OP_PLAY_AUDIO}, notedCallback);
        h.service.startWatchingStarted(new int[]{OP_PLAY_AUDIO}, startedCallback);
        int histories = ((java.util.Map<?, ?>)get(h.service, "mHistory")).size();
        int clients = ((java.util.Map<?, ?>)get(h.service, "mClients")).size();
        check(h.ops.getOpsForPackage(UID, NAME, new int[]{OP_PLAY_AUDIO}) == null, "Unavailable-device probe starts without operation accounting");
        for (int device : new int[]{1, 42, -1, Integer.MAX_VALUE}) {
            check(h.ops.noteOperationForDevice(OP_PLAY_AUDIO, UID, NAME, "one", device, true, "device contract", true)
                    .getOpMode() == MODE_IGNORED, "Unavailable device note returns ignored");
            check(h.ops.startOperationForDevice(client, OP_PLAY_AUDIO, UID, NAME, "one", device,
                    false, true, "device contract", true, 0, -1).getOpMode() == MODE_IGNORED,
                    "Unavailable device start returns ignored");
            h.ops.finishOperationForDevice(client, OP_PLAY_AUDIO, UID, NAME, "one", device);
        }
        for (boolean invalidProxy : new boolean[]{false, true}) {
            AttributionSourceState source = h.source(h.owner, client);
            AttributionSourceState receiver = h.source(h.peer, new Token());
            source.next = new AttributionSourceState[]{receiver};
            (invalidProxy ? source : receiver).deviceId = 1;
            check(h.ops.noteProxyOperationWithState(OP_PLAY_AUDIO, source, true, "device proxy", true, false)
                    .getOpMode() == MODE_IGNORED, "An unavailable proxy participant is rejected before noting either side");
            check(h.ops.startProxyOperationWithState(client, OP_PLAY_AUDIO, source, false, true,
                    "device proxy", true, false, 0, 0, -1).getOpMode() == MODE_IGNORED,
                    "An unavailable proxy participant is rejected before starting either side");
        }
        Token deliveryToken = new Token();
        AttributionSourceState source = h.source(h.owner, deliveryToken); source.deviceId = 1;
        h.asApp(() -> check(h.checker.checkOp(OP_PLAY_AUDIO, source, "device delivery", true, true) == PERMISSION_SOFT_DENIED,
                "PermissionChecker rejects unavailable-device delivery before owning resources"));
        h.asApp(() -> rejects(SecurityException.class, () -> h.ops.noteOperationForDevice(OP_PLAY_AUDIO,
                UID + 1, PEER, null, 1, false, null, false)));
        rejects(IllegalArgumentException.class, () -> h.ops.noteOperationForDevice(OP_NONE, UID, NAME,
                null, 1, false, null, false));
        drain(h);
        check(noted.get() == 0 && started.get() == 0, "Unavailable devices do not dispatch note or start observers");
        check(client.links == 0 && deliveryToken.links == 0, "Unavailable devices never acquire AppOps or delivery death recipients");
        check(((java.util.Map<?, ?>)get(h.service, "mHistory")).size() == histories
                && ((java.util.Map<?, ?>)get(h.service, "mClients")).size() == clients,
                "Unavailable devices leave history and live-client resources unchanged");
        check(h.ops.getOpsForPackage(UID, NAME, new int[]{OP_PLAY_AUDIO}) == null
                && h.ops.getOpsForPackage(UID + 1, PEER, new int[]{OP_PLAY_AUDIO}) == null,
                "Rejected device calls do not create a queryable record on either proxy participant");
        check(h.ops.extractAsyncOps(NAME) == null, "Unavailable devices do not enqueue asynchronous notes");
        h.service.stopWatchingNoted(notedCallback); h.service.stopWatchingStarted(startedCallback);
        check(notedToken.recipients.isEmpty() && startedToken.recipients.isEmpty(), "Device-test observers release their registration resources");
    }
    private static void lifetimes(Harness h) throws Exception {
        unavailableDevices(h);
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
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.CAMERA, "default:0", 0);
        check(h.ops.checkOperationRaw(OP_CAMERA, UID, NAME, null) == MODE_FOREGROUND,
                "The manifest background permission produces foreground-only camera access");
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
        flags(h, Manifest.permission.CAMERA, PackageManager.FLAG_PERMISSION_REVOKED_COMPAT,
                PackageManager.FLAG_PERMISSION_REVOKED_COMPAT); drain(h);
        check(!h.ops.isOperationActive(OP_CAMERA, UID, NAME), "Permission flag revocation pauses a live operation without waiting for another check");
        flags(h, Manifest.permission.CAMERA, PackageManager.FLAG_PERMISSION_REVOKED_COMPAT, 0); drain(h);
        check(h.ops.isOperationActive(OP_CAMERA, UID, NAME), "Restoring compatibility flags resumes the balanced live operation");
        h.ops.finishOperation(token, OP_CAMERA, UID, NAME, "one"); h.visible(false);
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
        h.ops.setUidMode(OP_CAMERA, UID, MODE_IGNORED);
        h.asApp(() -> check(h.checker.checkPermission(Manifest.permission.CAMERA, own, null, true, true, false, OP_NONE) == PERMISSION_SOFT_DENIED,
                "A granted permission with ignored AppOps returns soft denial"));
        h.ops.setUidMode(OP_CAMERA, UID, MODE_ALLOWED);
        h.visible(true);
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
    /** Public PermissionChecker results and observer metadata, using owned fixture packages. */
    private static void permissionCheckerEdges() throws Exception {
        Harness h = new Harness();
        String name = "fixture.appops.unmapped";
        String permission = name + ".permission.OPERATION";
        ApkPackage declared = pkg(name, UID + 19, (byte)19);
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, declared.apkPath);
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        var definition = new com.android.internal.pm.pkg.component.ParsedPermissionImpl();
        definition.setName(permission); definition.setPackageName(name);
        definition.setProtectionLevel(PermissionInfo.PROTECTION_NORMAL | PermissionInfo.PROTECTION_FLAG_APPOP);
        parsed.addPermission(definition);
        parsed.addUsesPermission(new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(permission, 0, java.util.Set.of()));
        parsed.hideAsFinal(); set(declared, "parsed", parsed);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, declared);
        check(AppOpsManager.permissionToOpCode(permission) == OP_NONE,
                "The declared app-op permission has no framework operation mapping");
        check(h.pm.permissions().checkPermission(name, permission) == PackageManager.PERMISSION_GRANTED,
                "The fixture has its normal permission independently of the missing app-op mapping");
        AttributionSourceState own = h.source(declared, new Token());
        for (boolean delivery : new boolean[]{false, true}) {
            check(h.checker.checkPermission(permission, own, null, delivery, delivery, false, OP_NONE)
                            == PERMISSION_HARD_DENIED,
                    "An app-op permission without its operation is hard denied before accounting");
        }

        ApkPackage[] participants = {
                pkg("fixture.appops.attribution.origin", UID + 20, (byte)20),
                pkg("fixture.appops.attribution.middle", UID + 21, (byte)21),
                pkg("fixture.appops.attribution.third", UID + 22, (byte)22),
                pkg("fixture.appops.attribution.fourth", UID + 23, (byte)23)};
        for (ApkPackage participant : participants) {
            call(h.pm, "add", new Class<?>[]{ApkPackage.class}, participant);
            h.ops.setUidMode(OP_VIBRATE, participant.uid(), MODE_ALLOWED);
            h.ops.setMode(OP_VIBRATE, participant.uid(), participant.packageName, MODE_ALLOWED);
            h.pm.permissions().grantRuntimePermission(participant.packageName,
                    Manifest.permission.READ_CONTACTS, "default:0", 0);
            check(h.ops.checkOperationRaw(OP_READ_CONTACTS, participant.uid(), participant.packageName, null) == MODE_ALLOWED,
                    "Each attribution participant has its own granted operation before the observer test");
        }
        attachedProcess(h, participants[0], PID + 20, new IApplicationThread.Default() {
            final IBinder token = new Binder();
            @Override public IBinder asBinder() { return token; }
        });
        // Explicit AOSP observer expectations: chain origin, middle nodes, final receiver,
        // privileged origin and datasource exclusion are distinct cases.
        for (boolean trusted : new boolean[]{false, true}) for (boolean datasource : new boolean[]{false, true}) {
            int count = trusted && datasource ? 4 : 3;
            AttributionSourceState[] nodes = new AttributionSourceState[count];
            for (int i = count - 1; i >= 0; i--) {
                nodes[i] = h.source(participants[i], new Token());
                if (trusted && i == 0) {
                    nodes[i].uid = android.os.Process.SYSTEM_UID;
                    nodes[i].packageName = "android"; nodes[i].attributionTag = null;
                }
                if (i + 1 < count) nodes[i].next = new AttributionSourceState[]{nodes[i + 1]};
                nodes[i].token = h.pm.permissions().registerAttributionSource(nodes[i]);
            }
            java.util.Map<String, List<Integer>> observed = new java.util.concurrent.ConcurrentHashMap<>();
            java.util.Set<Integer> chainIds = java.util.concurrent.ConcurrentHashMap.newKeySet();
            Token observerToken = new Token();
            var observer = new com.android.internal.app.IAppOpsStartedCallback.Default() {
                @Override public IBinder asBinder() { return observerToken; }
                @Override public void opStarted(int op, int uid, String pkg, String tag, int device,
                        int flags, int mode, int type, int attributionFlags, int chainId) {
                    if (type == AppOpsManager.OnOpStartedListener.START_TYPE_STARTED) {
                        observed.computeIfAbsent(pkg, ignored -> new java.util.concurrent.CopyOnWriteArrayList<>())
                                .add(attributionFlags);
                        chainIds.add(chainId);
                    }
                }
            };
            int op = datasource ? OP_READ_CONTACTS : OP_VIBRATE;
            h.service.startWatchingStarted(new int[]{op}, observer);
            try {
                Checked start = () -> {
                    int result = datasource
                        ? h.checker.checkPermission(Manifest.permission.READ_CONTACTS, nodes[0], null,
                                true, true, true, OP_NONE)
                        : h.checker.checkOp(OP_VIBRATE, nodes[0], null, true, true);
                    check(result == PERMISSION_GRANTED,
                            "The owned attribution chain starts (trusted=" + trusted + ", datasource="
                                    + datasource + "): result=" + result);
                };
                if (trusted) start.run(); else asCaller(participants[0].uid(), PID + 20, start);
                drain(h); drain(h);
                int trust = trusted ? ATTRIBUTION_FLAG_TRUSTED : ATTRIBUTION_FLAGS_NONE;
                java.util.Map<String, List<Integer>> expected = new java.util.LinkedHashMap<>();
                if (!datasource) expected.put(nodes[0].packageName, List.of(trust | ATTRIBUTION_FLAG_ACCESSOR));
                else if (!trusted) expected.put(nodes[0].packageName, List.of(ATTRIBUTION_FLAGS_NONE));
                for (int i = 1; i < count; i++) {
                    int role = i == count - 1 ? ATTRIBUTION_FLAG_RECEIVER
                            : datasource && i == 1 ? ATTRIBUTION_FLAG_ACCESSOR : ATTRIBUTION_FLAG_INTERMEDIARY;
                    expected.put(nodes[i].packageName, List.of(trust | role));
                }
                check(expected.equals(observed), "Attribution roles match AOSP: expected " + expected + ", got " + observed);
                check(chainIds.size() == 1 && !chainIds.contains(ATTRIBUTION_CHAIN_ID_NONE),
                        "All observer records identify the same actual attribution chain");
            } finally {
                Checked finish = () -> h.checker.finishDataDelivery(op, nodes[0], datasource);
                if (trusted) finish.run(); else asCaller(participants[0].uid(), PID + 20, finish);
                h.service.stopWatchingStarted(observer);
            }
            for (AttributionSourceState node : nodes) {
                check(!h.service.isOperationActive(op, node.uid, node.packageName),
                        "Finishing the attributed delivery balances every participant");
            }
            check(observerToken.recipients.isEmpty(), "Attribution observer teardown releases its death registration");
        }
    }

    private static void asCaller(int uid, int pid, Checked action) throws Exception {
        long identity = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | pid); action.run(); }
        finally { Binder.restoreCallingIdentity(identity); }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static void foregroundServiceExit() throws Exception {
        Harness h = new Harness();
        Object services = get(h.am, "mServices");
        set(services, "mNotifications", new com.gesher.server.NotificationManagerService());
        String name = NAME + ".AccountingService";
        var declared = new com.android.internal.pm.pkg.component.ParsedServiceImpl();
        declared.setPackageName(NAME); declared.setName(name); declared.setEnabled(true);
        declared.setExported(true); declared.setProcessName(NAME);
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(NAME, h.owner.apkPath);
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.setUid(UID);
        parsed.addUsesPermission(new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(
                Manifest.permission.VIBRATE, 0, java.util.Set.of()));
        parsed.addService(declared); parsed.hideAsFinal(); set(h.owner, "parsed", parsed);
        android.content.pm.ServiceInfo info = h.owner.serviceInfo(name, 0);
        check(info != null, "The owned service fixture uses declared manifest metadata");
        Class<?> recordType = type("ActiveServices$ServiceRecord");
        Object service = make(recordType,
                new Class<?>[]{android.content.pm.ServiceInfo.class, ApkPackage.class}, info, h.owner);
        var component = new android.content.ComponentName(NAME, name);
        ((java.util.Map<Object, Object>)get(services, "mServices")).put(component, service);
        set(service, "app", h.process); set(service, "created", true); set(service, "startRequested", true);
        set(service, "foregroundId", 41); set(service, "whileInUseAtStart", true);
        call(services, "setServiceForeground", new Class<?>[]{android.content.ComponentName.class,
                IBinder.class, int.class, android.app.Notification.class, int.class, int.class},
                component, service, 0, null, 0, 0);
        check(!(Boolean)get(service, "whileInUseAtStart"),
                "Explicit foreground exit discards the preceding start's visibility eligibility");
        check((Integer)get(service, "foregroundType") == 0 && (Integer)get(service, "foregroundCapabilities") == 0,
                "Foreground exit clears selected types and their capabilities");

        h.visible(true);
        Object activity = ((List<?>)get(h.process, "activities")).get(0);
        set(activity, "state", Enum.valueOf((Class)type("ActivityTaskManagerService$State"), "STOPPED"));
        set(service, "foregroundId", 42);
        h.ops.setUidMode(OP_VIBRATE, UID, MODE_FOREGROUND);
        java.util.concurrent.CountDownLatch inactive = new java.util.concurrent.CountDownLatch(1);
        AtomicInteger inactiveCount = new AtomicInteger();
        Token watcherToken = new Token(), operation = new Token();
        var watcher = new com.android.internal.app.IAppOpsActiveCallback.Default() {
            @Override public IBinder asBinder() { return watcherToken; }
            @Override public void opActiveChanged(int op, int uid, String pkg, String tag, int device,
                    boolean active, int flags, int chain) {
                if (!active) { inactiveCount.incrementAndGet(); inactive.countDown(); }
            }
        };
        h.service.startWatchingActive(new int[]{OP_VIBRATE}, watcher);
        try {
            check(h.ops.startOperation(operation, OP_VIBRATE, UID, NAME, null,
                            false, false, null, false, 0, -1).getOpMode() == MODE_ALLOWED,
                    "The service contributes foreground state to ordinary operation accounting");
            drain(h); drain(h);
            check((Boolean)call(services, "stopServiceToken",
                            new Class<?>[]{android.content.ComponentName.class, IBinder.class, int.class},
                            component, service, -1), "The owned service stops through the public server path");
            check(inactive.await(8, TimeUnit.SECONDS),
                    "Service removal publishes the settled inactive transition without a later AppOps query");
            drain(h);
            int active = 0;
            for (Object history : ((java.util.Map<?, ?>)get(h.service, "mHistory")).values()) {
                active += (Integer)get(history, "active");
            }
            check(active == 0 && inactiveCount.get() == 1,
                    "The published ledger and observer each reflect the service removal once");
            check(((java.util.Map<?, ?>)get(services, "mServices")).isEmpty()
                            && ((List<?>)get(h.atm, "mProcesses")).contains(h.process),
                    "A stopped Activity retains the process after its foreground service is gone");
        } finally {
            h.ops.finishOperation(operation, OP_VIBRATE, UID, NAME, null);
            h.service.stopWatchingActive(watcher);
        }
        check(watcherToken.recipients.isEmpty() && operation.recipients.isEmpty(),
                "Teardown releases operation and observer death registrations");
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
                "Flushing the published mode preserves explicit default");
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
        persistenceScheduling(h, directory);
        sharedRetirement(false, false, false);
        sharedRetirement(true, false, false);
        sharedRetirement(false, true, false);
        sharedRetirement(false, false, true);
        sharedRetirement(false, true, true);
        retirementGeneration();
        legacyEligibility();
        legacyTransition(false, true, false, false, false);
        legacyTransition(false, true, true, false, false);
        legacyTransition(true, true, false, false, false);
        legacyTransition(false, false, false, false, false);
        legacyTransition(false, false, false, true, false);
        legacyTransition(false, true, false, false, true);
        legacyTransition(false, true, true, false, true);
        runtimeOnlyPublication();
        publicationRegrant();
        transitionRestartAfterWriteFailure();
    }
    /** Only the file I/O boundary is delayed or failed; production publication and XML remain real. */
    private static final class ControlledFile extends android.util.AtomicFile {
        final AtomicInteger writes = new AtomicInteger();
        final java.util.concurrent.CountDownLatch entered = new java.util.concurrent.CountDownLatch(1);
        final java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        volatile boolean failFinish;
        ControlledFile(File path) { super(path); }
        @Override public java.io.FileOutputStream startWrite() throws java.io.IOException {
            if (writes.incrementAndGet() == 1) {
                entered.countDown();
                try {
                    if (!release.await(15, TimeUnit.SECONDS)) throw new java.io.IOException("Timed out in isolated slow-disk fixture");
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt(); throw new java.io.IOException(interrupted);
                }
            }
            return super.startWrite();
        }
        @Override public void finishWrite(java.io.FileOutputStream output) {
            if (failFinish) throw new IllegalStateException("Isolated atomic-commit failure");
            super.finishWrite(output);
        }
    }
    private static void persistenceScheduling(Harness h, File directory) throws Exception {
        File path = File.createTempFile("coalesced-", ".xml", directory); path.delete();
        AppOpsService service = persistent(h, path);
        ControlledFile disk = new ControlledFile(path); set(get(service, "mStore"), "mFile", disk);
        Watcher watcher = new Watcher(); service.startWatchingMode(OP_VIBRATE, NAME, watcher);
        service.setMode(OP_VIBRATE, UID, NAME, MODE_IGNORED);
        check(service.checkRawMode(OP_VIBRATE, UID, NAME) == MODE_IGNORED, "The ordinary setter publishes before deferred serialization");
        check(disk.entered.await(5, TimeUnit.SECONDS), "The deferred writer eventually runs without a forced reload");
        try {
            java.util.concurrent.FutureTask<Void> mutation = new java.util.concurrent.FutureTask<>(() -> {
                synchronized (h.pm) { synchronized (get(service, "mLock")) { } }
                for (int i = 0; i < 20; i++) service.setMode(OP_VIBRATE, UID, NAME,
                        i % 2 == 0 ? MODE_DEFAULT : MODE_IGNORED);
                service.setMode(OP_VIBRATE, UID, NAME, MODE_ERRORED);
                return null;
            });
            Thread publisher = new Thread(mutation, "appops-persistence-contract"); publisher.setDaemon(true); publisher.start();
            mutation.get(3, TimeUnit.SECONDS);
            check(service.checkRawMode(OP_VIBRATE, UID, NAME) == MODE_ERRORED,
                    "Blocked serialization holds neither policy publication nor package/operation monitors");
            awaitCount(watcher.calls, 22);
            check(disk.writes.get() == 1, "Mutations during a blocked disk write coalesce without concurrent serialization");
        } finally { disk.release.countDown(); }
        service.reloadNonHistoricalState();
        check(disk.writes.get() == 2, "Explicit flush writes only the latest coalesced snapshot after the in-flight write");
        check(persistent(h, path).checkRawMode(OP_VIBRATE, UID, NAME) == MODE_ERRORED,
                "An older in-flight write cannot replace the newest durable policy");
        service.stopWatchingMode(watcher);
        check(watcher.token.recipients.isEmpty(), "Persistence callbacks release their watcher registration");

        disk.failFinish = true;
        service.setMode(OP_VIBRATE, UID, NAME, MODE_IGNORED);
        service.reloadNonHistoricalState();
        check(service.checkRawMode(OP_VIBRATE, UID, NAME) == MODE_IGNORED,
                "A failed flush/reload retains the newer in-memory decision");
        check(persistent(h, path).checkRawMode(OP_VIBRATE, UID, NAME) == MODE_ERRORED,
                "An interrupted atomic commit leaves the prior complete disk snapshot readable");
        disk.failFinish = false; service.reloadNonHistoricalState();
        check(persistent(h, path).checkRawMode(OP_VIBRATE, UID, NAME) == MODE_IGNORED,
                "A later explicit flush retries the latest dirty policy successfully");
        path.delete();

        File parent = File.createTempFile("unavailable-parent-", ".file", directory);
        File unavailable = new File(parent, "appops.xml");
        AppOpsService noStorage = persistent(h, unavailable);
        Watcher failedWriter = new Watcher(); noStorage.startWatchingMode(OP_VIBRATE, NAME, failedWriter);
        noStorage.setMode(OP_VIBRATE, UID, NAME, MODE_IGNORED);
        awaitCount(failedWriter.calls, 1);
        noStorage.reloadNonHistoricalState();
        check(noStorage.checkRawMode(OP_VIBRATE, UID, NAME) == MODE_IGNORED,
                "An unavailable state parent cannot abort synchronous policy or observer publication");
        noStorage.setMode(OP_VIBRATE, UID, NAME, MODE_DEFAULT);
        check(parent.delete() && parent.mkdir(), "The isolated unavailable state path can recover");
        noStorage.reloadNonHistoricalState();
        check(persistent(h, unavailable).checkRawMode(OP_VIBRATE, UID, NAME) == MODE_DEFAULT,
                "Storage recovery persists the current decision without reverting it");
        noStorage.stopWatchingMode(failedWriter); unavailable.delete(); parent.delete();

        Object policy = h.pm.permissions(), permissionWriter = get(policy, "mWriter");
        check((Boolean)call(permissionWriter, "flush", new Class<?>[0]), "Permission state reaches its explicit durability boundary");
        android.util.AtomicFile permissionFile = (android.util.AtomicFile)get(policy, "mPermissionFile");
        File blocked = File.createTempFile("permission-parent-", ".file", directory);
        try {
            set(policy, "mPermissionFile", new android.util.AtomicFile(new File(blocked, "permissions")));
            h.ops.setUidMode(OP_CAMERA, UID, MODE_IGNORED);
            check(h.pm.permissions().checkPermission(NAME, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                    && h.ops.checkOperationRaw(OP_CAMERA, UID, NAME, null) == MODE_IGNORED,
                    "Runtime UID setters publish compatibility flags even while their permission file is unavailable");
            check(!(Boolean)call(permissionWriter, "flush", new Class<?>[0]), "Permission serialization failure is contained by the writer");
        } finally { set(policy, "mPermissionFile", permissionFile); blocked.delete(); }
        check((Boolean)call(permissionWriter, "flush", new Class<?>[0]), "A recovered permission writer retries its latest flag snapshot");
        Object restored = restoredPermissions(h);
        check((Integer)call(restored, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_CAMERA, UID) == MODE_IGNORED,
                "Compatibility permission flags survive restart as the source of runtime AppOps policy");
        h.ops.setUidMode(OP_CAMERA, UID, MODE_ALLOWED);
        flags(h, Manifest.permission.CAMERA, PackageManager.FLAG_PERMISSION_ONE_TIME | PackageManager.FLAG_PERMISSION_USER_SET,
                PackageManager.FLAG_PERMISSION_ONE_TIME | PackageManager.FLAG_PERMISSION_USER_SET);
        check((Boolean)call(permissionWriter, "flush", new Class<?>[0]), "A session flag snapshot can be serialized without conferring a durable grant");
        restored = restoredPermissions(h);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                && (Integer)call(restored, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_CAMERA, UID) == MODE_IGNORED,
                "A one-time grant remains live in memory but cannot survive policy restart");
        flags(h, Manifest.permission.CAMERA, PackageManager.FLAG_PERMISSION_ONE_TIME | PackageManager.FLAG_PERMISSION_USER_SET, 0);
        check((Boolean)call(permissionWriter, "flush", new Class<?>[0]), "The final restored runtime permission state is durable");
    }
    private static Object restoredPermissions(Harness h) throws Exception {
        Object restored = make(type("PermissionPolicy"), new Class<?>[]{PackageManagerService.class}, h.pm);
        if (!binder) {
            Object catalog = get(restored, "mCatalog");
            set(catalog, "mFramework", new java.util.LinkedHashMap<>((java.util.Map<?, ?>)get(get(h.pm.permissions(), "mCatalog"), "mFramework")));
            set(catalog, "mLoaded", true);
        }
        return restored;
    }

    private static void drainPackages(Harness h) throws Exception {
        ((java.util.concurrent.ExecutorService)get(h.pm, "mPackageEventExecutor")).submit(() -> { }).get(5, TimeUnit.SECONDS);
        drain(h); drain(h);
    }

    private static void publish(Harness h, ApkPackage pkg, boolean replacing) throws Exception {
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, pkg);
        call(h.pm, "packageInstalled", new Class<?>[]{ApkPackage.class, boolean.class, Object.class}, pkg, replacing, new Object());
        drainPackages(h);
    }

    private static void retire(Harness h, ApkPackage pkg) throws Exception {
        call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, pkg);
        call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, pkg.packageName);
        h.pm.permissions().forgetPackage(pkg.packageName);
        call(h.pm, "remove", new Class<?>[]{String.class}, pkg.packageName);
        call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, pkg);
        drainPackages(h);
    }

    private static Object rebootPermissions(Harness h) throws Exception {
        Object policy = restoredPermissions(h);
        call(policy, "initializePackages", new Class<?>[0]);
        check((Boolean)call(get(policy, "mWriter"), "flush", new Class<?>[0]), "Boot permission reconciliation reaches durable state");
        return policy;
    }

    private static int permissionFlags(Object policy, String name, String permission) throws Exception {
        return (Integer)call(policy, "getPermissionFlags", new Class<?>[]{String.class, String.class, String.class, int.class},
                name, permission, "default:0", 0);
    }

    private static Object activateServices(Harness h) throws Exception {
        Object previous = field(type("Services"), "sLast").get(null);
        field(type("Services"), "sLast").set(null, make(type("Services$Created"),
                new Class<?>[]{PackageManagerService.class, h.atm.getClass(), h.am.getClass()}, h.pm, h.atm, h.am));
        return previous;
    }

    /** AOSP's ordinary runtime-flag transitions, through actual package publication. */
    private static void legacyTransition(boolean priorGrant, boolean userDecision, boolean clearCompat,
            boolean clearReview, boolean removeLegacy) throws Exception {
        Harness h = new Harness(); requested(h.owner, 36, Manifest.permission.READ_CONTACTS);
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.MASK_PERMISSION_FLAGS_ALL, 0);
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0, "transition setup");
        if (priorGrant) h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0);
        if (userDecision) flags(h, Manifest.permission.READ_CONTACTS,
                PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED,
                PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED);
        drainPackages(h);
        ApkPackage legacy = pkg("fixture.appops.transition.legacy", UID, (byte)7, 22);
        requested(legacy, 22, Manifest.permission.READ_CONTACTS);
        Watcher watcher = new Watcher(); h.service.startWatchingMode(OP_READ_CONTACTS, NAME, watcher);
        Token operation = new Token(); AtomicInteger exits = new AtomicInteger();
        set(h.process, "thread", new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void scheduleExit() { exits.incrementAndGet(); operation.die(); }
        });
        Object previousServices = activateServices(h);
        try {
            publish(h, legacy, false);
            boolean deniedLegacyOp = !priorGrant && userDecision;
            check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED,
                    "Actual legacy requester admission installs the UID legacy grant");
            int admittedFlags = permissionFlags(h.pm.permissions(), NAME, Manifest.permission.READ_CONTACTS);
            check(h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, NAME, null) == (deniedLegacyOp ? MODE_IGNORED : MODE_ALLOWED)
                    && ((admittedFlags & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) != 0) == deniedLegacyOp,
                    "Publication preserves a previous user denial as AppOps compatibility revocation");
            check(((admittedFlags & PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED) != 0) == (!priorGrant && !userDecision),
                    "Only a previously empty permission state receives automatic legacy review");
            if (clearCompat) h.ops.setUidMode(OP_READ_CONTACTS, UID, MODE_ALLOWED);
            if (clearReview) flags(h, Manifest.permission.READ_CONTACTS, PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED, 0);
            drainPackages(h);
            int expectedLegacyMode = deniedLegacyOp && !clearCompat ? MODE_IGNORED : MODE_ALLOWED;
            check(h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, NAME, null) == expectedLegacyMode,
                    "An authorized compatibility or review decision takes effect on the existing legacy grant");
            int flagsAfterDecision = permissionFlags(h.pm.permissions(), NAME, Manifest.permission.READ_CONTACTS);
            for (int i = 0; i < 3; i++) {
                h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS);
                h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, NAME, null);
            }
            check(permissionFlags(h.pm.permissions(), NAME, Manifest.permission.READ_CONTACTS) == flagsAfterDecision,
                    "Permission and mode reads never reintroduce cleared compatibility revocation");
            ApkPackage republished = pkg(legacy.packageName, UID, (byte)7, 22);
            requested(republished, 22, Manifest.permission.READ_CONTACTS);
            publish(h, republished, true); legacy = republished;
            check(h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, NAME, null) == expectedLegacyMode
                    && permissionFlags(h.pm.permissions(), NAME, Manifest.permission.READ_CONTACTS) == flagsAfterDecision,
                    "A later legacy publication retains the prior compatibility and review decision");
            check((Boolean)call(get(h.pm.permissions(), "mWriter"), "flush", new Class<?>[0]), "The reconciled legacy basis and flags persist together");
            Object reboot = rebootPermissions(h);
            check((Integer)call(reboot, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_READ_CONTACTS, UID) == expectedLegacyMode
                    && permissionFlags(reboot, NAME, Manifest.permission.READ_CONTACTS) == flagsAfterDecision,
                    "Boot reconciliation preserves stored legacy grant basis, including a legitimate compat clear");
            if (expectedLegacyMode == MODE_ALLOWED) h.ops.startOperation(operation, OP_READ_CONTACTS, UID, NAME, null,
                    false, false, null, false, 0, -1);
            if (removeLegacy) {
                retire(h, legacy);
                check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED
                        && h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, NAME, null) == expectedLegacyMode,
                        "Removing one requester retains the UID flag state still requested by a surviving member");
                ApkPackage modern = pkg(NAME, UID, (byte)7);
                requested(modern, 36, Manifest.permission.READ_CONTACTS);
                publish(h, modern, true);
            } else {
                ApkPackage modern = pkg(legacy.packageName, UID, (byte)7);
                requested(modern, 36, Manifest.permission.READ_CONTACTS);
                publish(h, modern, true);
            }
            boolean finalGrant = priorGrant || clearCompat || clearReview;
            check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS)
                    == (finalGrant ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED)
                    && h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, NAME, null) == (finalGrant ? MODE_ALLOWED : MODE_IGNORED),
                    "Modern publication promotes reviewed grants and revokes unreviewed or compatibility-denied grants");
            int upgradedFlags = permissionFlags(h.pm.permissions(), NAME, Manifest.permission.READ_CONTACTS);
            check((upgradedFlags & (PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED
                    | PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED | PackageManager.FLAG_PERMISSION_REVOKED_COMPAT)) == 0,
                    "Modern reconciliation removes obsolete legacy review and compatibility state");
            check(exits.get() == (finalGrant ? 0 : 1) && (Boolean)get(h.process, "dead") == !finalGrant,
                    "Disappearing legacy binary authority follows normal UID process retirement even when raw mode stays ignored");
            check(h.ops.isOperationActive(OP_READ_CONTACTS, UID, NAME) == (finalGrant && !removeLegacy),
                    "Grant loss or package replacement cannot retain the old operation generation");
            check((Boolean)call(get(h.pm.permissions(), "mWriter"), "flush", new Class<?>[0]), "Modern transition flags are durable");
            reboot = rebootPermissions(h);
            check((Integer)call(reboot, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_READ_CONTACTS, UID)
                    == (finalGrant ? MODE_ALLOWED : MODE_IGNORED), "The modern transition survives policy restart");
        } finally {
            field(type("Services"), "sLast").set(null, previousServices);
            h.service.stopWatchingMode(watcher); operation.die();
        }
        check(watcher.token.recipients.isEmpty() && operation.recipients.isEmpty(), "Transition observers and client ownership are released");
    }

    private static void runtimeOnlyPublication() throws Exception {
        Harness h = new Harness(); requested(h.owner, 36, Manifest.permission.ANSWER_PHONE_CALLS);
        h.pm.permissions().updatePermissionFlags(NAME, Manifest.permission.ANSWER_PHONE_CALLS, PackageManager.MASK_PERMISSION_FLAGS_ALL, 0,
                true, "default:0", 0);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0);
        h.pm.permissions().updatePermissionFlags(NAME, Manifest.permission.ANSWER_PHONE_CALLS,
                PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED | PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED | PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                true, "default:0", 0);
        drainPackages(h);
        ApkPackage legacy = pkg("fixture.appops.transition.runtimeonly", UID, (byte)7, 22);
        requested(legacy, 22, Manifest.permission.ANSWER_PHONE_CALLS);
        publish(h, legacy, false);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_DENIED
                && permissionFlags(h.pm.permissions(), NAME, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                "Runtime-only legacy admission removes previous runtime and user flags while retaining restriction exemptions");
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0);
        ApkPackage unrelated = pkg("fixture.appops.transition.unrelated", UID, (byte)7, 22);
        requested(unrelated, 22); publish(h, unrelated, false);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_GRANTED,
                "Publishing a nonrequester does not reevaluate an unrelated runtime-only grant");
        ApkPackage modern = pkg(legacy.packageName, UID, (byte)7);
        requested(modern, 36, Manifest.permission.ANSWER_PHONE_CALLS); publish(h, modern, true);
        check(h.ops.checkOperationRaw(OP_ANSWER_PHONE_CALLS, UID, NAME, null) == MODE_ALLOWED,
                "A valid explicit runtime-only grant is retained when its requesters become modern");
        retire(h, modern); retire(h, unrelated);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_GRANTED,
                "Removing shared siblings retains the explicit grant still requested by the modern owner");
    }

    private static void publicationRegrant() throws Exception {
        Harness h = new Harness(); requested(h.owner, 36, Manifest.permission.ANSWER_PHONE_CALLS);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0);
        drainPackages(h);
        Watcher watcher = new Watcher(); h.service.startWatchingMode(OP_ANSWER_PHONE_CALLS, NAME, watcher);
        AtomicInteger exits = new AtomicInteger();
        set(h.process, "thread", new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void scheduleExit() { exits.incrementAndGet(); }
        });
        Object previousServices = activateServices(h), publicationLock = new Object();
        ApkPackage legacy = pkg("fixture.appops.transition.locked", UID, (byte)7, 22);
        requested(legacy, 22, Manifest.permission.ANSWER_PHONE_CALLS);
        try {
            synchronized (publicationLock) {
                call(h.pm, "add", new Class<?>[]{ApkPackage.class, Object.class}, legacy, publicationLock);
                check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_DENIED,
                        "Publication exposes reconciled permission policy before returning from the package transaction");
                h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0);
                check(watcher.calls.get() == 0 && exits.get() == 0, "Publication locks gate callbacks and process retirement, not synchronous policy updates");
            }
            drainPackages(h); awaitCount(watcher.calls, 2);
            check(exits.get() == 0 && !(Boolean)get(h.process, "dead"),
                    "Queued revocation rechecks a newer legitimate grant before retiring a process generation");
        } finally {
            field(type("Services"), "sLast").set(null, previousServices); h.service.stopWatchingMode(watcher);
        }
        check(watcher.token.recipients.isEmpty(), "Publication ordering leaves no watcher death registration");
    }

    private static void transitionRestartAfterWriteFailure() throws Exception {
        Harness h = new Harness(); requested(h.owner, 36, Manifest.permission.READ_CONTACTS);
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.MASK_PERMISSION_FLAGS_ALL, 0);
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0, "restart transition");
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED,
                PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED);
        Object policy = h.pm.permissions(), writer = get(policy, "mWriter");
        check((Boolean)call(writer, "flush", new Class<?>[0]), "The prior user denial is durably recorded before package admission");
        android.util.AtomicFile original = (android.util.AtomicFile)get(policy, "mPermissionFile");
        ControlledFile disk = new ControlledFile(original.getBaseFile()); disk.release.countDown(); disk.failFinish = true;
        set(policy, "mPermissionFile", disk);
        try {
            ApkPackage legacy = pkg("fixture.appops.transition.recovery", UID, (byte)7, 22);
            requested(legacy, 22, Manifest.permission.READ_CONTACTS); publish(h, legacy, false);
            check(!(Boolean)call(writer, "flush", new Class<?>[0]), "A failed deferred transition write keeps the earlier complete permission snapshot");
            Object reboot = rebootPermissions(h);
            check((Integer)call(reboot, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_READ_CONTACTS, UID) == MODE_IGNORED
                    && (permissionFlags(reboot, NAME, Manifest.permission.READ_CONTACTS) & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) != 0,
                    "Boot reconciles the installed legacy requester with the prior durable user denial after a write failure");
        } finally { set(policy, "mPermissionFile", original); call(writer, "flush", new Class<?>[0]); }
    }

    private static void requested(ApkPackage pkg, int target, String... permissions) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(pkg.packageName, pkg.apkPath);
        parsed.setEnabled(true); parsed.setTargetSdkVersion(target);
        for (String permission : permissions) parsed.addUsesPermission(
                new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(permission, 0, java.util.Set.of()));
        parsed.hideAsFinal(); set(pkg, "parsed", parsed);
    }

    @SuppressWarnings("unchecked")
    private static Object attachedProcess(Harness h, ApkPackage pkg, int pid, IApplicationThread thread) throws Exception {
        long identity = Binder.clearCallingIdentity();
        Object process;
        try {
            Binder.restoreCallingIdentity(((long)pkg.uid() << 32) | pid);
            process = make(type("ActivityTaskManagerService$ProcessRecord"),
                    new Class<?>[]{h.atm.getClass(), int.class, IApplicationThread.class}, h.atm, pid, thread);
        } finally { Binder.restoreCallingIdentity(identity); }
        set(process, "packageName", pkg.packageName); set(process, "processName", pkg.packageName + ":" + pid);
        set(process, "packageCode", pkg); set(process, "ready", true);
        ((List<Object>)get(h.atm, "mProcesses")).add(process);
        return process;
    }

    /** Real uninstall stages and killUid/processDied, with only APK, client death and disk boundaries controlled. */
    private static void sharedRetirement(boolean stillRequested, boolean legacy, boolean failDisk) throws Exception {
        Harness h = new Harness(legacy ? 22 : 36, Manifest.permission.READ_CONTACTS);
        ApkPackage sibling = pkg("fixture.appops.remaining", UID, (byte)7);
        requested(sibling, 36, stillRequested ? new String[]{Manifest.permission.READ_CONTACTS} : new String[0]);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, sibling);
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.MASK_PERMISSION_FLAGS_ALL,
                PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0);
        drain(h); drain(h);
        Object policy = h.pm.permissions(), writer = get(policy, "mWriter");
        check((Boolean)call(writer, "flush", new Class<?>[0]), "The shared-UID grant is durable before retirement");
        android.util.AtomicFile originalFile = (android.util.AtomicFile)get(policy, "mPermissionFile");
        ControlledFile disk = new ControlledFile(originalFile.getBaseFile()); disk.failFinish = failDisk;
        Object previousServices = field(type("Services"), "sLast").get(null);
        Object currentServices = make(type("Services$Created"),
                new Class<?>[]{PackageManagerService.class, h.atm.getClass(), h.am.getClass()}, h.pm, h.atm, h.am);
        Token operation = new Token(), delivery = new Token(), secondOperation = new Token(), unrelatedOperation = new Token();
        AtomicInteger exits = new AtomicInteger(), secondExits = new AtomicInteger(), unrelatedExits = new AtomicInteger();
        java.util.concurrent.atomic.AtomicBoolean unlocked = new java.util.concurrent.atomic.AtomicBoolean(true);
        Object operationLock = get(h.service, "mLock");
        IApplicationThread exiting = new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void scheduleExit() {
                if (Thread.holdsLock(h.pm) || Thread.holdsLock(policy) || Thread.holdsLock(h.atm)
                        || Thread.holdsLock(operationLock)) unlocked.set(false);
                exits.incrementAndGet(); operation.die(); delivery.die();
            }
        };
        set(h.process, "packageName", sibling.packageName); set(h.process, "processName", sibling.packageName);
        set(h.process, "packageCode", sibling); set(h.process, "thread", exiting);
        Object second = attachedProcess(h, sibling, PID + 1, new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void scheduleExit() { secondExits.incrementAndGet(); secondOperation.die(); }
        });
        Object unrelated = attachedProcess(h, h.peer, PID + 2, new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void scheduleExit() { unrelatedExits.incrementAndGet(); unrelatedOperation.die(); }
        });
        Watcher watcher = new Watcher(); h.service.startWatchingMode(OP_READ_CONTACTS, sibling.packageName, watcher);
        java.util.concurrent.FutureTask<Void> retirement = null;
        try {
            field(type("Services"), "sLast").set(null, currentServices);
            check(h.ops.startOperation(operation, OP_READ_CONTACTS, UID, sibling.packageName, null,
                    false, false, null, false, 0, -1).getOpMode() == MODE_ALLOWED,
                    "The sibling uses the UID grant before its requester leaves");
            h.ops.startOperation(secondOperation, OP_READ_CONTACTS, UID, sibling.packageName, null,
                    false, false, null, false, 0, -1);
            h.ops.startOperation(unrelatedOperation, OP_VIBRATE, UID + 1, PEER, null,
                    false, false, null, false, 0, -1);
            AttributionSourceState source = h.source(sibling, delivery); source.attributionTag = null;
            h.asApp(() -> check(h.checker.checkPermission(Manifest.permission.READ_CONTACTS, source, null,
                    true, true, false, OP_NONE) == PERMISSION_GRANTED, "The shared UID owns a real permission data delivery"));
            call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
            call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, NAME);
            check(h.ops.isOperationActive(OP_READ_CONTACTS, UID, sibling.packageName),
                    "Removing the departing package's capabilities does not preempt the sibling's permission transition");
            set(policy, "mPermissionFile", disk);
            retirement = new java.util.concurrent.FutureTask<>(() -> { h.pm.permissions().forgetPackage(NAME); return null; });
            Thread remover = new Thread(retirement, "shared-permission-retirement-contract"); remover.setDaemon(true); remover.start();
            check(disk.entered.await(5, TimeUnit.SECONDS), "Uninstall reaches its explicit permission durability boundary");
            java.util.concurrent.FutureTask<Void> inspect = new java.util.concurrent.FutureTask<>(() -> {
                synchronized (h.pm) { synchronized (policy) { synchronized (operationLock) { } } }
                return null;
            });
            Thread observer = new Thread(inspect, "shared-retirement-monitor-contract"); observer.setDaemon(true); observer.start();
            inspect.get(3, TimeUnit.SECONDS);
            check(!retirement.isDone(), "Disk durability waits without retaining package, permission or AppOps monitors");
            drain(h); drain(h); awaitCount(watcher.calls, stillRequested ? 0 : 1);
            check(h.pm.permissions().checkPermission(sibling.packageName, Manifest.permission.READ_CONTACTS)
                    == (stillRequested ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED)
                    && h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, sibling.packageName, null)
                    == (stillRequested ? MODE_ALLOWED : MODE_IGNORED),
                    "Live permission and raw AppOps use the surviving membership before registry removal");
            check(h.ops.isOperationActive(OP_READ_CONTACTS, UID, sibling.packageName) == stillRequested,
                    "Retirement updates active shared-UID operations even while persistence is blocked");
            check(exits.get() == (stillRequested ? 0 : 1) && secondExits.get() == (stillRequested ? 0 : 1)
                    && (Boolean)get(h.process, "dead") == !stillRequested && (Boolean)get(second, "dead") == !stillRequested,
                    "Grant loss retires every attached shared-UID process through normal processDied exactly once");
            check(unlocked.get() && unrelatedExits.get() == 0 && !(Boolean)get(unrelated, "dead")
                    && h.ops.isOperationActive(OP_VIBRATE, UID + 1, PEER),
                    "UID revocation calls clients outside service monitors and preserves unrelated processes");
            check((operation.recipients.isEmpty() && delivery.recipients.isEmpty() && secondOperation.recipients.isEmpty())
                    == !stillRequested, "Normal process retirement and Binder death release starts and delivery ownership");
            int retainedFlags = h.pm.permissions().getPermissionFlags(sibling.packageName,
                    Manifest.permission.READ_CONTACTS, "default:0", 0);
            check(stillRequested ? (retainedFlags & PackageManager.FLAG_PERMISSION_USER_FIXED) != 0 : retainedFlags == 0,
                    "Permission trimming retains a surviving requester's flags and removes an unrequested UID grant");
            disk.release.countDown();
            if (failDisk) {
                java.util.concurrent.FutureTask<Void> pending = retirement;
                rejects(IllegalStateException.class, () -> {
                    try { pending.get(5, TimeUnit.SECONDS); }
                    catch (java.util.concurrent.ExecutionException failure) { throw (Exception)failure.getCause(); }
                });
                check(h.pm.get(NAME) == h.owner, "A failed durability boundary has not removed the installed package");
                if (legacy) call(h.pm, "recoveredPackageRemovals", new Class<?>[0]);
                else call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
                drain(h); drain(h); awaitCount(watcher.calls, legacy ? 2 : 1);
                check(h.pm.permissions().checkPermission(sibling.packageName, Manifest.permission.READ_CONTACTS)
                        == (legacy ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED),
                        "Rollback restores installed legacy manifest authority without resurrecting deleted runtime grants");
                check(!h.ops.isOperationActive(OP_READ_CONTACTS, UID, sibling.packageName),
                        "Rollback cannot resurrect a retired process's starts or data delivery");
            } else {
                retirement.get(5, TimeUnit.SECONDS);
                call(h.pm, "remove", new Class<?>[]{String.class}, NAME);
                call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
                Object restored = restoredPermissions(h);
                check((Integer)call(restored, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_READ_CONTACTS, UID)
                        == (stillRequested ? MODE_ALLOWED : MODE_IGNORED),
                        "A restarted policy agrees with the committed shared-UID retirement");
                ApkPackage replacement = pkg(NAME, UID, (byte)7);
                requested(replacement, 36, Manifest.permission.READ_CONTACTS);
                call(h.pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
                check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS)
                        == (stillRequested ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED),
                        "Reinstall uses current shared-UID permission state and cannot inherit the removed grant");
            }
            check(((java.util.Map<?, ?>)get(policy, "mForgottenPackages")).isEmpty(),
                    "Committed and rolled-back removals release their generation retirement markers");
        } finally {
            disk.release.countDown();
            if (retirement != null) try { retirement.get(5, TimeUnit.SECONDS); } catch (java.util.concurrent.ExecutionException expected) { }
            set(policy, "mPermissionFile", originalFile);
            call(writer, "flush", new Class<?>[0]);
            field(type("Services"), "sLast").set(null, previousServices);
            h.service.stopWatchingMode(watcher);
            operation.die(); delivery.die(); secondOperation.die(); unrelatedOperation.die();
        }
        check(watcher.token.recipients.isEmpty() && ((java.util.Map<?, ?>)get(h.service, "mClients")).isEmpty(),
                "Shared retirement leaves no client starts or watcher death registrations after teardown");
    }

    private static void retirementGeneration() throws Exception {
        Harness h = new Harness(); requested(h.owner, 36, Manifest.permission.READ_CONTACTS);
        ApkPackage sibling = pkg("fixture.appops.remaining", UID, (byte)7);
        requested(sibling, 36); call(h.pm, "add", new Class<?>[]{ApkPackage.class}, sibling);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0);
        drain(h); drain(h);
        ApkPackage replacement = pkg(sibling.packageName, UID, (byte)7);
        requested(replacement, 36, Manifest.permission.READ_CONTACTS);
        Token operation = new Token(); AtomicInteger exits = new AtomicInteger(), laterExits = new AtomicInteger();
        set(h.process, "packageName", sibling.packageName); set(h.process, "processName", sibling.packageName);
        set(h.process, "packageCode", sibling);
        set(h.process, "thread", new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void scheduleExit() {
                exits.incrementAndGet();
                try {
                    call(h.pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
                    h.pm.permissions().grantRuntimePermission(replacement.packageName,
                            Manifest.permission.READ_CONTACTS, "default:0", 0);
                } catch (Exception failure) { throw new AssertionError(failure); }
            }
        });
        Object later = attachedProcess(h, sibling, PID + 1, new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void scheduleExit() { laterExits.incrementAndGet(); operation.die(); }
        });
        Watcher watcher = new Watcher(); h.service.startWatchingMode(OP_READ_CONTACTS, sibling.packageName, watcher);
        h.ops.startOperation(operation, OP_READ_CONTACTS, UID, sibling.packageName, null,
                false, false, null, false, 0, -1);
        java.util.concurrent.CountDownLatch blocked = new java.util.concurrent.CountDownLatch(1), release = new java.util.concurrent.CountDownLatch(1);
        ((java.util.concurrent.ScheduledThreadPoolExecutor)get(h.service, "mEvents")).execute(() -> {
            blocked.countDown();
            try { if (!release.await(10, TimeUnit.SECONDS)) throw new AssertionError("Generation callback barrier timed out"); }
            catch (InterruptedException failure) { Thread.currentThread().interrupt(); throw new AssertionError(failure); }
        });
        check(blocked.await(5, TimeUnit.SECONDS), "The lifecycle race holds only the explicit callback boundary");
        Object previousServices = field(type("Services"), "sLast").get(null);
        try {
            field(type("Services"), "sLast").set(null, make(type("Services$Created"),
                    new Class<?>[]{PackageManagerService.class, h.atm.getClass(), h.am.getClass()}, h.pm, h.atm, h.am));
            call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
            call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, NAME);
            h.pm.permissions().forgetPackage(NAME);
            call(h.pm, "remove", new Class<?>[]{String.class}, NAME);
            call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
            check(exits.get() == 1 && laterExits.get() == 0 && !(Boolean)get(later, "dead"),
                    "Reentrant package replacement invalidates the old UID revocation principal before another exit");
            check(h.pm.permissions().checkPermission(sibling.packageName, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED,
                    "A current requesting generation can receive a legitimate new grant during client reentry");
            release.countDown(); drain(h); drain(h); awaitCount(watcher.calls, 1);
            check(h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, sibling.packageName, null) == MODE_ALLOWED
                    && !h.ops.isOperationActive(OP_READ_CONTACTS, UID, sibling.packageName),
                    "Queued retirement callbacks cannot revive the old generation under its replacement's new grant");
            Object restored = restoredPermissions(h);
            check((Integer)call(restored, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_READ_CONTACTS, UID) == MODE_ALLOWED,
                    "The retirement flush persists the newer reentrant grant, not an obsolete revoked snapshot");
        } finally {
            release.countDown(); field(type("Services"), "sLast").set(null, previousServices);
            h.service.stopWatchingMode(watcher); operation.die();
        }
        check(watcher.token.recipients.isEmpty() && operation.recipients.isEmpty(),
                "Generation races release the old operation and mode watcher death registrations");
    }

    private static void legacyEligibility() throws Exception {
        Harness h = new Harness(); requested(h.owner, 36, Manifest.permission.READ_CONTACTS);
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.MASK_PERMISSION_FLAGS_ALL, 0);
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0, "target scope");
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_DENIED,
                "The modern permission requester begins with an explicit denial");
        ApkPackage legacy = pkg("fixture.appops.scope.legacy", UID, (byte)7, 22);
        requested(legacy, 22, Manifest.permission.ANSWER_PHONE_CALLS);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, legacy);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_DENIED
                && h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_DENIED,
                "A nonrequesting legacy sibling cannot turn a modern UID denial into an automatic grant");
        check(h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, NAME, null) == MODE_IGNORED
                && h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, legacy.packageName, null) == MODE_IGNORED,
                "Raw AppOps shares the per-permission requesting-target decision across the UID");
        check(h.pm.permissions().getPermissionFlags(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0) == 0,
                "An unrelated legacy target does not supply an automatic permission review flag");
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED,
                PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED);
        check(h.pm.permissions().getPermissionFlags(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0)
                == PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED,
                "An implicit flag uses the actual permission requester's modern eligibility");
        flags(h, Manifest.permission.READ_CONTACTS, PackageManager.MASK_PERMISSION_FLAGS_ALL, 0);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0);
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_READ_CONTACTS, UID, legacy.packageName, null) == MODE_ALLOWED,
                "A legitimate modern grant propagates to every admitted UID sibling");
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0, "target scope");
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_DENIED,
                "Revoking the modern grant is not overridden by the nonrequesting legacy sibling");

        PermissionInfo runtimeOnly = h.pm.permissions().getPermissionInfo(Manifest.permission.ANSWER_PHONE_CALLS, null, 0);
        check(runtimeOnly.isRuntime() && (runtimeOnly.protectionLevel & PermissionInfo.PROTECTION_FLAG_RUNTIME_ONLY) != 0,
                "The runtime-only fixture uses the pinned dangerous|runtime platform definition");
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_DENIED
                && h.ops.checkOperationRaw(OP_ANSWER_PHONE_CALLS, UID, legacy.packageName, null) == MODE_IGNORED,
                "A runtime-only permission is not automatically granted to its legacy requester");
        check(h.pm.permissions().getPermissionFlags(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0) == 0,
                "An ungranted runtime-only permission has no automatic legacy review or user flags");
        h.pm.permissions().grantRuntimePermission(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0);
        h.ops.setUidMode(OP_ANSWER_PHONE_CALLS, UID, MODE_ALLOWED);
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_DENIED
                && h.ops.checkOperationRaw(OP_ANSWER_PHONE_CALLS, UID, legacy.packageName, null) == MODE_IGNORED,
                "Neither a legacy runtime grant call nor an AppOps setter invents runtime-only authority");
        h.pm.permissions().updatePermissionFlags(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS,
                PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT, PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                true, "default:0", 0);
        check(h.pm.permissions().getPermissionFlags(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0)
                == PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                "Runtime-only legacy eligibility retains an explicit restriction exemption without supplying a grant");
        h.pm.permissions().updatePermissionFlags(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS,
                PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED, PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED, true, "default:0", 0);
        check(h.pm.permissions().getPermissionFlags(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0)
                == (PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT | PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED),
                "The flag API cannot report LEGACY_GRANTED review semantics for a runtime-only permission");
        h.pm.permissions().updatePermissionFlags(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS,
                PackageManager.MASK_PERMISSION_FLAGS_ALL, 0, true, "default:0", 0);

        requested(h.owner, 36, Manifest.permission.READ_CONTACTS, Manifest.permission.ANSWER_PHONE_CALLS);
        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0);
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_ANSWER_PHONE_CALLS, UID, legacy.packageName, null) == MODE_ALLOWED,
                "A modern requester can receive an explicit runtime-only grant shared with its legacy UID sibling");
        h.asApp(() -> rejects(SecurityException.class, () -> h.pm.permissions().grantRuntimePermission(NAME,
                Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0)));
        h.ops.setUidMode(OP_ANSWER_PHONE_CALLS, UID, MODE_IGNORED);
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_ANSWER_PHONE_CALLS, UID, legacy.packageName, null) == MODE_IGNORED,
                "A legitimate runtime-only grant still obeys compatibility flags without losing its binary grant");
        h.ops.setUidMode(OP_ANSWER_PHONE_CALLS, UID, MODE_ALLOWED);
        check(h.ops.checkOperationRaw(OP_ANSWER_PHONE_CALLS, UID, legacy.packageName, null) == MODE_ALLOWED,
                "Clearing compatibility restores only the already granted runtime-only operation");
        h.pm.permissions().revokeRuntimePermission(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0, "legacy API");
        check(h.pm.permissions().checkPermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_GRANTED,
                "The legacy package grant API leaves an existing modern runtime grant unchanged");
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.ANSWER_PHONE_CALLS, "default:0", 0, "modern API");
        check(h.pm.permissions().checkPermission(legacy.packageName, Manifest.permission.ANSWER_PHONE_CALLS) == PackageManager.PERMISSION_DENIED
                && h.ops.checkOperationRaw(OP_ANSWER_PHONE_CALLS, UID, legacy.packageName, null) == MODE_IGNORED,
                "Modern revocation does not fall back to a runtime-only legacy grant");

        ApkPackage normalLegacy = pkg("fixture.appops.scope.requester", UID + 2, (byte)7, 22);
        requested(normalLegacy, 22, Manifest.permission.READ_CONTACTS);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, normalLegacy);
        ApkPackage modernPeer = pkg("fixture.appops.scope.modern", UID + 2, (byte)7);
        requested(modernPeer, 36); call(h.pm, "add", new Class<?>[]{ApkPackage.class}, modernPeer);
        check(h.pm.permissions().checkPermission(modernPeer.packageName, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_READ_CONTACTS, normalLegacy.uid(), modernPeer.packageName, null) == MODE_ALLOWED,
                "An actual legacy requester supplies its ordinary legacy grant to the shared UID");
        check(h.pm.permissions().getPermissionFlags(modernPeer.packageName, Manifest.permission.READ_CONTACTS, "default:0", 0)
                == PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED,
                "A new ordinary legacy grant receives the platform review flag despite a modern nonrequester");
        h.ops.setUidMode(OP_READ_CONTACTS, normalLegacy.uid(), MODE_IGNORED);
        check(h.pm.permissions().checkPermission(modernPeer.packageName, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED
                && h.ops.checkOperationRaw(OP_READ_CONTACTS, normalLegacy.uid(), modernPeer.packageName, null) == MODE_IGNORED,
                "The legitimate legacy grant retains compatibility revocation semantics");
        h.ops.setUidMode(OP_READ_CONTACTS, normalLegacy.uid(), MODE_ALLOWED);
        check(h.ops.checkOperationRaw(OP_READ_CONTACTS, normalLegacy.uid(), modernPeer.packageName, null) == MODE_ALLOWED,
                "Restoring legacy compatibility does not require a modern runtime grant");

        h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.READ_CONTACTS, "default:0", 0);
        drain(h); drain(h);
        Watcher watcher = new Watcher(); h.service.startWatchingMode(OP_READ_CONTACTS, NAME, watcher);
        Token operation = new Token(); h.ops.startOperation(operation, OP_READ_CONTACTS, UID, NAME, null,
                false, false, null, false, 0, -1);
        try {
            call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, legacy);
            call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, legacy.packageName);
            h.pm.permissions().forgetPackage(legacy.packageName);
            call(h.pm, "remove", new Class<?>[]{String.class}, legacy.packageName);
            call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, legacy);
            drain(h); drain(h); awaitCount(watcher.calls, 0);
            check(h.pm.permissions().checkPermission(NAME, Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED
                    && h.ops.isOperationActive(OP_READ_CONTACTS, UID, NAME),
                    "Retiring a nonrequesting legacy member preserves the modern grant, operation and watcher state");
            Object restored = restoredPermissions(h);
            check((Integer)call(restored, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_READ_CONTACTS, UID) == MODE_ALLOWED
                    && (Integer)call(restored, "runtimeOpMode", new Class<?>[]{int.class, int.class}, OP_ANSWER_PHONE_CALLS, UID) == MODE_IGNORED,
                    "Durable UID policy preserves explicit grants and runtime-only denial across policy restart");
        } finally { h.service.stopWatchingMode(watcher); operation.die(); }
        check(watcher.token.recipients.isEmpty() && operation.recipients.isEmpty(),
                "Eligibility and nonrequester retirement checks release their real observer and operation registrations");
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
        permissionCheckerEdges();
        foregroundServiceExit();
        return checks;
    }
    public static void main(String[] args) throws Exception {
        int total = run(true);
        System.out.println("AppOpsContractTest all passed: " + total + " checks; binder=true");
    }
}
