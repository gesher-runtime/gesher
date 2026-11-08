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
        set(code, "mDataDir", "/fixture/" + name + "/data"); set(code, "sharedLibraries", SharedLibraryGraph.EMPTY);
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
            ActivityTaskManagerService.ActivityRecord activity = h.record(CLIENT, h.snapshot(READER, delivered, false));
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
            ComponentCaller unknownCaller = new ComponentCaller(activity.token, token());
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
            check(h.permission(OWNER, new ComponentCaller(activity.token, null), ITEM, READ) == PackageManager.PERMISSION_DENIED, "Null caller reused initial identity");
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity((-1L << 32) | h.processes.get(CLIENT).pid);
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

            ActivityCallerState.Snapshot serverSnapshot;
            synchronized (h.pm) {
                serverSnapshot = ActivityCallerState.capture(new ActivityCallerState.Origin(null,
                        android.os.Process.SYSTEM_UID, "android", true, false), new Intent().setData(ITEM), h.grants);
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
            int oldUid = h.processes.get(READER).uid;
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
            ActivityTaskManagerService.ActivityRecord another = h.record(CLIENT, h.snapshot(OWNER, new Intent().setData(ITEM), true));
            check(h.permission(OWNER, new ComponentCaller(another.token, activity.initialCallerInfoAccessToken), ITEM, READ)
                    == PackageManager.PERMISSION_DENIED, "Caller token escaped its activity");
            h.activities.remove(activity); activity.state = ActivityTaskManagerService.State.DESTROYED;
            check(h.permission(OWNER, caller, ITEM, READ) == PackageManager.PERMISSION_DENIED, "Destroyed activity retained usable token");
            h.as(READER, () -> check(caller.getUid() == android.os.Process.INVALID_UID, "Destroyed activity retained identity"));
            check(!h.atm.finish(activity, "retired-source-test", 0, null), "Retired source published a new finish result");

            ActivityOptions creation = ActivityOptions.makeBasic().setShareIdentityEnabled(true);
            for (Bundle sender : List.of(Bundle.EMPTY, ActivityOptions.makeBasic().toBundle(), falseIdentityBundle())) {
                Bundle merged = creation.toBundle();
                merged.putAll(ActivityOptions.fromBundle(sender).toBundle());
                check(ActivityOptions.fromBundle(merged).isShareIdentityEnabled(),
                        "Pinned normalized PendingIntent false/absent merge changed");
            }
            Bundle merged = ActivityOptions.makeBasic().toBundle(); merged.putAll(creation.toBundle());
            check(ActivityOptions.fromBundle(merged).isShareIdentityEnabled(), "Sender shareIdentity true lost in normalized merge");
            return checks;
        } finally { ActivityClient.setActivityClientController(null); }
    }

    private static Bundle falseIdentityBundle() {
        Bundle result = new Bundle(); result.putBoolean("android:activity.shareIdentity", false); return result;
    }

    private static final class Harness {
        final Map<String, ApkPackage> packages = new LinkedHashMap<>();
        final Map<String, ActivityTaskManagerService.ProcessRecord> processes = new LinkedHashMap<>();
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final ActivityManagerService am;
        final UriGrants grants;
        final IActivityClientController controller;
        final List<ActivityTaskManagerService.ActivityRecord> activities = new ArrayList<>();

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
            set(pm, "mPackageRemoval", new ArrayList<>()); set(pm, "mPermissions", new PermissionPolicy(pm));
            atm = (ActivityTaskManagerService)UNSAFE.allocateInstance(ActivityTaskManagerService.class);
            set(atm, "mPm", pm); set(atm, "mActivities", activities);
            List<ActivityTaskManagerService.ProcessRecord> list = new ArrayList<>(); set(atm, "mProcesses", list);
            int pid = 872001;
            for (ApkPackage code : packages.values()) {
                ActivityTaskManagerService.ProcessRecord process =
                        (ActivityTaskManagerService.ProcessRecord)UNSAFE.allocateInstance(ActivityTaskManagerService.ProcessRecord.class);
                set(process, "pid", pid++); set(process, "uid", code.uid());
                set(process, "thread", new AppThread()); set(process, "activities", new ArrayList<>());
                process.packageName = code.packageName; process.processName = code.packageName; process.ready = true;
                processes.put(code.packageName, process); list.add(process);
            }
            grants = new UriGrants(pm, atm);
            ContentProviders providers = (ContentProviders)UNSAFE.allocateInstance(ContentProviders.class);
            set(providers, "mPm", pm); set(providers, "mUriGrants", grants);
            am = (ActivityManagerService)UNSAFE.allocateInstance(ActivityManagerService.class);
            set(am, "mPm", pm); set(am, "mAtm", atm); set(am, "mProviders", providers);
            set(am, "mCallers", new CallerIdentity(pm, atm)); atm.setActivityManager(am);
            ActivityClientController implementation = new ActivityClientController(atm);
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
            ActivityTaskManagerService.ProcessRecord process = processes.get(name);
            long old = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)process.uid << 32) | process.pid);
                check(Binder.getCallingPid() == process.pid && Binder.getCallingUid() == process.uid,
                        "Controlled caller identity was not installed");
                action.run();
            } finally { Binder.restoreCallingIdentity(old); }
        }
        ActivityCallerState.Snapshot snapshot(String source, Intent intent, boolean share) {
            ApkPackage code = packages.get(source);
            synchronized (pm) {
                return ActivityCallerState.capture(new ActivityCallerState.Origin(code, code.uid(), source,
                        false, share), intent, grants);
            }
        }
        ActivityTaskManagerService.ActivityRecord record(String owner, ActivityCallerState.Snapshot snapshot)
                throws Exception {
            ActivityTaskManagerService.ActivityRecord record =
                    (ActivityTaskManagerService.ActivityRecord)UNSAFE.allocateInstance(ActivityTaskManagerService.ActivityRecord.class);
            set(record, "token", token());
            set(record, "initialCallerInfoAccessToken", token()); set(record, "callerState", new ActivityCallerState());
            set(record, "targetCode", packages.get(owner));
            ActivityInfo info = new ActivityInfo(); info.packageName = owner; info.name = owner + ".Activity";
            info.applicationInfo = packages.get(owner).applicationInfo(0);
            set(record, "info", info); set(record, "pendingResults", new ArrayList<>());
            record.app = processes.get(owner); record.state = ActivityTaskManagerService.State.RESUMED;
            record.callerState.add(record.initialCallerInfoAccessToken, snapshot);
            record.app.activities.add(record); activities.add(record);
            return record;
        }
        ComponentCaller caller(ActivityTaskManagerService.ActivityRecord record) {
            return new ComponentCaller(record.token, record.initialCallerInfoAccessToken);
        }
        int permission(String query, ComponentCaller caller, Uri uri, int modes) throws Exception {
            int[] answer = {-999}; as(query, () -> answer[0] = caller.checkContentUriPermission(uri, modes)); return answer[0];
        }
        void grant(String target, Uri uri, int modes) throws Exception {
            as(OWNER, () -> am.grantUriPermission(processes.get(OWNER).thread, target, uri, modes, 0));
        }
        void revoke(String target, Uri uri, int modes) throws Exception {
            as(OWNER, () -> am.revokeUriPermission(processes.get(OWNER).thread, target, uri, modes, 0));
        }
    }
}
