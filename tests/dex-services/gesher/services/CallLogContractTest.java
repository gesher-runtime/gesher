/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.ContentProviderHolder;
import android.app.GesherSettingsContext;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.IContentService;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser;
import android.content.pm.ProviderInfo;
import android.content.pm.Signature;
import android.content.pm.SystemFeaturesCache;
import android.content.res.AssetManager;
import android.database.ContentObserver;
import android.database.Cursor;
import android.database.IContentObserver;
import android.database.sqlite.SQLiteDatabase;
import android.hardware.display.IDisplayManager;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.IServiceManager;
import android.os.Looper;
import android.os.Process;
import android.os.ServiceManager;
import android.permission.IPermissionChecker;
import android.permission.IPermissionManager;
import android.provider.CallLog;
import android.system.Os;
import com.android.internal.app.IAppOpsService;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.Services;
import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.StandIn;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.stream.Stream;
import sun.misc.Unsafe;

/**
 * Contract tests for the built-in SQLite-backed {@code content://call_log} provider,
 * strict SQLiteQueryBuilder validation, phone number matching, DB upgrade, AppOps
 * enforcement (including {@code targetSdk < 23}), URI grants, notification URI
 * registration with {@code ContentService}, and package admission hardening.
 */
public final class CallLogContractTest {
    private static final String BUILT_IN_PKG = "com.android.providers.contacts";
    private static final String PKG_NONE = "fixture.calllog.none";
    private static final String PKG_READ = "fixture.calllog.read";
    private static final String PKG_WRITE = "fixture.calllog.write";
    private static final String PKG_LEGACY = "fixture.calllog.legacy";
    private static final int READ_FLAG = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    private static final int WRITE_FLAG = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;

    private static int sChecks;

    private interface Operation {
        void run() throws Exception;
    }

    private static void check(boolean ok, String reason) {
        if (!ok) throw new AssertionError(reason);
        sChecks++;
    }

    private static void rejects(Class<? extends Throwable> expected, Operation op) throws Exception {
        try {
            op.run();
        } catch (InvocationTargetException wrapped) {
            Throwable cause = wrapped.getCause();
            check(expected.isInstance(cause), "Wrong failure cause: " + cause);
            return;
        } catch (Throwable failure) {
            check(expected.isInstance(failure), "Wrong failure: " + failure);
            return;
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method m = type.getDeclaredMethod(name, params);
        m.setAccessible(true);
        return m;
    }

    private static Object construct(Class<?> type, Class<?>[] params, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(params);
        c.setAccessible(true);
        try {
            return c.newInstance(args);
        } catch (InvocationTargetException wrapped) {
            Throwable cause = wrapped.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            if (cause instanceof Error) throw (Error) cause;
            throw wrapped;
        }
    }

    private static Context observerRecordingContext(ContentProvider provider, List<Uri> notifications) {
        class TestContext extends ContextWrapper {
            ContentResolver resolver;
            TestContext() { super(null); }
            @Override public String getPackageName() { return BUILT_IN_PKG; }
            @Override public String getOpPackageName() { return BUILT_IN_PKG; }
            @Override public int getUserId() { return 0; }
            @Override public ApplicationInfo getApplicationInfo() { return new ApplicationInfo(); }
            @Override public Object getSystemService(String name) { return null; }
            @Override public ContentResolver getContentResolver() { return resolver; }
        }
        TestContext ctx = new TestContext();
        ctx.resolver = new ContentResolver(ctx) {
            @Override protected IContentProvider acquireProvider(Context c, String name) { throw new AssertionError(); }
            @Override protected IContentProvider acquireUnstableProvider(Context c, String name) { throw new AssertionError(); }
            @Override public boolean releaseProvider(IContentProvider p) { return true; }
            @Override public boolean releaseUnstableProvider(IContentProvider p) { return true; }
            @Override public void unstableProviderDied(IContentProvider p) { }
            @Override public void notifyChange(Uri uri, ContentObserver observer) {
                check(!Thread.holdsLock(provider), "Observer notified under provider lock");
                check(Binder.getCallingUid() == Process.myUid(), "Observer notification leaked caller UID");
                notifications.add(uri);
            }
        };
        return ctx;
    }

    private static ContentProvider newCallLogProvider(PackageManagerService pm,
            ActivityTaskManagerService atm, File dbFile) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.CallLogProvider");
        return (ContentProvider) construct(type,
                new Class<?>[] { PackageManagerService.class, ActivityTaskManagerService.class, File.class },
                pm, atm, dbFile);
    }

    private static void testCallLogSqliteAndPersistence(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "calllog.db");
        List<Uri> notifications = new ArrayList<>();
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo attachedInfo = (ProviderInfo) method(support, "callLogProviderInfo").invoke(null);

        ContentProvider provider = newCallLogProvider(null, null, dbFile);
        provider.attachInfo(observerRecordingContext(provider, notifications), attachedInfo);
        try {
            Uri callsUri = CallLog.Calls.CONTENT_URI;
            check("vnd.android.cursor.dir/calls".equals(provider.getType(callsUri)), "CallLog dir MIME");
            check("vnd.android.cursor.item/calls".equals(
                    provider.getType(Uri.withAppendedPath(callsUri, "1"))), "CallLog item MIME");
            rejects(IllegalArgumentException.class,
                    () -> provider.query(Uri.parse("content://call_log/unknown"), null, null, null, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(callsUri, new String[] { "no_such_column" }, null, null, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(callsUri, new String[] { CallLog.Calls._ID }, "no_such_column = ?",
                            new String[] { "1" }, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(callsUri.buildUpon().appendQueryParameter("limit", "-1").build(),
                            null, null, null, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(callsUri.buildUpon().appendQueryParameter("offset", "1").build(),
                            null, null, null, null));

            ContentValues v1 = new ContentValues();
            v1.put(CallLog.Calls.NUMBER, "+972501234567");
            v1.put(CallLog.Calls.CACHED_NAME, "Dana");
            v1.put(CallLog.Calls.TYPE, CallLog.Calls.INCOMING_TYPE);
            v1.put(CallLog.Calls.DATE, 1000L);
            v1.put(CallLog.Calls.DURATION, 45L);
            Uri row1 = provider.insert(callsUri, v1);
            check(row1 != null && ContentUris.parseId(row1) > 0, "Insert first CallLog row");

            ContentValues v2 = new ContentValues();
            v2.put(CallLog.Calls.NUMBER, "050-9876541");
            v2.put(CallLog.Calls.CACHED_NAME, "Uri");
            v2.put(CallLog.Calls.TYPE, CallLog.Calls.OUTGOING_TYPE);
            v2.put(CallLog.Calls.DATE, 2000L);
            v2.put(CallLog.Calls.DURATION, 12L);
            Uri row2 = provider.insert(callsUri, v2);
            check(row2 != null, "Insert second CallLog row");
            check(notifications.size() == 2, "CallLog insert notifies observers");

            ContentValues v3 = new ContentValues();
            v3.put(CallLog.Calls.NUMBER, "052-1112233");
            v3.put(CallLog.Calls.TYPE, CallLog.Calls.MISSED_TYPE);
            v3.put(CallLog.Calls.DATE, 3000L);
            ContentValues v4 = new ContentValues();
            v4.put(CallLog.Calls.NUMBER, "054-4445566");
            v4.put(CallLog.Calls.TYPE, CallLog.Calls.INCOMING_TYPE);
            v4.put(CallLog.Calls.DATE, 4000L);
            check(provider.bulkInsert(callsUri, new ContentValues[] { v3, v4 }) == 2, "BulkInsert CallLog rows");
            check(notifications.size() == 3, "BulkInsert sends single coalesced notification");

            // Normalized numbers are computed automatically
            try (Cursor c = provider.query(row1,
                    new String[] { CallLog.Calls.CACHED_NORMALIZED_NUMBER, CallLog.Calls.NEW,
                            CallLog.Calls.NUMBER_PRESENTATION },
                    null, null, null)) {
                check(c != null && c.moveToFirst()
                        && "+972501234567".equals(c.getString(0))
                        && c.getInt(1) == 1
                        && c.getInt(2) == CallLog.Calls.PRESENTATION_ALLOWED,
                        "Auto-normalized number and CallLog defaults");
            }
            try (Cursor c = provider.query(row2,
                    new String[] { CallLog.Calls.CACHED_NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "0509876541".equals(c.getString(0)),
                        "Strips punctuation in normalized_number");
            }

            // Default sort + limit/offset
            Uri pagedUri = callsUri.buildUpon()
                    .appendQueryParameter("limit", "2")
                    .appendQueryParameter("offset", "1")
                    .build();
            try (Cursor c = provider.query(pagedUri, new String[] { CallLog.Calls.DATE }, null, null, null)) {
                check(c != null && c.getCount() == 2 && c.moveToFirst()
                        && c.getLong(0) == 3000L && c.moveToNext() && c.getLong(0) == 2000L,
                        "Default DESC order and limit/offset pagination");
            }

            // Loose phone number matching (0501234567 <-> +972501234567; short "1" does NOT match "050-9876541")
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(CallLog.Calls.CONTENT_FILTER_URI, Uri.encode("0501234567")),
                    new String[] { CallLog.Calls.CACHED_NAME }, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst() && "Dana".equals(c.getString(0)),
                        "calls/filter/0501234567 matches +972501234567");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(CallLog.Calls.CONTENT_FILTER_URI, Uri.encode("1")),
                    new String[] { CallLog.Calls._ID }, null, null, null)) {
                check(c != null && c.getCount() == 0,
                        "calls/filter/1 must not false-match 050-9876541");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(CallLog.Calls.CONTENT_FILTER_URI, Uri.encode("Dana")),
                    new String[] { CallLog.Calls.CACHED_NAME }, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst() && "Dana".equals(c.getString(0)),
                        "calls/filter by cached name matches Dana");
            }

            // Update recomputes normalized_number and persists
            ContentValues up = new ContentValues();
            up.put(CallLog.Calls.NUMBER, "+1 (555) 765-4321");
            check(provider.update(row1, up, null, null) == 1, "Update CallLog row by ID");
            try (Cursor c = provider.query(row1,
                    new String[] { CallLog.Calls.CACHED_NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "+15557654321".equals(c.getString(0)),
                        "Update recomputes normalized_number");
            }

            check(provider.delete(row2, null, null) == 1, "Delete CallLog row by ID");
            try (Cursor c = provider.query(row2, new String[] { CallLog.Calls._ID }, null, null, null)) {
                check(c != null && c.getCount() == 0, "Deleted row removed");
            }
        } finally {
            provider.shutdown();
        }

        // Reopen existing DB and check persistence across provider restarts
        ContentProvider reopened = newCallLogProvider(null, null, dbFile);
        reopened.attachInfo(observerRecordingContext(reopened, new ArrayList<>()), attachedInfo);
        try {
            try (Cursor c = reopened.query(CallLog.Calls.CONTENT_URI,
                    new String[] { CallLog.Calls._ID, CallLog.Calls.NUMBER }, null, null, null)) {
                check(c != null && c.getCount() == 3, "CallLog data persists across reopen");
            }
        } finally {
            reopened.shutdown();
        }

        // v1 -> v2 schema migration preserves existing rows and adds transcription
        File v1File = new File(dbDir, "calllog-v1.db");
        try (SQLiteDatabase legacyDb = SQLiteDatabase.openOrCreateDatabase(v1File, null)) {
            legacyDb.execSQL("CREATE TABLE calls ("
                    + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    + "number TEXT,"
                    + "presentation INTEGER NOT NULL DEFAULT 1,"
                    + "date INTEGER,"
                    + "duration INTEGER,"
                    + "type INTEGER"
                    + ");");
            legacyDb.execSQL("INSERT INTO calls(number, date, duration, type) VALUES('0501234567', 5555, 10, 1);");
            legacyDb.setVersion(1);
        }
        ContentProvider migrated = newCallLogProvider(null, null, v1File);
        migrated.attachInfo(observerRecordingContext(migrated, new ArrayList<>()), attachedInfo);
        try {
            try (Cursor c = migrated.query(CallLog.Calls.CONTENT_URI,
                    new String[] { CallLog.Calls.NUMBER, "transcription" }, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && "0501234567".equals(c.getString(0)) && c.isNull(1),
                        "v1->v2 CallLog migration preserves rows and adds transcription");
            }
        } finally {
            migrated.shutdown();
        }
    }

    private static ApkPackage fakeApp(Unsafe unsafe, String pkgName, int uid, int targetSdk,
            String... perms) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(pkgName, "/fixture/" + pkgName + "/base.apk");
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(targetSdk);
        for (String perm : perms) {
            parsed.addUsesPermission(new ParsedUsesPermissionImpl(perm, 0, Collections.emptySet()));
        }
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "packageName").set(apk, pkgName);
        field(ApkPackage.class, "parsed").set(apk, parsed);
        field(ApkPackage.class, "apkPath").set(apk, "/fixture/" + pkgName + "/base.apk");
        field(ApkPackage.class, "mDataDir").set(apk, "/fixture/" + pkgName + "/data");
        field(ApkPackage.class, "mUid").set(apk, uid);
        field(ApkPackage.class, "sharedLibraries").set(apk, SharedLibraryGraph.EMPTY);
        field(ApkPackage.class, "mSigningIdentityChecked").set(apk, true);
        field(ApkPackage.class, "mVerifiedSignatures").set(apk,
                new Signature[] { new Signature(new byte[] { (byte) uid }) });
        return apk;
    }

    private static ApkPackage conflictingAuthorityPkg(Unsafe unsafe, String pkgName, int uid,
            String authority) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(pkgName, "/fixture/" + pkgName + "/base.apk");
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(36);
        ParsedProviderImpl p = new ParsedProviderImpl();
        p.setName(pkgName + ".HijackCallLog");
        p.setPackageName(pkgName);
        p.setAuthority(authority);
        p.setEnabled(true);
        parsed.addProvider(p);
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "packageName").set(apk, pkgName);
        field(ApkPackage.class, "parsed").set(apk, parsed);
        field(ApkPackage.class, "apkPath").set(apk, "/fixture/" + pkgName + "/base.apk");
        field(ApkPackage.class, "mDataDir").set(apk, "/fixture/" + pkgName + "/data");
        field(ApkPackage.class, "mUid").set(apk, uid);
        field(ApkPackage.class, "sharedLibraries").set(apk, SharedLibraryGraph.EMPTY);
        field(ApkPackage.class, "mSigningIdentityChecked").set(apk, true);
        field(ApkPackage.class, "mVerifiedSignatures").set(apk,
                new Signature[] { new Signature(new byte[] { (byte) uid }) });
        return apk;
    }

    private static void as(int pid, int uid, Operation op) throws Exception {
        long old = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) uid << 32) | (pid & 0xffffffffL));
            check(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid,
                    "Native Binder identity unavailable");
            op.run();
        } finally {
            Binder.restoreCallingIdentity(old);
        }
    }

    private static AttributionSource source(int uid, String pkg) {
        return new AttributionSource.Builder(uid)
                .setPid(Binder.getCallingPid())
                .setPackageName(pkg)
                .build();
    }

    @SuppressWarnings("unchecked")
    private static void testSecurityPermissionsAppOpsUriGrantsAndPackageManager() throws Exception {
        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Unsafe unsafe = (Unsafe) unsafeField.get(null);

        ApkPackage appNone = fakeApp(unsafe, PKG_NONE, 13201, 36);
        ApkPackage appRead = fakeApp(unsafe, PKG_READ, 13202, 36, Manifest.permission.READ_CALL_LOG);
        ApkPackage appWrite = fakeApp(unsafe, PKG_WRITE, 13203, 36,
                Manifest.permission.READ_CALL_LOG, Manifest.permission.WRITE_CALL_LOG);
        ApkPackage appLegacy = fakeApp(unsafe, PKG_LEGACY, 13204, 22, Manifest.permission.READ_CALL_LOG);
        ApkPackage hijackBootPkg = conflictingAuthorityPkg(unsafe, "fixture.hijack.boot", 13205, "call_log");
        ApkPackage spoofBootPkg = fakeApp(unsafe, BUILT_IN_PKG, 13206, 36);

        PackageManagerService pm = (PackageManagerService) construct(PackageManagerService.class,
                new Class<?>[] { Collection.class, boolean.class },
                Arrays.asList(appNone, appRead, appWrite, appLegacy, hijackBootPkg, spoofBootPkg), false);

        check(pm.get("fixture.hijack.boot") == null,
                "PM constructor safely skips package declaring built-in call_log authority");

        ActivityTaskManagerService atm = (ActivityTaskManagerService) construct(
                ActivityTaskManagerService.class, new Class<?>[] { PackageManagerService.class }, pm);
        Class<?> notificationsType = Class.forName("com.gesher.server.NotificationManagerService");
        Object notifications = construct(notificationsType, new Class<?>[0]);
        ActivityManagerService am = (ActivityManagerService) construct(ActivityManagerService.class,
                new Class<?>[] { ActivityTaskManagerService.class, PackageManagerService.class, notificationsType },
                atm, pm, notifications);
        Class<?> syncType = Class.forName("com.gesher.server.SyncManager");
        Object sync = construct(syncType, new Class<?>[] { ActivityManagerService.class, PackageManagerService.class },
                am, pm);
        IContentService content = (IContentService) construct(Class.forName("com.gesher.server.ContentService"),
                new Class<?>[] { syncType }, sync);

        as(Process.myPid(), Process.SYSTEM_UID, () -> {
            int restrictMask = PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION
                    | PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
            int restrictValue = PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
            pm.permissions().updatePermissionFlags(PKG_READ, Manifest.permission.READ_CALL_LOG,
                    restrictMask, restrictValue, true, "default:0", 0);
            pm.permissions().updatePermissionFlags(PKG_WRITE, Manifest.permission.READ_CALL_LOG,
                    restrictMask, restrictValue, true, "default:0", 0);
            pm.permissions().updatePermissionFlags(PKG_WRITE, Manifest.permission.WRITE_CALL_LOG,
                    restrictMask, restrictValue, true, "default:0", 0);
            pm.permissions().updatePermissionFlags(PKG_LEGACY, Manifest.permission.READ_CALL_LOG,
                    restrictMask, restrictValue, true, "default:0", 0);
            pm.grantRuntimePermission(PKG_READ, Manifest.permission.READ_CALL_LOG, 0);
            pm.grantRuntimePermission(PKG_WRITE, Manifest.permission.READ_CALL_LOG, 0);
            pm.grantRuntimePermission(PKG_WRITE, Manifest.permission.WRITE_CALL_LOG, 0);
        });

        Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        List<Object> processes = (List<Object>) field(ActivityTaskManagerService.class, "mProcesses").get(atm);
        int[][] ids = { { 9201, 13201 }, { 9202, 13202 }, { 9203, 13203 }, { 9204, 13204 } };
        String[] pkgs = { PKG_NONE, PKG_READ, PKG_WRITE, PKG_LEGACY };
        Object[] recs = new Object[pkgs.length];
        IApplicationThread[] threads = new IApplicationThread[pkgs.length];
        for (int i = 0; i < pkgs.length; i++) {
            final int pid = ids[i][0], uid = ids[i][1];
            final String pkg = pkgs[i];
            IApplicationThread thread = new IApplicationThread.Default() {
                private final Binder binder = new Binder();
                @Override public IBinder asBinder() { return binder; }
            };
            threads[i] = thread;
            Object[] holder = new Object[1];
            as(pid, uid, () -> holder[0] = construct(processType,
                    new Class<?>[] { ActivityTaskManagerService.class, int.class, IApplicationThread.class },
                    atm, pid, thread));
            method(ActivityTaskManagerService.class, "assignPackage",
                    processType, String.class, String.class).invoke(atm, holder[0], pkg, pkg);
            field(processType, "ready").setBoolean(holder[0], true);
            processes.add(holder[0]);
            recs[i] = holder[0];
        }
        Object procNone = recs[0], procRead = recs[1], procWrite = recs[2], procLegacy = recs[3];
        IApplicationThread threadRead = threads[1];

        Object providers = field(ActivityManagerService.class, "mProviders").get(am);
        Class<?> cpType = providers.getClass();
        Method getProvider = method(cpType, "getContentProvider", processType, String.class, String.class);
        Object uriGrants = method(cpType, "uriGrants").invoke(providers);
        Method canAccess = method(uriGrants.getClass(), "canAccess", ApkPackage.class, Uri.class, int.class, int.class);
        ContentProvider callProv = (ContentProvider) method(cpType, "callLog").invoke(providers);

        IContentObserver observer = new IContentObserver.Stub() {
            @Override public void onChange(boolean self, Uri uri, int user) { }
            @Override public void onChangeEtc(boolean self, Uri[] uris, int flags, int user) { }
        };
        Uri callLogUri = CallLog.Calls.CONTENT_URI;

        Field cacheField = ServiceManager.class.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        Map<String, IBinder> cache = (Map<String, IBinder>) cacheField.get(null);
        Map<String, IBinder> previousCache = new java.util.HashMap<>(cache);
        Field smField = ServiceManager.class.getDeclaredField("sServiceManager");
        smField.setAccessible(true);
        Object previousSm = smField.get(null);

        Map<String, IBinder> testServices = new java.util.LinkedHashMap<>();
        testServices.put("package", StandIn.of(android.content.pm.IPackageManager.class, pm).asBinder());
        testServices.put(Context.DISPLAY_SERVICE,
                StandIn.of(IDisplayManager.class, new Services.DisplayManager()).asBinder());
        testServices.put("permissionmgr", StandIn.of(IPermissionManager.class, pm.permissions()).asBinder());
        testServices.put(Context.APP_OPS_SERVICE,
                StandIn.of(IAppOpsService.class, pm.appOps()).asBinder());
        IPermissionChecker permChecker = (IPermissionChecker) construct(
                Class.forName("com.gesher.server.AppOpsPermissionChecker"),
                new Class<?>[] { PackageManagerService.class }, pm);
        testServices.put(Context.PERMISSION_CHECKER_SERVICE,
                StandIn.of(IPermissionChecker.class, permChecker).asBinder());
        testServices.put(Context.ACTIVITY_SERVICE, StandIn.of(IActivityManager.class, am).asBinder());
        testServices.put(ContentResolver.CONTENT_SERVICE_NAME,
                StandIn.of(IContentService.class, content).asBinder());
        Constructor<?> smCtor = Services.ServiceManagerStandIn.class.getDeclaredConstructor(Map.class);
        smCtor.setAccessible(true);
        IServiceManager inProcessSm = (IServiceManager) smCtor.newInstance(testServices);
        smField.set(null, StandIn.of(IServiceManager.class, inProcessSm).asInterface());
        cache.putAll(testServices);
        SystemFeaturesCache.setInstance(new SystemFeaturesCache(pm.getSystemAvailableFeatures().getList()));
        try {
            // Unprivileged package: denied everywhere before URI grant
            as(9201, 13201, () -> {
                rejects(SecurityException.class, () -> getProvider.invoke(providers, procNone, PKG_NONE, "call_log"));
                rejects(SecurityException.class,
                        () -> content.registerContentObserver(callLogUri, true, observer, 0, 36));
                rejects(SecurityException.class,
                        () -> callProv.query(callLogUri, null, null, null, null));
            });
            check(!(Boolean) canAccess.invoke(uriGrants, appNone, callLogUri, READ_FLAG, 0),
                    "UriGrants denies unprivileged call_log");

            // Write package inserts two records
            Uri[] insertedUris = new Uri[2];
            as(9203, 13203, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procWrite, PKG_WRITE, "call_log");
                check(h != null && h.provider != null && h.info != null
                        && Manifest.permission.READ_CALL_LOG.equals(h.info.readPermission)
                        && Manifest.permission.WRITE_CALL_LOG.equals(h.info.writePermission)
                        && h.info.grantUriPermissions,
                        "Write app acquires call_log holder with real permissions preserved");
                for (int i = 0; i < 2; i++) {
                    ContentValues cv = new ContentValues();
                    cv.put(CallLog.Calls.NUMBER, "050000000" + (i + 1));
                    cv.put(CallLog.Calls.TYPE, CallLog.Calls.INCOMING_TYPE);
                    cv.put(CallLog.Calls.DATE, 1000L + i);
                    insertedUris[i] = h.provider.insert(source(13203, PKG_WRITE), callLogUri, cv, null);
                    check(insertedUris[i] != null, "Write app inserts call_log row " + i);
                }
            });

            // Read-only package: can acquire, query via IContentProvider (which sets notification URI via ContentService),
            // cannot write, and AppOps records read op
            as(9202, 13202, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procRead, PKG_READ, "call_log");
                check(h != null && h.provider != null, "Read app acquires call_log holder");
                try (Cursor c = h.provider.query(source(13202, PKG_READ), callLogUri,
                        new String[] { CallLog.Calls._ID, CallLog.Calls.NUMBER }, null, null)) {
                    check(c != null && c.getCount() == 2,
                            "Read app queries call_log with registered ContentService notification URI");
                }
                content.registerContentObserver(callLogUri, true, observer, 0, 36);
                ContentValues cv = new ContentValues();
                cv.put(CallLog.Calls.NUMBER, "0509999999");
                rejects(SecurityException.class,
                        () -> h.provider.insert(source(13202, PKG_READ), callLogUri, cv, null));
                rejects(SecurityException.class,
                        () -> content.notifyChange(new Uri[] { callLogUri }, null, false, 0, 0, 36, PKG_READ));
            });

            // Legacy targetSdk 22 app: allowed initially, then AppOps-revoked via FLAG_PERMISSION_REVOKED_COMPAT
            // while PermissionPolicy.checkPermission STILL reports PERMISSION_GRANTED
            as(9204, 13204, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procLegacy, PKG_LEGACY, "call_log");
                check(h != null && h.provider != null, "Legacy app acquires call_log holder");
                try (Cursor c = callProv.query(callLogUri, new String[] { CallLog.Calls._ID }, null, null, null)) {
                    check(c != null && c.getCount() == 2, "Legacy app initially reads call_log");
                }
            });
            as(Process.myPid(), Process.SYSTEM_UID, () -> {
                pm.permissions().updatePermissionFlags(PKG_LEGACY, Manifest.permission.READ_CALL_LOG,
                        PackageManager.FLAG_PERMISSION_REVOKED_COMPAT,
                        PackageManager.FLAG_PERMISSION_REVOKED_COMPAT, true, "default:0", 0);
            });
            check(pm.permissions().checkPermission(PKG_LEGACY, Manifest.permission.READ_CALL_LOG)
                            == PackageManager.PERMISSION_GRANTED,
                    "Legacy compat revocation leaves checkPermission == GRANTED");
            as(9204, 13204, () -> {
                rejects(SecurityException.class,
                        () -> callProv.query(callLogUri, new String[] { CallLog.Calls._ID }, null, null, null));
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procLegacy, PKG_LEGACY, "call_log");
                rejects(SecurityException.class, () -> h.provider.query(source(13204, PKG_LEGACY),
                        callLogUri, new String[] { CallLog.Calls._ID }, null, null));
            });

            // URI grant flow: appRead grants row 0 READ_URI_PERMISSION to appNone (which holds no READ_CALL_LOG)
            Uri grantedUri = insertedUris[0];
            Uri ungrantedUri = insertedUris[1];
            as(9202, 13202, () -> am.grantUriPermission(threadRead, PKG_NONE, grantedUri, READ_FLAG, 0));
            check((Boolean) canAccess.invoke(uriGrants, appNone, grantedUri, READ_FLAG, 0),
                    "UriGrants permits granted call_log row");
            check(!(Boolean) canAccess.invoke(uriGrants, appNone, ungrantedUri, READ_FLAG, 0),
                    "UriGrants denies ungranted call_log row");
            as(9201, 13201, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procNone, PKG_NONE, "call_log");
                check(h != null && h.provider != null, "URI-granted app acquires call_log holder");
                try (Cursor c = callProv.query(grantedUri,
                        new String[] { CallLog.Calls._ID, CallLog.Calls.NUMBER }, null, null, null)) {
                    check(c != null && c.getCount() == 1 && c.moveToFirst()
                            && "0500000001".equals(c.getString(1)),
                            "URI-granted app queries specific call_log row");
                }
                rejects(SecurityException.class,
                        () -> callProv.query(ungrantedUri, new String[] { CallLog.Calls._ID }, null, null, null));
                rejects(SecurityException.class,
                        () -> callProv.query(callLogUri, new String[] { CallLog.Calls._ID }, null, null, null));
                ContentValues blocked = new ContentValues();
                blocked.put(CallLog.Calls.NUMBER, "0500000009");
                rejects(SecurityException.class, () -> callProv.insert(callLogUri, blocked));
                rejects(SecurityException.class, () -> callProv.delete(grantedUri, null, null));
            });
            rejects(SecurityException.class,
                    () -> as(9202, 13202, () -> content.registerContentObserver(callLogUri, true, observer, 10, 36)));

            // Restoring legacy compat flag re-enables AppOps-mediated access
            as(Process.myPid(), Process.SYSTEM_UID, () -> pm.permissions().updatePermissionFlags(
                    PKG_LEGACY, Manifest.permission.READ_CALL_LOG,
                    PackageManager.FLAG_PERMISSION_REVOKED_COMPAT, 0, true, "default:0", 0));
            as(9204, 13204, () -> {
                try (Cursor c = callProv.query(callLogUri, new String[] { CallLog.Calls._ID }, null, null, null)) {
                    check(c != null && c.getCount() == 2, "Restoring legacy compat flag restores read access");
                }
            });
        } finally {
            callProv.shutdown();
            cache.clear();
            cache.putAll(previousCache);
            smField.set(null, previousSm);
        }

        // PackageManager resolution, versionName, SYSTEM_UID visibility, and getInstalledPackages isolation
        ProviderInfo callInfo = pm.resolveContentProvider("call_log", 0, 0);
        check(callInfo != null && BUILT_IN_PKG.equals(callInfo.packageName)
                && Manifest.permission.READ_CALL_LOG.equals(callInfo.readPermission)
                && Manifest.permission.WRITE_CALL_LOG.equals(callInfo.writePermission)
                && callInfo.grantUriPermissions,
                "PM resolves call_log with permissions and grantUriPermissions");
        check(pm.resolveContentProvider("call_log", 0, 10) == null, "PM resolveContentProvider denies user 10");
        check(pm.getProviderInfo(new ComponentName(BUILT_IN_PKG,
                "com.android.providers.contacts.CallLogProvider"), 0, 0) != null,
                "PM getProviderInfo resolves CallLogProvider");
        check(pm.getProviderInfo(new ComponentName(BUILT_IN_PKG,
                "com.android.providers.contacts.CallLogProvider"), 0, 10) == null,
                "PM getProviderInfo denies user 10");

        PackageInfo builtInPkg = pm.getPackageInfo(BUILT_IN_PKG, PackageManager.GET_PROVIDERS, 0);
        check(builtInPkg != null && builtInPkg.versionName != null && !builtInPkg.versionName.isEmpty()
                && builtInPkg.providers != null && builtInPkg.providers.length >= 1,
                "PM getPackageInfo returns non-null versionName and providers");
        check(pm.getApplicationInfo(BUILT_IN_PKG, 0, 0) != null, "PM getApplicationInfo exposes built-in contacts pkg");
        check(Arrays.asList(pm.getPackagesForUid(Process.SYSTEM_UID)).contains(BUILT_IN_PKG),
                "SYSTEM_UID includes com.android.providers.contacts");

        for (Object item : pm.getInstalledPackages(0, 0).getList()) {
            PackageInfo installed = (PackageInfo) item;
            check(!BUILT_IN_PKG.equals(installed.packageName),
                    "getInstalledPackages must not include synthetic " + BUILT_IN_PKG);
        }
        for (Object item : pm.getInstalledApplications(0, 0).getList()) {
            ApplicationInfo installed = (ApplicationInfo) item;
            check(!BUILT_IN_PKG.equals(installed.packageName),
                    "getInstalledApplications must not include synthetic " + BUILT_IN_PKG);
        }

        // Admission & prepareAdd reject spoofing built-in authorities/package
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        Method admit = method(support, "checkPackageAdmission", ApkPackage.class);
        ApkPackage hijackPkg = conflictingAuthorityPkg(unsafe, "fixture.hijack", 13298, "call_log");
        rejects(PackageParser.PackageParserException.class, () -> admit.invoke(null, hijackPkg));
        ApkPackage spoofPkg = fakeApp(unsafe, BUILT_IN_PKG, 13299, 36);
        rejects(PackageParser.PackageParserException.class, () -> admit.invoke(null, spoofPkg));
        Method prepareAdd = method(PackageManagerService.class, "prepareAdd", ApkPackage.class);
        rejects(IllegalArgumentException.class, () -> prepareAdd.invoke(pm, hijackPkg));
        rejects(IllegalArgumentException.class, () -> prepareAdd.invoke(pm, spoofPkg));
    }

    public static void main(String[] args) throws Exception {
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        GesherSettingsContext.prepare();
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        String previousData = System.getenv("ANDROID_DATA");
        String previousSpoof = System.getenv("GESHER_MICROG_SPOOF");
        Path data = Files.createTempDirectory("gesher-calllog-contract-");
        try {
            Os.setenv("ANDROID_DATA", data.toString(), true);
            Os.setenv("GESHER_MICROG_SPOOF", "1", true);
            testCallLogSqliteAndPersistence(data.toFile());
            testSecurityPermissionsAppOpsUriGrantsAndPackageManager();
            System.out.println("CallLogContractTest all passed: " + sChecks + " checks");
        } finally {
            if (previousData == null) Os.unsetenv("ANDROID_DATA");
            else Os.setenv("ANDROID_DATA", previousData, true);
            if (previousSpoof == null) Os.unsetenv("GESHER_MICROG_SPOOF");
            else Os.setenv("GESHER_MICROG_SPOOF", previousSpoof, true);
            try (Stream<Path> walk = Files.walk(data)) {
                for (Path p : (Iterable<Path>) walk.sorted(Comparator.reverseOrder())::iterator) {
                    Files.deleteIfExists(p);
                }
            }
        }
    }
}
