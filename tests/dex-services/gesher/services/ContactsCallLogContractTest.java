/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.app.IApplicationThread;
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
import android.content.res.AssetManager;
import android.database.ContentObserver;
import android.database.Cursor;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.provider.CallLog;
import android.provider.ContactsContract;
import android.system.Os;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
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
import java.util.stream.Stream;
import sun.misc.Unsafe;

/**
 * Contract tests for built-in SQLite-backed CallLog and Contacts providers,
 * permission enforcement, ContentObserver gating, PackageManager resolution,
 * and admission boundaries.
 */
public final class ContactsCallLogContractTest {
    private static final String PKG_NONE = "fixture.contacts.none";
    private static final String PKG_READ = "fixture.contacts.read";
    private static final String PKG_WRITE = "fixture.contacts.write";
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
            @Override public String getPackageName() { return "com.android.providers.contacts"; }
            @Override public String getOpPackageName() { return "com.android.providers.contacts"; }
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

    private static ContentProvider newContactsProvider(PackageManagerService pm,
            ActivityTaskManagerService atm, File dbFile) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.ContactsProvider");
        return (ContentProvider) construct(type,
                new Class<?>[] { PackageManagerService.class, ActivityTaskManagerService.class, File.class },
                pm, atm, dbFile);
    }

    private static void testCallLogSqliteAndPersistence(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "calllog.db");
        List<Uri> notifications = new ArrayList<>();
        ContentProvider provider = newCallLogProvider(null, null, dbFile);
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo attachedInfo = (ProviderInfo) method(support, "withoutPermissions", ProviderInfo.class)
                .invoke(null, method(support, "callLogProviderInfo").invoke(null));
        provider.attachInfo(observerRecordingContext(provider, notifications), attachedInfo);
        try {
            Uri callsUri = CallLog.Calls.CONTENT_URI;
            check("vnd.android.cursor.dir/calls".equals(provider.getType(callsUri)), "CallLog dir MIME");
            check("vnd.android.cursor.item/calls".equals(
                    provider.getType(Uri.withAppendedPath(callsUri, "1"))), "CallLog item MIME");
            rejects(IllegalArgumentException.class,
                    () -> provider.query(Uri.parse("content://call_log/unknown"), null, null, null, null));

            try (Cursor empty = provider.query(callsUri, null, null, null, null)) {
                check(empty != null && empty.getCount() == 0, "Fresh CallLog must be empty");
            }

            ContentValues first = new ContentValues();
            first.put(CallLog.Calls.NUMBER, "+15550101");
            first.put(CallLog.Calls.CACHED_NAME, "Alice");
            first.put(CallLog.Calls.TYPE, CallLog.Calls.OUTGOING_TYPE);
            first.put(CallLog.Calls.DATE, 2000L);
            first.put(CallLog.Calls.DURATION, 30L);
            Uri insertedFirst = provider.insert(callsUri, first);
            check(insertedFirst != null, "CallLog insert returned null");

            ContentValues second = new ContentValues();
            second.put(CallLog.Calls.NUMBER, "050-9876543");
            second.put(CallLog.Calls.CACHED_NAME, "Bob");
            second.put(CallLog.Calls.TYPE, CallLog.Calls.INCOMING_TYPE);
            second.put(CallLog.Calls.DATE, 1000L);
            second.put(CallLog.Calls.DURATION, 15L);
            Uri insertedSecond = provider.insert(callsUri, second);
            check(insertedSecond != null && notifications.size() == 2, "CallLog insert notifications");

            try (Cursor all = provider.query(callsUri,
                    new String[] { CallLog.Calls.CACHED_NAME, CallLog.Calls.NUMBER }, null, null, null)) {
                check(all != null && all.getCount() == 2 && all.moveToFirst(), "CallLog query all count");
                check("Alice".equals(all.getString(0)), "Default sort DESC places Alice first");
            }

            try (Cursor byId = provider.query(insertedSecond,
                    new String[] { CallLog.Calls.CACHED_NAME }, null, null, null)) {
                check(byId != null && byId.getCount() == 1 && byId.moveToFirst()
                        && "Bob".equals(byId.getString(0)), "CallLog query by ID");
            }

            try (Cursor filtered = provider.query(Uri.parse("content://call_log/calls/filter/9876"),
                    new String[] { CallLog.Calls.CACHED_NAME }, null, null, null)) {
                check(filtered != null && filtered.getCount() == 1 && filtered.moveToFirst()
                        && "Bob".equals(filtered.getString(0)), "CallLog filter query");
            }

            try (Cursor limited = provider.query(Uri.parse("content://call_log/calls?limit=1"),
                    null, null, null, null)) {
                check(limited != null && limited.getCount() == 1, "CallLog ?limit=1");
            }
            rejects(IllegalArgumentException.class,
                    () -> provider.query(Uri.parse("content://call_log/calls?limit=-1"), null, null, null, null));

            ContentValues update = new ContentValues();
            update.put(CallLog.Calls.DURATION, 99L);
            check(provider.update(insertedFirst, update, null, null) == 1, "CallLog update by ID");
            check(provider.delete(insertedSecond, null, null) == 1, "CallLog delete by ID");
            check(notifications.size() == 4, "CallLog update/delete notified observers");
        } finally {
            provider.shutdown();
        }

        ContentProvider reopened = newCallLogProvider(null, null, dbFile);
        try {
            try (Cursor persisted = reopened.query(CallLog.Calls.CONTENT_URI,
                    new String[] { CallLog.Calls.CACHED_NAME, CallLog.Calls.DURATION }, null, null, null)) {
                check(persisted != null && persisted.getCount() == 1 && persisted.moveToFirst(),
                        "CallLog persistence across shutdown/reopen");
                check("Alice".equals(persisted.getString(0)) && persisted.getLong(1) == 99L,
                        "Persisted CallLog row values");
            }
        } finally {
            reopened.shutdown();
        }
    }

    private static void testContactsSqliteAndPersistence(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts2.db");
        List<Uri> notifications = new ArrayList<>();
        ContentProvider provider = newContactsProvider(null, null, dbFile);
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo attachedInfo = (ProviderInfo) method(support, "withoutPermissions", ProviderInfo.class)
                .invoke(null, method(support, "contactsProviderInfo").invoke(null));
        provider.attachInfo(observerRecordingContext(provider, notifications), attachedInfo);
        long contactId;
        String lookupKey;
        try {
            rejects(IllegalArgumentException.class,
                    () -> provider.query(Uri.parse("content://com.android.contacts/unknown_table"),
                            null, null, null, null));
            try (Cursor empty = provider.query(ContactsContract.Contacts.CONTENT_URI, null, null, null, null)) {
                check(empty != null && empty.getCount() == 0, "Fresh Contacts must be empty");
            }
            try (Cursor dirs = provider.query(ContactsContract.Directory.CONTENT_URI,
                    new String[] { ContactsContract.Directory._ID }, null, null, null)) {
                check(dirs != null && dirs.getCount() == 1 && dirs.moveToFirst() && dirs.getLong(0) == 0L,
                        "Default local directory seeded");
            }

            Uri rawUri = provider.insert(ContactsContract.RawContacts.CONTENT_URI, new ContentValues());
            check(rawUri != null, "RawContacts insert");
            long rawId = ContentUris.parseId(rawUri);

            ContentValues nameRow = new ContentValues();
            nameRow.put(ContactsContract.Data.RAW_CONTACT_ID, rawId);
            nameRow.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
            nameRow.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "Ada Lovelace");
            Uri nameDataUri = provider.insert(ContactsContract.Data.CONTENT_URI, nameRow);
            check(nameDataUri != null, "StructuredName insert");

            ContentValues phoneRow = new ContentValues();
            phoneRow.put(ContactsContract.Data.RAW_CONTACT_ID, rawId);
            phoneRow.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "+1 (555) 234-5678");
            phoneRow.put(ContactsContract.CommonDataKinds.Phone.TYPE, ContactsContract.CommonDataKinds.Phone.TYPE_MOBILE);
            Uri phoneUri = provider.insert(ContactsContract.CommonDataKinds.Phone.CONTENT_URI, phoneRow);
            check(phoneUri != null, "Phone insert via data/phones");

            ContentValues emailRow = new ContentValues();
            emailRow.put(ContactsContract.Data.RAW_CONTACT_ID, rawId);
            emailRow.put(ContactsContract.CommonDataKinds.Email.ADDRESS, "ada@example.test");
            Uri emailUri = provider.insert(ContactsContract.CommonDataKinds.Email.CONTENT_URI, emailRow);
            check(emailUri != null && notifications.size() == 4, "Email insert & notifications");

            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts._ID, ContactsContract.Contacts.LOOKUP_KEY,
                            ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.HAS_PHONE_NUMBER },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst(), "Contacts row count");
                contactId = c.getLong(0);
                lookupKey = c.getString(1);
                check("Ada Lovelace".equals(c.getString(2)) && c.getInt(3) == 1 && lookupKey != null,
                        "Contacts projection display_name/has_phone_number/lookup");
            }

            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.Contacts.CONTENT_LOOKUP_URI, lookupKey),
                    new String[] { ContactsContract.Contacts._ID }, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst() && c.getLong(0) == contactId,
                        "Contacts lookup URI");
            }

            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("5552345678")),
                    new String[] { ContactsContract.PhoneLookup.DISPLAY_NAME, ContactsContract.PhoneLookup.NUMBER },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && "Ada Lovelace".equals(c.getString(0)), "PhoneLookup normalized match");
            }

            try (Cursor legacy = provider.query(Uri.parse("content://contacts/phones"),
                    new String[] { ContactsContract.CommonDataKinds.Phone.NUMBER }, null, null, null)) {
                check(legacy != null && legacy.getCount() == 1 && legacy.moveToFirst()
                        && "+1 (555) 234-5678".equals(legacy.getString(0)), "Legacy contacts/phones query");
            }

            ContentValues group = new ContentValues();
            group.put(ContactsContract.Groups.TITLE, "Engineers");
            Uri groupUri = provider.insert(ContactsContract.Groups.CONTENT_URI, group);
            check(groupUri != null, "Groups insert");

            ContentValues rename = new ContentValues();
            rename.put(ContactsContract.Data.DATA1, "Ada King");
            check(provider.update(nameDataUri, rename, null, null) == 1, "Update structured name");
            check(provider.delete(phoneUri, null, null) == 1, "Delete phone data row");
            try (Cursor c = provider.query(
                    ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, contactId),
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.HAS_PHONE_NUMBER },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "Ada King".equals(c.getString(0)) && c.getInt(1) == 0,
                        "Parent contact updated after data mutation/deletion");
            }

            check(provider.delete(
                    ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, contactId), null, null) == 1,
                    "Delete contact");
            try (Cursor del = provider.query(ContactsContract.DeletedContacts.CONTENT_URI,
                    new String[] { ContactsContract.DeletedContacts.CONTACT_ID }, null, null, null)) {
                check(del != null && del.getCount() == 1 && del.moveToFirst() && del.getLong(0) == contactId,
                        "DeletedContacts records deleted contact_id");
            }
        } finally {
            provider.shutdown();
        }

        ContentProvider reopened = newContactsProvider(null, null, dbFile);
        try {
            try (Cursor groups = reopened.query(ContactsContract.Groups.CONTENT_URI,
                    new String[] { ContactsContract.Groups.TITLE }, null, null, null)) {
                check(groups != null && groups.getCount() == 1 && groups.moveToFirst()
                        && "Engineers".equals(groups.getString(0)), "Contacts DB persistence");
            }
        } finally {
            reopened.shutdown();
        }
    }

    private static ApkPackage fakeApp(Unsafe unsafe, String pkgName, int uid, String... perms) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(pkgName, "/fixture/" + pkgName + "/base.apk");
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(36);
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

    @SuppressWarnings("unchecked")
    private static void testSecurityPermissionsAndPackageManager() throws Exception {
        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Unsafe unsafe = (Unsafe) unsafeField.get(null);

        ApkPackage appNone = fakeApp(unsafe, PKG_NONE, 13101);
        ApkPackage appRead = fakeApp(unsafe, PKG_READ, 13102,
                "android.permission.READ_CALL_LOG", "android.permission.READ_CONTACTS");
        ApkPackage appWrite = fakeApp(unsafe, PKG_WRITE, 13103,
                "android.permission.READ_CALL_LOG", "android.permission.WRITE_CALL_LOG",
                "android.permission.READ_CONTACTS", "android.permission.WRITE_CONTACTS");

        PackageManagerService pm = (PackageManagerService) construct(PackageManagerService.class,
                new Class<?>[] { Collection.class, boolean.class },
                Arrays.asList(appNone, appRead, appWrite), false);
        pm.grantRuntimePermission(PKG_READ, "android.permission.READ_CALL_LOG", 0);
        pm.grantRuntimePermission(PKG_READ, "android.permission.READ_CONTACTS", 0);
        pm.grantRuntimePermission(PKG_WRITE, "android.permission.READ_CALL_LOG", 0);
        pm.grantRuntimePermission(PKG_WRITE, "android.permission.WRITE_CALL_LOG", 0);
        pm.grantRuntimePermission(PKG_WRITE, "android.permission.READ_CONTACTS", 0);
        pm.grantRuntimePermission(PKG_WRITE, "android.permission.WRITE_CONTACTS", 0);

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

        Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        List<Object> processes = (List<Object>) field(ActivityTaskManagerService.class, "mProcesses").get(atm);
        Object procNone = null, procRead = null, procWrite = null;
        int[][] ids = { { 9101, 13101 }, { 9102, 13102 }, { 9103, 13103 } };
        String[] pkgs = { PKG_NONE, PKG_READ, PKG_WRITE };
        Object[] recs = new Object[3];
        for (int i = 0; i < 3; i++) {
            final int pid = ids[i][0], uid = ids[i][1];
            final String pkg = pkgs[i];
            IApplicationThread thread = new IApplicationThread.Default() {
                private final Binder binder = new Binder();
                @Override public IBinder asBinder() { return binder; }
            };
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
        procNone = recs[0];
        procRead = recs[1];
        procWrite = recs[2];

        Object providers = field(ActivityManagerService.class, "mProviders").get(am);
        Class<?> cpType = providers.getClass();
        Method getProvider = method(cpType, "getContentProvider", processType, String.class, String.class);
        Object uriGrants = method(cpType, "uriGrants").invoke(providers);
        Method canAccess = method(uriGrants.getClass(), "canAccess", ApkPackage.class, Uri.class, int.class, int.class);

        IContentObserver observer = new IContentObserver.Stub() {
            @Override public void onChange(boolean self, Uri uri, int user) { }
            @Override public void onChangeEtc(boolean self, Uri[] uris, int flags, int user) { }
        };

        Uri callLogUri = CallLog.Calls.CONTENT_URI;
        Uri contactsUri = ContactsContract.Contacts.CONTENT_URI;
        Uri legacyUri = Uri.parse("content://contacts/people");

        // Unprivileged package: denied everywhere
        final Object pNone = procNone;
        as(9101, 13101, () -> {
            rejects(SecurityException.class, () -> getProvider.invoke(providers, pNone, PKG_NONE, "call_log"));
            rejects(SecurityException.class, () -> getProvider.invoke(providers, pNone, PKG_NONE, "com.android.contacts"));
            rejects(SecurityException.class, () -> getProvider.invoke(providers, pNone, PKG_NONE, "contacts"));
            rejects(SecurityException.class, () -> content.registerContentObserver(callLogUri, true, observer, 0, 36));
            rejects(SecurityException.class, () -> content.registerContentObserver(contactsUri, true, observer, 0, 36));
            rejects(SecurityException.class, () -> content.registerContentObserver(legacyUri, true, observer, 0, 36));
        });
        check(!(Boolean) canAccess.invoke(uriGrants, appNone, callLogUri, READ_FLAG, 0), "UriGrants denies unprivileged call_log");
        check(!(Boolean) canAccess.invoke(uriGrants, appNone, contactsUri, READ_FLAG, 0), "UriGrants denies unprivileged contacts");

        // Read-only package: can observe and query, cannot insert/notify
        final Object pRead = procRead;
        as(9102, 13102, () -> {
            content.registerContentObserver(callLogUri, true, observer, 0, 36);
            content.registerContentObserver(contactsUri, true, observer, 0, 36);
            content.registerContentObserver(legacyUri, true, observer, 0, 36);
            rejects(SecurityException.class, () -> content.notifyChange(new Uri[] { callLogUri }, null, false, 0, 0, 36, PKG_READ));
            rejects(SecurityException.class, () -> content.notifyChange(new Uri[] { contactsUri }, null, false, 0, 0, 36, PKG_READ));

            ContentProviderHolder callHolder = (ContentProviderHolder) getProvider.invoke(providers, pRead, PKG_READ, "call_log");
            check(callHolder != null && callHolder.provider != null, "Read-permitted app acquires call_log");
            ContentProvider callProv = (ContentProvider) field(cpType, "mCallLog").get(providers);
            try (Cursor c = callProv.query(callLogUri, null, null, null, null)) {
                check(c != null && c.getCount() == 0, "Read-permitted app queries empty call_log");
            }
            rejects(SecurityException.class, () -> callProv.insert(callLogUri, new ContentValues()));

            ContentProviderHolder contactsHolder = (ContentProviderHolder) getProvider.invoke(providers, pRead, PKG_READ, "com.android.contacts");
            check(contactsHolder != null && contactsHolder.provider != null, "Read-permitted app acquires com.android.contacts");
            ContentProvider contactsProv = (ContentProvider) field(cpType, "mContacts").get(providers);
            try (Cursor c = contactsProv.query(contactsUri, null, null, null, null)) {
                check(c != null && c.getCount() == 0, "Read-permitted app queries empty contacts");
            }
            rejects(SecurityException.class, () -> contactsProv.insert(ContactsContract.RawContacts.CONTENT_URI, new ContentValues()));
        });
        check((Boolean) canAccess.invoke(uriGrants, appRead, callLogUri, READ_FLAG, 0), "UriGrants allows read call_log");
        check(!(Boolean) canAccess.invoke(uriGrants, appRead, callLogUri, WRITE_FLAG, 0), "UriGrants denies write call_log for reader");

        // Write-permitted package: insert, query and notifyChange succeed
        final Object pWrite = procWrite;
        as(9103, 13103, () -> {
            getProvider.invoke(providers, pWrite, PKG_WRITE, "call_log");
            ContentProvider callProv = (ContentProvider) field(cpType, "mCallLog").get(providers);
            ContentValues cv = new ContentValues();
            cv.put(CallLog.Calls.NUMBER, "555-1234");
            cv.put(CallLog.Calls.TYPE, CallLog.Calls.MISSED_TYPE);
            cv.put(CallLog.Calls.DATE, 1234L);
            Uri inserted = callProv.insert(callLogUri, cv);
            check(inserted != null, "Write-permitted app inserts into call_log");
            content.notifyChange(new Uri[] { inserted }, null, false, 0, 0, 36, PKG_WRITE);

            rejects(SecurityException.class, () -> content.registerContentObserver(callLogUri, true, observer, 10, 36));
        });

        // PackageManager resolution & SYSTEM_UID metadata
        ProviderInfo callInfo = pm.resolveContentProvider("call_log", 0, 0);
        ProviderInfo contactsInfo = pm.resolveContentProvider("com.android.contacts", 0, 0);
        ProviderInfo legacyInfo = pm.resolveContentProvider("contacts", 0, 0);
        check(callInfo != null && "com.android.providers.contacts".equals(callInfo.packageName),
                "PM resolves call_log");
        check(contactsInfo != null && "com.android.providers.contacts".equals(contactsInfo.packageName),
                "PM resolves com.android.contacts");
        check(legacyInfo != null && "com.android.providers.contacts".equals(legacyInfo.packageName),
                "PM resolves legacy contacts");
        check(pm.resolveContentProvider("call_log", 0, 10) == null, "PM resolveContentProvider denies user 10");
        check(pm.getProviderInfo(new ComponentName("com.android.providers.contacts",
                "com.android.providers.contacts.CallLogProvider"), 0, 0) != null, "PM getProviderInfo CallLog");
        check(pm.getProviderInfo(new ComponentName("com.android.providers.contacts",
                "com.android.providers.contacts.CallLogProvider"), 0, 10) == null, "PM getProviderInfo denies user 10");
        PackageInfo builtInPkg = pm.getPackageInfo("com.android.providers.contacts", PackageManager.GET_PROVIDERS, 0);
        check(builtInPkg != null && builtInPkg.providers != null && builtInPkg.providers.length == 2,
                "PM getPackageInfo exposes built-in contacts providers");
        check(Arrays.asList(pm.getPackagesForUid(Process.SYSTEM_UID)).contains("com.android.providers.contacts"),
                "SYSTEM_UID includes com.android.providers.contacts");

        // Package admission & publishContentProviders reject spoofing built-in authorities/package
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        Method admit = method(support, "checkPackageAdmission", ApkPackage.class);

        PackageImpl hijackAuth = (PackageImpl) PackageImpl.forTesting("fixture.hijack", "/fixture/hijack/base.apk");
        ParsedProviderImpl p = new ParsedProviderImpl();
        p.setName("fixture.hijack.CallLog");
        p.setPackageName("fixture.hijack");
        p.setAuthority("call_log");
        p.setEnabled(true);
        hijackAuth.addProvider(p);
        hijackAuth.hideAsFinal();
        ApkPackage hijackPkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "packageName").set(hijackPkg, "fixture.hijack");
        field(ApkPackage.class, "parsed").set(hijackPkg, hijackAuth);
        rejects(PackageParser.PackageParserException.class, () -> admit.invoke(null, hijackPkg));

        ApkPackage spoofPkg = fakeApp(unsafe, "com.android.providers.contacts", 13199);
        rejects(PackageParser.PackageParserException.class, () -> admit.invoke(null, spoofPkg));
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        String previousData = System.getenv("ANDROID_DATA");
        String previousSpoof = System.getenv("GESHER_MICROG_SPOOF");
        Path data = Files.createTempDirectory("gesher-contacts-contract-");
        try {
            Os.setenv("ANDROID_DATA", data.toString(), true);
            Os.setenv("GESHER_MICROG_SPOOF", "1", true);
            testCallLogSqliteAndPersistence(data.toFile());
            testContactsSqliteAndPersistence(data.toFile());
            testSecurityPermissionsAndPackageManager();
            System.out.println("ContactsCallLogContractTest all passed: " + sChecks + " checks");
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
