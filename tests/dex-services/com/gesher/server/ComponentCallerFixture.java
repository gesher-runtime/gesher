/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.ActivityClient;
import android.app.ActivityOptions;
import android.app.ComponentCaller;
import android.app.IActivityClientController;
import android.app.IApplicationThread;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import sun.misc.Unsafe;

/** Real caller/controller/provider policy with constructed parsed manifests and caller identities. */
public final class ComponentCallerFixture {
    private static final String OWNER = "fixture.caller.owner", READER = "fixture.caller.reader";
    private static final String BOTH = "fixture.caller.both", CLIENT = "fixture.caller.client";
    private static final String COLLISION = "fixture.caller.collision";
    private static final String READ_PERMISSION = "fixture.caller.READ", WRITE_PERMISSION = "fixture.caller.WRITE";
    private static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    private static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private static final int PREFIX = Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
    private static final int PERSIST = Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;
    private static final Uri ITEM = Uri.parse("content://fixture.caller.protected/item?version=1#one");
    private static final Uri PRIVATE = Uri.parse("content://fixture.caller.private/tree/item");
    private static final Uri ABSENT = Uri.parse("content://fixture.caller.protected/absent");
    private static final Unsafe UNSAFE;
    private static boolean binder;
    private static int checks;
    static {
        try { Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true); UNSAFE = (Unsafe)field.get(null); }
        catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Operation { void run() throws Exception; }
    private static void check(boolean okay, String reason) {
        if (!okay) throw new AssertionError(reason);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Operation operation, String reason) throws Exception {
        try { operation.run(); }
        catch (Throwable actual) {
            if (!type.isInstance(actual)) throw new AssertionError(reason + ": " + actual, actual);
            checks++; return;
        }
        throw new AssertionError(reason + ": no exception");
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }
    private static Class<?> type(String name) throws ClassNotFoundException {
        return Class.forName("com.gesher.server." + name);
    }
    private static Object construct(String name, Class<?>[] parameters, Object... arguments) throws Exception {
        var constructor = type(name).getDeclaredConstructor(parameters);
        constructor.setAccessible(true);
        try { return constructor.newInstance(arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object invoke(Class<?> owner, Object target, String name,
            Class<?>[] parameters, Object... arguments) throws Exception {
        Method method = owner.getDeclaredMethod(name, parameters); method.setAccessible(true);
        try { return method.invoke(target, arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error error) throw error;
        if (failure instanceof Exception exception) throw exception;
        throw new AssertionError(failure);
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    private static Object state(String name) throws Exception {
        return Enum.valueOf((Class)type("ActivityTaskManagerService$State"), name);
    }
    private static IBinder activityToken(Object activity) throws Exception { return (IBinder)get(activity, "token"); }
    private static IBinder token() {
        if (binder) return new Binder();
        return (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class},
                (self, method, args) -> switch (method.getName()) {
                    case "equals" -> self == args[0];
                    case "hashCode" -> System.identityHashCode(self);
                    case "toString" -> "Host caller token";
                    default -> throw new AssertionError("Unexpected host Binder transport: " + method.getName());
                });
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder token = token();
        @Override public IBinder asBinder() { return token; }
    }
    private static ApkPackage code(String name, int uid, String... uses) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String permission : uses) parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0,
                java.util.Collections.emptySet()));
        if (OWNER.equals(name)) {
            for (String permissionName : new String[]{READ_PERMISSION, WRITE_PERMISSION}) {
                ParsedPermissionImpl permission = new ParsedPermissionImpl();
                permission.setName(permissionName); permission.setPackageName(OWNER);
                permission.setProtectionLevel(PermissionInfo.PROTECTION_NORMAL); parsed.addPermission(permission);
            }
            for (String authority : new String[]{"protected", "private"}) {
                ParsedProviderImpl provider = new ParsedProviderImpl();
                provider.setName(OWNER + ".Provider" + authority); provider.setPackageName(OWNER);
                provider.setEnabled(true); provider.setExported(authority.equals("protected"));
                provider.setAuthority("fixture.caller." + authority); provider.setGrantUriPermissions(true);
                provider.setReadPermission(READ_PERMISSION); provider.setWritePermission(WRITE_PERMISSION);
                parsed.addProvider(provider);
            }
        }
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", name); set(code, "parsed", parsed); set(code, "mUid", uid);
        set(code, "apkPath", "/fixture/" + name + "/base.apk");
        set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", field(SharedLibraryGraph.class, "EMPTY").get(null));
        return code;
    }

    public static int run(boolean realBinder) throws Exception {
        binder = realBinder; checks = 0;
        Harness h = new Harness();
        try {
            Uri stream = Uri.parse("content://fixture.caller.protected/stream");
            Uri clipUri = Uri.parse("content://fixture.caller.protected/clip");
            Uri nestedUri = Uri.parse("content://fixture.caller.protected/nested");
            Intent delivered = new Intent().setData(ITEM);
            delivered.putParcelableArrayListExtra(Intent.EXTRA_STREAM, new ArrayList<>(List.of(stream, ITEM)));
            ClipData clip = ClipData.newRawUri("URI", clipUri);
            clip.addItem(new ClipData.Item(new Intent().setData(nestedUri)));
            clip.addItem(new ClipData.Item(PRIVATE)); delivered.setClipData(clip);
            Object activity = h.record(CLIENT, h.snapshot(READER, delivered, false));
            ComponentCaller caller = h.caller(activity);
            check(h.permission(OWNER, caller, ITEM, READ) == PackageManager.PERMISSION_GRANTED, "Sender read permission lost");
            check(h.permission(OWNER, caller, ITEM, WRITE) == PackageManager.PERMISSION_DENIED, "Sender write permission invented");
            check(h.permission(OWNER, caller, ITEM, READ | WRITE) == PackageManager.PERMISSION_DENIED, "Combined request ignored write");
            ComponentCaller both = h.caller(h.record(CLIENT, h.snapshot(BOTH, new Intent().setData(ITEM), false)));
            check(h.permission(OWNER, both, ITEM, WRITE) == PackageManager.PERMISSION_GRANTED, "Real sender write permission lost");
            check(h.permission(OWNER, both, ITEM, READ | WRITE) == PackageManager.PERMISSION_GRANTED,
                    "Real combined sender permissions lost");
            ComponentCaller singleStream = h.caller(h.record(CLIENT,
                    h.snapshot(READER, new Intent().putExtra(Intent.EXTRA_STREAM, stream), false)));
            check(h.permission(OWNER, singleStream, stream, READ) == PackageManager.PERMISSION_GRANTED,
                    "Single-URI EXTRA_STREAM was not captured");
            check(h.permission(OWNER, caller, PRIVATE, READ) == PackageManager.PERMISSION_DENIED, "Delivered inaccessible URI was not retained");
            for (Uri uri : List.of(stream, clipUri, nestedUri)) {
                check(h.permission(OWNER, caller, uri, READ) == PackageManager.PERMISSION_GRANTED, "URI delivery location missing: " + uri);
            }
            h.as(OWNER, () -> expect(IllegalArgumentException.class, () -> caller.checkContentUriPermission(ABSENT, READ), "Absent URI"));
            for (Uri neighbor : List.of(Uri.parse(ITEM.toString().replace("version=1", "version=2")),
                    Uri.parse(ITEM.toString().replace("#one", "#two")))) {
                h.as(OWNER, () -> expect(IllegalArgumentException.class,
                        () -> caller.checkContentUriPermission(neighbor, READ), "Exact URI query/fragment identity"));
            }
            check(h.permission(OWNER, caller, Uri.parse(ITEM.toString().replace("content://", "content://0@")), READ)
                    == PackageManager.PERMISSION_GRANTED, "Embedded user zero did not normalize");
            delivered.setData(ABSENT); delivered.setClipData(null); delivered.removeExtra(Intent.EXTRA_STREAM);
            check(h.permission(OWNER, caller, ITEM, READ) == PackageManager.PERMISSION_GRANTED, "Intent mutation changed history");
            h.as(OWNER, () -> expect(IllegalArgumentException.class, () -> caller.checkContentUriPermission(ABSENT, READ), "Mutated Intent enlarged snapshot"));

            ComponentCaller unknownActivity = new ComponentCaller(token(), token());
            ComponentCaller unknownCaller = new ComponentCaller(activityToken(activity), token());
            h.as(CLIENT, () -> {
                expect(SecurityException.class, () -> caller.checkContentUriPermission(ITEM, READ), "Querier current permission required");
                expect(SecurityException.class, () -> unknownActivity.checkContentUriPermission(ITEM, READ), "Permission check before activity lookup");
                expect(IllegalArgumentException.class, () -> unknownActivity.checkContentUriPermission(ITEM, 0), "No-access flags before identity lookup");
                expect(IllegalArgumentException.class, () -> caller.checkContentUriPermission(Uri.parse("file:///tmp/file"), READ), "Non-content URI validation");
            });
            h.as(OWNER, () -> {
                expect(IllegalArgumentException.class, () -> unknownCaller.checkContentUriPermission(ITEM, PREFIX), "No-access advanced flags");
                expect(SecurityException.class, () -> caller.checkContentUriPermission(Uri.parse("content://10@fixture.caller.protected/item"), READ), "Unsupported user not redirected");
            });
            check(h.permission(OWNER, unknownActivity, ITEM, READ) == PackageManager.PERMISSION_DENIED, "Unknown activity granted");
            check(h.permission(OWNER, unknownCaller, ITEM, READ) == PackageManager.PERMISSION_DENIED, "Unknown caller granted");
            check(h.permission(OWNER, new ComponentCaller(activityToken(activity), null), ITEM, READ) == PackageManager.PERMISSION_DENIED, "Null caller reused initial identity");
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity((-1L << 32) | (int)get(h.processes.get(CLIENT), "pid"));
                expect(IllegalArgumentException.class, () -> unknownActivity.checkContentUriPermission(ITEM, READ),
                        "Negative querying UID before activity lookup");
                expect(IllegalArgumentException.class, () -> caller.checkContentUriPermission(ITEM, READ),
                        "Negative querying UID before current access");
            } finally { Binder.restoreCallingIdentity(identity); }
            check(h.permission(OWNER, caller, ITEM, READ | Intent.FLAG_ACTIVITY_NEW_TASK) == PackageManager.PERMISSION_GRANTED,
                    "Unrelated mode bit rejected unlike Android full check");
            check(h.permission(OWNER, caller, ITEM, READ | PERSIST) == PackageManager.PERMISSION_GRANTED,
                    "Persistable-demand flag rejected real direct provider access");
            h.as(OWNER, () -> expect(IllegalArgumentException.class,
                    () -> caller.checkContentUriPermission(ITEM, READ | PREFIX), "Prefix query reused nonprefix snapshot key"));
            h.grant(CLIENT, PRIVATE, READ);
            ComponentCaller privateOwner = h.caller(h.record(CLIENT, h.snapshot(OWNER, new Intent().setData(PRIVATE), false)));
            check(h.permission(CLIENT, privateOwner, PRIVATE, READ) == PackageManager.PERMISSION_GRANTED, "Real exact grant did not authorize query");
            h.as(CLIENT, () -> {
                expect(SecurityException.class, () -> privateOwner.checkContentUriPermission(PRIVATE, READ | PREFIX), "Exact grant promoted to prefix query");
                expect(SecurityException.class, () -> privateOwner.checkContentUriPermission(PRIVATE, READ | PERSIST), "Nonpersistable grant met persistable demand");
            });
            h.grant(CLIENT, Uri.parse("content://fixture.caller.private/tree"), READ | PREFIX);
            h.as(CLIENT, () -> expect(IllegalArgumentException.class,
                    () -> privateOwner.checkContentUriPermission(PRIVATE, READ | PREFIX), "Current prefix access lost snapshot identity distinction"));
            h.revoke(CLIENT, PRIVATE, READ);
            h.revoke(CLIENT, Uri.parse("content://fixture.caller.private/tree"), READ);
            h.as(CLIENT, () -> expect(SecurityException.class,
                    () -> privateOwner.checkContentUriPermission(PRIVATE, READ), "Revoked recipient retained current access"));

            ComponentCaller before = h.caller(h.record(CLIENT, h.snapshot(READER, new Intent().setData(PRIVATE), false)));
            h.grant(READER, PRIVATE, READ);
            ComponentCaller after = h.caller(h.record(CLIENT, h.snapshot(READER, new Intent().setData(PRIVATE), false)));
            check(h.permission(OWNER, before, PRIVATE, READ) == PackageManager.PERMISSION_DENIED, "Later grant upgraded historical denial");
            check(h.permission(OWNER, after, PRIVATE, READ) == PackageManager.PERMISSION_GRANTED, "Grant absent from later snapshot");
            h.revoke(READER, PRIVATE, READ);
            check(h.permission(OWNER, after, PRIVATE, READ) == PackageManager.PERMISSION_GRANTED, "Later revoke rewrote historical permission");
            h.grant(READER, PRIVATE, WRITE);
            check(h.permission(OWNER, after, PRIVATE, WRITE) == PackageManager.PERMISSION_DENIED, "Later write grant upgraded old read-only snapshot");
            check(!before.equals(after), "Different delivery identities collapsed");

            Object serverSnapshot;
            synchronized (h.pm) {
                serverSnapshot = h.capture(null, android.os.Process.SYSTEM_UID, "android", true,
                        false, new Intent().setData(ITEM));
            }
            ComponentCaller server = h.caller(h.record(CLIENT, serverSnapshot));
            check(h.permission(OWNER, server, ITEM, READ | WRITE) == PackageManager.PERMISSION_GRANTED,
                    "Actual server authority became an absent installed package");
            h.as(CLIENT, () -> expect(SecurityException.class, () -> server.checkContentUriPermission(ITEM, READ),
                    "Server history bypassed recipient current access"));
            ComponentCaller noData = h.caller(h.record(CLIENT, h.snapshot(OWNER, null, true)));
            h.as(OWNER, () -> {
                check(OWNER.equals(noData.getPackage()), "Null result data lost its real sender identity");
                expect(IllegalArgumentException.class, () -> noData.checkContentUriPermission(ITEM, READ),
                        "Null data invented a delivered URI");
            });

            h.as(READER, () -> {
                check(caller.getUid() == h.packages.get(READER).uid(), "Same authenticated package UID hidden");
                check(READER.equals(caller.getPackage()), "Same authenticated package name hidden");
            });
            for (String other : List.of(OWNER, CLIENT, COLLISION)) h.as(other, () -> {
                check(caller.getUid() == android.os.Process.INVALID_UID && caller.getPackage() == null,
                        "Hidden identity leaked to another package, including a numeric UID collision");
            });
            ComponentCaller shared = h.caller(h.record(CLIENT, h.snapshot(READER, new Intent().setData(ITEM), true)));
            h.as(CLIENT, () -> check(shared.getUid() == h.packages.get(READER).uid() && READER.equals(shared.getPackage()),
                    "Explicit shared identity unavailable"));
            int oldUid = (int)get(h.processes.get(READER), "uid");
            ApkPackage replacement = code(READER, 12402, READ_PERMISSION);
            Map<String, ApkPackage> replaced = new LinkedHashMap<>(h.packages); replaced.put(READER, replacement);
            set(h.pm, "mPackages", replaced); set(h.processes.get(READER), "uid", replacement.uid());
            h.as(READER, () -> check(caller.getUid() == android.os.Process.INVALID_UID && caller.getPackage() == null,
                    "Same-name replacement with different UID inherited hidden identity"));
            set(h.pm, "mPackages", new LinkedHashMap<>(h.packages)); set(h.processes.get(READER), "uid", oldUid);
            Map<String, ApkPackage> recipientReplaced = new LinkedHashMap<>(h.packages);
            recipientReplaced.put(CLIENT, code(CLIENT, 12504));
            set(h.pm, "mPackages", recipientReplaced);
            check(h.permission(OWNER, shared, ITEM, READ) == PackageManager.PERMISSION_DENIED,
                    "Retired recipient generation retained its historical activity capability");
            h.as(OWNER, () -> check(shared.getUid() == android.os.Process.INVALID_UID && shared.getPackage() == null,
                    "Retired recipient generation retained a shared identity"));
            set(h.pm, "mPackages", new LinkedHashMap<>(h.packages));
            check(h.permission(OWNER, shared, ITEM, READ) == PackageManager.PERMISSION_GRANTED,
                    "Original recipient generation control lost its snapshot");
            Object another = h.record(CLIENT, h.snapshot(OWNER, new Intent().setData(ITEM), true));
            check(h.permission(OWNER, new ComponentCaller(activityToken(another), (IBinder)get(activity, "initialCallerInfoAccessToken")), ITEM, READ)
                    == PackageManager.PERMISSION_DENIED, "Caller token escaped its activity");
            h.activities.remove(activity); set(activity, "state", state("DESTROYED"));
            check(h.permission(OWNER, caller, ITEM, READ) == PackageManager.PERMISSION_DENIED, "Destroyed activity retained usable token");
            h.as(READER, () -> check(caller.getUid() == android.os.Process.INVALID_UID, "Destroyed activity retained identity"));
            check(!(boolean)invoke(ActivityTaskManagerService.class, h.atm, "finish",
                    new Class<?>[]{type("ActivityTaskManagerService$ActivityRecord"), String.class, int.class, Intent.class},
                    activity, "retired-source-test", 0, null), "Retired source published a new finish result");

            ActivityOptions creation = ActivityOptions.makeBasic().setShareIdentityEnabled(true);
            for (Bundle sender : List.of(Bundle.EMPTY, ActivityOptions.makeBasic().toBundle(), falseIdentityBundle())) {
                Bundle merged = creation.toBundle();
                merged.putAll(ActivityOptions.fromBundle(sender).toBundle());
                check(ActivityOptions.fromBundle(merged).isShareIdentityEnabled(),
                        "Pinned normalized PendingIntent false/absent merge changed");
            }
            Bundle merged = ActivityOptions.makeBasic().toBundle(); merged.putAll(creation.toBundle());
            check(ActivityOptions.fromBundle(merged).isShareIdentityEnabled(), "Sender shareIdentity true lost in normalized merge");
            retirementContracts();
            bootstrapContracts();
            return checks;
        } finally { ActivityClient.setActivityClientController(null); }
    }

    private static Bundle falseIdentityBundle() {
        Bundle result = new Bundle(); result.putBoolean("android:activity.shareIdentity", false); return result;
    }

    /** Run in a fresh VM with the actual nonprotected environment configuration. */
    public static int runBootstrap(boolean realBinder) throws Exception {
        binder = realBinder; checks = 0;
        try { bootstrapContracts(); return checks; }
        finally { ActivityClient.setActivityClientController(null); }
    }

    private static void retirementContracts() throws Exception {
        for (String retirement : List.of("record", "process", "package")) {
            for (String operation : List.of("finish", "task", "navigate")) {
                Harness h = new Harness();
                Object recipient = h.record(OWNER, h.snapshot(READER, new Intent().setData(ITEM), false));
                Object source = h.record(CLIENT, h.snapshot(READER, new Intent().setData(ITEM), false));
                set(source, "resultTo", recipient);
                // A scheduling boundary only: retire after forToken/the first
                // liveness check, immediately before real CallerIdentity runs.
                // No policy method or authentication result is substituted.
                set(h.atm, "mBeforeActivityOwnerAuthentication", (Runnable)() -> {
                    try { h.retire(source, retirement); }
                    catch (ReflectiveOperationException failure) { throw new AssertionError(failure); }
                });
                h.as(CLIENT, () -> {
                    if (operation.equals("navigate")) {
                        check(!h.controller.navigateUpTo(activityToken(source), new Intent().setClassName(OWNER,
                                OWNER + ".Activity"), null, -1, new Intent().setData(ITEM)),
                                retirement + " retirement must decline navigation");
                    } else {
                        check(h.controller.finishActivity(activityToken(source), -1, new Intent().setData(ITEM),
                                operation.equals("task") ? 2 : 0), retirement + " retirement must already be finished");
                    }
                });
                check(((List<?>)get(recipient, "pendingResults")).isEmpty(), "Retirement published result data");
                check(!(boolean)get(source, "finishing"), "Retirement started a fresh finish mutation");
            }
        }
        Harness h = new Harness();
        Object source = h.record(CLIENT, h.snapshot(READER, new Intent().setData(ITEM), false));
        h.as(OWNER, () -> {
            expect(SecurityException.class, () -> h.controller.finishActivity(activityToken(source), 0, null, 0),
                    "Live wrong-owner finish must remain unauthorized");
            expect(SecurityException.class, () -> h.controller.navigateUpTo(activityToken(source),
                    new Intent().setClassName(OWNER, OWNER + ".Activity"), null, 0, null),
                    "Live wrong-owner navigation must remain unauthorized");
        });
        check(!(boolean)get(source, "finishing"), "Wrong owner mutated the live activity");
        set(source, "finishing", true);
        h.as(CLIENT, () -> check(h.controller.finishActivity(activityToken(source), 0, null, 0),
                "Live owner could not finish an already-finishing activity"));
        set(source, "finishing", false);
        h.retire(source, "package");
        check(!(boolean)invoke(ActivityTaskManagerService.class, h.atm, "finishFromApp",
                new Class<?>[]{type("ActivityTaskManagerService$ActivityRecord"), int.class, Intent.class},
                source, 0, null), "Package retirement after authorization reached finish mutation");
        check(!(boolean)invoke(ActivityTaskManagerService.class, h.atm, "navigateUpTo",
                new Class<?>[]{type("ActivityTaskManagerService$ActivityRecord"), Intent.class, String.class, int.class, Intent.class},
                source, new Intent().setClassName(OWNER, OWNER + ".Activity"), null, 0, null),
                "Package retirement after authorization reached navigation mutation");
        check(!(boolean)get(source, "finishing"), "Late package retirement mutated the source");
    }

    private static void bootstrapContracts() throws Exception {
        Harness h = new Harness();
        Object app = UNSAFE.allocateInstance(type("ActivityTaskManagerService$ProcessRecord"));
        IApplicationThread thread = new AppThread();
        int pid = 873001, uid = h.packages.get(CLIENT).uid();
        set(app, "pid", pid); set(app, "uid", uid); set(app, "thread", thread);
        @SuppressWarnings("unchecked") List<Object> attached = (List<Object>)get(h.atm, "mProcesses");
        attached.add(app);
        check(h.bootstrap(app, thread, pid, uid), "Exact live unassigned process not eligible for legacy enrollment");
        check(!h.bootstrap(null, thread, pid, uid), "Missing process admitted");
        check(!h.bootstrap(app, null, pid, uid), "Missing application thread admitted");
        check(!h.bootstrap(app, thread, pid + 1, uid), "Wrong bootstrap PID admitted");
        check(!h.bootstrap(app, thread, pid, uid + 1), "Wrong bootstrap UID admitted");
        check(!h.bootstrap(app, new AppThread(), pid, uid), "Different bootstrap Binder thread admitted");
        attached.remove(app);
        check(!h.bootstrap(app, thread, pid, uid), "Retired bootstrap process admitted");
        attached.add(app);
        for (String name : List.of("packageName", "processName")) {
            set(app, name, CLIENT);
            check(!h.bootstrap(app, thread, pid, uid), "Assigned " + name + " admitted for enrollment");
            set(app, name, null);
        }
        for (String name : List.of("exiting", "spawned", "ready")) {
            set(app, name, true);
            check(!h.bootstrap(app, thread, pid, uid), name + " process admitted for enrollment");
            set(app, name, false);
        }
        set(app, "startSeq", 1L);
        check(!h.bootstrap(app, thread, pid, uid), "Existing start generation admitted for enrollment");
        set(app, "startSeq", 0L);
        set(app, "thread", null);
        check(!h.bootstrap(app, thread, pid, uid), "Process without retained thread admitted");
        set(app, "thread", thread);
        Class<?> starterType = type("ActivityTaskManagerService$ProcessStarter");
        set(h.atm, "mStarter", Proxy.newProxyInstance(starterType.getClassLoader(), new Class<?>[]{starterType},
                (self, method, arguments) -> { throw new AssertionError("Bootstrap predicate must not launch a process"); }));
        boolean protectedMode = GmsSignatureSpoofing.isEnabled();
        check(h.bootstrap(app, thread, pid, uid) == !protectedMode,
                protectedMode ? "Protected spawner-owned device accepted legacy enrollment"
                        : "Nonprotected self-attachment rejected merely because a starter exists");
        set(h.atm, "mStarter", null);
        check(h.bootstrap(app, thread, pid, uid), "Restored exact unassigned control rejected");
    }

    private static final class Harness {
        final Map<String, ApkPackage> packages = new LinkedHashMap<>();
        final Map<String, Object> processes = new LinkedHashMap<>();
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final ActivityManagerService am;
        final Object grants;
        final IActivityClientController controller;
        final List<Object> activities = new ArrayList<>();

        Harness() throws Exception {
            packages.put(OWNER, code(OWNER, 12301));
            packages.put(READER, code(READER, 12302, READ_PERMISSION));
            packages.put(BOTH, code(BOTH, 12303, READ_PERMISSION, WRITE_PERMISSION));
            packages.put(CLIENT, code(CLIENT, 12304));
            packages.put(COLLISION, code(COLLISION, 12302));
            // Omit unrelated constructors/native registration. The parsed provider
            // projection, grants, permissions and controller stay real on both runtimes.
            pm = (PackageManagerService)UNSAFE.allocateInstance(PackageManagerService.class);
            set(pm, "mPackages", new LinkedHashMap<>(packages)); set(pm, "mRemoving", new LinkedHashMap<>());
            set(pm, "mPackageRemoval", new ArrayList<>());
            set(pm, "mPermissions", construct("PermissionPolicy", new Class<?>[]{PackageManagerService.class}, pm));
            atm = (ActivityTaskManagerService)UNSAFE.allocateInstance(ActivityTaskManagerService.class);
            set(atm, "mPm", pm); set(atm, "mActivities", activities);
            List<Object> list = new ArrayList<>(); set(atm, "mProcesses", list);
            int pid = 872001;
            for (ApkPackage code : packages.values()) {
                Object process = UNSAFE.allocateInstance(type("ActivityTaskManagerService$ProcessRecord"));
                set(process, "pid", pid++); set(process, "uid", code.uid());
                set(process, "thread", new AppThread()); set(process, "activities", new ArrayList<>());
                set(process, "packageName", code.packageName); set(process, "processName", code.packageName);
                set(process, "ready", true);
                processes.put(code.packageName, process); list.add(process);
            }
            grants = construct("UriGrants", new Class<?>[]{PackageManagerService.class, ActivityTaskManagerService.class}, pm, atm);
            Object providers = UNSAFE.allocateInstance(type("ContentProviders"));
            set(providers, "mPm", pm); set(providers, "mUriGrants", grants);
            am = (ActivityManagerService)UNSAFE.allocateInstance(ActivityManagerService.class);
            set(am, "mPm", pm); set(am, "mAtm", atm); set(am, "mProviders", providers);
            set(am, "mCallers", new CallerIdentity(pm, atm));
            invoke(ActivityTaskManagerService.class, atm, "setActivityManager", new Class<?>[]{ActivityManagerService.class}, am);
            ActivityClientController implementation = (ActivityClientController)construct("ActivityClientController",
                    new Class<?>[]{ActivityTaskManagerService.class}, atm);
            if (binder) {
                StandIn.setStrict(true);
                IBinder target = StandIn.of(IActivityClientController.class, implementation).asBinder();
                controller = IActivityClientController.Stub.asInterface(new Binder() {
                    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                            throws RemoteException { return target.transact(code, data, reply, flags); }
                });
            } else controller = implementation;
            ActivityClient.setActivityClientController(controller);
        }
        void as(String name, Operation action) throws Exception {
            Object process = processes.get(name);
            int pid = (int)get(process, "pid"), uid = (int)get(process, "uid");
            long old = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)uid << 32) | pid);
                check(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid,
                        "Controlled caller identity was not installed");
                action.run();
            } finally { Binder.restoreCallingIdentity(old); }
        }
        Object snapshot(String source, Intent intent, boolean share) throws Exception {
            ApkPackage code = packages.get(source);
            synchronized (pm) {
                return capture(code, code.uid(), source, false, share, intent);
            }
        }
        Object capture(ApkPackage code, int uid, String source, boolean system, boolean share, Intent intent) throws Exception {
            Object origin = construct("ActivityCallerState$Origin", new Class<?>[]{ApkPackage.class,
                    int.class, String.class, boolean.class, boolean.class}, code, uid, source, system, share);
            return invoke(type("ActivityCallerState"), null, "capture", new Class<?>[]{type("ActivityCallerState$Origin"),
                    Intent.class, type("UriGrants")}, origin, intent, grants);
        }
        Object record(String owner, Object snapshot) throws Exception {
            Object record = UNSAFE.allocateInstance(type("ActivityTaskManagerService$ActivityRecord"));
            set(record, "token", token());
            set(record, "initialCallerInfoAccessToken", token());
            Object callerState = construct("ActivityCallerState", new Class<?>[0]);
            set(record, "callerState", callerState);
            set(record, "targetCode", packages.get(owner));
            ActivityInfo info = new ActivityInfo(); info.packageName = owner; info.name = owner + ".Activity";
            info.applicationInfo = packages.get(owner).applicationInfo(0);
            set(record, "info", info); set(record, "pendingResults", new ArrayList<>());
            Object app = processes.get(owner);
            set(record, "app", app); set(record, "state", state("RESUMED"));
            invoke(type("ActivityCallerState"), callerState, "add",
                    new Class<?>[]{IBinder.class, type("ActivityCallerState$Snapshot")},
                    get(record, "initialCallerInfoAccessToken"), snapshot);
            ((List<Object>)get(app, "activities")).add(record); activities.add(record);
            return record;
        }
        ComponentCaller caller(Object record) throws Exception {
            return new ComponentCaller(activityToken(record), (IBinder)get(record, "initialCallerInfoAccessToken"));
        }
        int permission(String query, ComponentCaller caller, Uri uri, int modes) throws Exception {
            int[] answer = {-999}; as(query, () -> answer[0] = caller.checkContentUriPermission(uri, modes)); return answer[0];
        }
        void grant(String target, Uri uri, int modes) throws Exception {
            as(OWNER, () -> am.grantUriPermission((IApplicationThread)get(processes.get(OWNER), "thread"), target, uri, modes, 0));
        }
        void revoke(String target, Uri uri, int modes) throws Exception {
            as(OWNER, () -> am.revokeUriPermission((IApplicationThread)get(processes.get(OWNER), "thread"), target, uri, modes, 0));
        }
        boolean bootstrap(Object app, IApplicationThread thread, int pid, int uid) throws Exception {
            return (boolean)invoke(ActivityTaskManagerService.class, atm, "legacyBootstrapProcess",
                    new Class<?>[]{type("ActivityTaskManagerService$ProcessRecord"), IApplicationThread.class,
                            int.class, int.class}, app, thread, pid, uid);
        }
        @SuppressWarnings("unchecked")
        void retire(Object record, String kind) throws ReflectiveOperationException {
            try {
                if (kind.equals("package")) {
                    Map<String, ApkPackage> replacement = new LinkedHashMap<>(packages);
                    replacement.put(CLIENT, code(CLIENT, 12604));
                    set(pm, "mPackages", replacement);
                    return;
                }
                synchronized (atm) {
                    Object process = get(record, "app");
                    if (kind.equals("process")) {
                        ((List<Object>)get(atm, "mProcesses")).remove(process);
                        for (Object activity : new ArrayList<>((List<Object>)get(process, "activities"))) {
                            activities.remove(activity); set(activity, "state", state("DESTROYED"));
                        }
                    } else { activities.remove(record); set(record, "state", state("DESTROYED")); }
                }
            } catch (ReflectiveOperationException failure) { throw failure; }
            catch (Exception failure) { throw new AssertionError("Cannot arrange retirement boundary", failure); }
        }
    }
}
