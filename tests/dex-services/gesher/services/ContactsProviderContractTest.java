/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.Manifest;
import android.app.ContentProviderHolder;
import android.app.GesherSettingsContext;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.IContentService;
import android.content.Intent;
import android.content.OperationApplicationException;
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
import android.os.IBinder;
import android.os.IServiceManager;
import android.os.Looper;
import android.os.Process;
import android.os.ServiceManager;
import android.permission.IPermissionChecker;
import android.permission.IPermissionManager;
import android.provider.ContactsContract;
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
 * Contract tests for the built-in SQLite-backed {@code content://com.android.contacts}
 * provider, covering {@link ContactsContract} schema and persistence, strict
 * {@code SQLiteQueryBuilder}, loose {@code phone_lookup} matching and
 * auto-{@code data4}, transactional {@link ContentProvider#applyBatch}, sync-adapter
 * soft/hard deletes, display-name fallback chain, AppOps/URI-grant enforcement, and
 * removal of the legacy {@code content://contacts} authority.
 */
public final class ContactsProviderContractTest {
    private static final String BUILT_IN_PKG = "android";
    private static final String PKG_NONE = "fixture.contacts.none";
    private static final String PKG_READ = "fixture.contacts.read";
    private static final String PKG_WRITE = "fixture.contacts.write";
    private static final String PKG_LEGACY = "fixture.contacts.legacy";
    private static final int READ_FLAG = Intent.FLAG_GRANT_READ_URI_PERMISSION;

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
        return observerRecordingContext(provider, notifications, null);
    }

    private static Context observerRecordingContext(ContentProvider provider, List<Uri> notifications,
            List<Boolean> syncRequests) {
        class TestContext extends ContextWrapper {
            ContentResolver resolver;
            TestContext() { super(GesherSettingsContext.get()); }
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
            @Override public void notifyChange(Uri uri, ContentObserver observer, boolean syncToNetwork) {
                check(!Thread.holdsLock(provider), "Observer notified under provider lock");
                check(Binder.getCallingUid() == Process.myUid(), "Observer notification leaked caller UID");
                notifications.add(uri);
                if (syncRequests != null) syncRequests.add(syncToNetwork);
            }
        };
        return ctx;
    }

    private static ContentProvider newContactsProvider(PackageManagerService pm,
            ActivityTaskManagerService atm, File dbFile) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.ContactsProvider");
        return (ContentProvider) construct(type,
                new Class<?>[] { PackageManagerService.class, ActivityTaskManagerService.class, File.class },
                pm, atm, dbFile);
    }

    private static void testContactsSchemaViewsStrictAndFallback(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts-schema.db");
        List<Uri> notifications = new ArrayList<>();
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo attachedInfo = (ProviderInfo) method(support, "contactsProviderInfo").invoke(null);

        ContentProvider provider = newContactsProvider(null, null, dbFile);
        provider.attachInfo(observerRecordingContext(provider, notifications), attachedInfo);
        try {
            // Default local directory seeded on clean database
            try (Cursor dir = provider.query(ContactsContract.Directory.CONTENT_URI,
                    new String[] { ContactsContract.Directory._ID, ContactsContract.Directory.PACKAGE_NAME },
                    null, null, null)) {
                check(dir != null && dir.getCount() == 1 && dir.moveToFirst()
                        && dir.getLong(0) == 0L && BUILT_IN_PKG.equals(dir.getString(1)),
                        "Default local Contacts directory seeded");
            }

            // Legacy content://contacts authority is removed / unsupported
            rejects(IllegalArgumentException.class,
                    () -> provider.getType(Uri.parse("content://contacts/people")));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(Uri.parse("content://contacts/people"), null, null, null, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(Uri.parse("content://contacts/phones"), null, null, null, null));

            // Strict SQLiteQueryBuilder rejects unknown columns in projection or selection
            rejects(IllegalArgumentException.class,
                    () -> provider.query(ContactsContract.Contacts.CONTENT_URI,
                            new String[] { "no_such_col" }, null, null, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(ContactsContract.Contacts.CONTENT_URI,
                            new String[] { ContactsContract.Contacts._ID }, "no_such_col = 1", null, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(ContactsContract.Data.CONTENT_URI,
                            new String[] { "bogus_col" }, null, null, null));

            // Insert two contacts for sort_key ordering and the public projections.
            ContentValues rc1 = new ContentValues();
            rc1.put(ContactsContract.RawContacts.ACCOUNT_TYPE, "local");
            rc1.put(ContactsContract.RawContacts.ACCOUNT_NAME, "device");
            Uri raw1Uri = provider.insert(ContactsContract.RawContacts.CONTENT_URI, rc1);
            long raw1 = ContentUris.parseId(raw1Uri);
            Uri rawEntity = Uri.withAppendedPath(raw1Uri, "entity");
            try (Cursor c = provider.query(rawEntity,
                    new String[] {"_id", "data_id"}, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && c.getLong(0) == raw1 && c.isNull(1),
                        "Raw entity retains a raw contact with no data rows");
            }
            Uri unknownDirectory = ContactsContract.Contacts.CONTENT_URI.buildUpon()
                    .appendQueryParameter(ContactsContract.DIRECTORY_PARAM_KEY, "424242").build();
            check(provider.query(unknownDirectory, null, null, null, null) == null,
                    "An absent directory does not return local contacts");

            // Display-name fallback chain exercise on raw1:
            // Email -> Phone -> Organization -> Nickname -> Composed StructuredName -> DisplayName
            ContentValues phoneCv = new ContentValues();
            phoneCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            phoneCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            phoneCv.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "+972 50-234-5678");
            phoneCv.put(ContactsContract.CommonDataKinds.Phone.TYPE, ContactsContract.CommonDataKinds.Phone.TYPE_MOBILE);
            Uri phoneUri = provider.insert(ContactsContract.Data.CONTENT_URI, phoneCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.DISPLAY_NAME_SOURCE,
                            ContactsContract.Contacts.HAS_PHONE_NUMBER, "sort_key", "phonebook_label" },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "+972 50-234-5678".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.PHONE
                        && c.getInt(2) == 1 && "+972 50-234-5678".equals(c.getString(3)),
                        "Display name falls back to Phone when higher sources absent");
            }
            // Verify auto-populated data4 normalized phone number
            try (Cursor c = provider.query(phoneUri,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "+972502345678".equals(c.getString(0)),
                        "Auto-computes data4 normalized_number on Phone insert");
            }
            try (Cursor c = provider.query(ContactsContract.RawContactsEntity.CONTENT_URI,
                    new String[] {"_id", "data_id"}, "_id = ?",
                    new String[] {Long.toString(raw1)}, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && c.getLong(0) == raw1 && c.getLong(1) == ContentUris.parseId(phoneUri),
                        "Raw contact entities expose distinct raw-contact and data IDs");
            }
            ContentValues group = new ContentValues();
            group.put(ContactsContract.Groups.TITLE, "Test group");
            long groupId = ContentUris.parseId(provider.insert(ContactsContract.Groups.CONTENT_URI, group));
            ContentValues member = new ContentValues();
            member.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            member.put(ContactsContract.Data.MIMETYPE,
                    ContactsContract.CommonDataKinds.GroupMembership.CONTENT_ITEM_TYPE);
            member.put(ContactsContract.CommonDataKinds.GroupMembership.GROUP_ROW_ID, groupId);
            provider.insert(ContactsContract.Data.CONTENT_URI, member);
            try (Cursor c = provider.query(ContactsContract.Groups.CONTENT_SUMMARY_URI,
                    new String[] {ContactsContract.Groups.SUMMARY_COUNT,
                            ContactsContract.Groups.SUMMARY_WITH_PHONES},
                    "_id = ?", new String[] {Long.toString(groupId)}, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getInt(1) == 1,
                        "Group summary counts real contact and phone memberships");
            }

            ContentValues emailCv = new ContentValues();
            emailCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            emailCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE);
            emailCv.put(ContactsContract.CommonDataKinds.Email.ADDRESS, "ada@example.org");
            Uri emailUri = provider.insert(ContactsContract.Data.CONTENT_URI, emailCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.DISPLAY_NAME_SOURCE },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "+972 50-234-5678".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.PHONE,
                        "Phone takes precedence over Email for display_name");
            }

            ContentValues orgCv = new ContentValues();
            orgCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            orgCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Organization.CONTENT_ITEM_TYPE);
            orgCv.put(ContactsContract.CommonDataKinds.Organization.COMPANY, "Analytical Engines");
            Uri orgUri = provider.insert(ContactsContract.Data.CONTENT_URI, orgCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.DISPLAY_NAME_SOURCE },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "Analytical Engines".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.ORGANIZATION,
                        "Organization takes precedence over Phone for display_name");
            }

            ContentValues nickCv = new ContentValues();
            nickCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            nickCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Nickname.CONTENT_ITEM_TYPE);
            nickCv.put(ContactsContract.CommonDataKinds.Nickname.NAME, "Byronic");
            Uri nickUri = provider.insert(ContactsContract.Data.CONTENT_URI, nickCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.DISPLAY_NAME_SOURCE },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "Byronic".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.NICKNAME,
                        "Nickname takes precedence over Organization");
            }

            // StructuredName composed from GIVEN_NAME + FAMILY_NAME when DISPLAY_NAME is omitted
            ContentValues structComposed = new ContentValues();
            structComposed.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            structComposed.put(ContactsContract.Data.MIMETYPE,
                    ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
            structComposed.put(ContactsContract.CommonDataKinds.StructuredName.GIVEN_NAME, "Ada");
            structComposed.put(ContactsContract.CommonDataKinds.StructuredName.FAMILY_NAME, "Lovelace");
            Uri nameUri = provider.insert(ContactsContract.Data.CONTENT_URI, structComposed);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME,
                            ContactsContract.Contacts.DISPLAY_NAME_ALTERNATIVE,
                            ContactsContract.Contacts.DISPLAY_NAME_SOURCE,
                            "sort_key", "phonebook_label", "phonebook_bucket" },
                    null, null, null)) {
                check(c != null && c.moveToFirst()
                        && "Ada Lovelace".equals(c.getString(0))
                        && "Lovelace, Ada".equals(c.getString(1))
                        && c.getInt(2) == ContactsContract.DisplayNameSources.STRUCTURED_NAME
                        && "Ada Lovelace".equals(c.getString(3))
                        && "A".equals(c.getString(4))
                        && c.getInt(5) > 0,
                        "Composed StructuredName populates display_name, alt, sort_key, phonebook_label");
            }

            // Explicit StructuredName.DISPLAY_NAME overrides composed parts, and deleting reverses fallback
            ContentValues rename = new ContentValues();
            rename.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "Augusta Ada");
            check(provider.update(nameUri, rename, null, null) == 1, "Update StructuredName DISPLAY_NAME");
            check(provider.delete(nameUri, null, null) == 1, "Delete StructuredName reverses fallback");
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME }, null, null, null)) {
                check(c != null && c.moveToFirst() && "Byronic".equals(c.getString(0)),
                        "Deleting StructuredName falls back to Nickname");
            }
            check(provider.delete(nickUri, null, null) == 1, "Delete Nickname");
            check(provider.delete(orgUri, null, null) == 1, "Delete Organization");
            check(provider.delete(emailUri, null, null) == 1, "Delete Email");
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME }, null, null, null)) {
                check(c != null && c.moveToFirst() && "+972 50-234-5678".equals(c.getString(0)),
                        "Deleting higher sources falls back down to Phone");
            }

            // Insert second contact with "Zed Smith" and test ORDER BY sort_key + PhoneLookup loose match
            ContentValues rc2 = new ContentValues();
            Uri raw2Uri = provider.insert(ContactsContract.RawContacts.CONTENT_URI, rc2);
            long raw2 = ContentUris.parseId(raw2Uri);
            ContentValues rc2Name = new ContentValues();
            rc2Name.put(ContactsContract.Data.RAW_CONTACT_ID, raw2);
            rc2Name.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
            rc2Name.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "Zed Smith");
            provider.insert(ContactsContract.Data.CONTENT_URI, rc2Name);
            ContentValues rc2Phone = new ContentValues();
            rc2Phone.put(ContactsContract.Data.RAW_CONTACT_ID, raw2);
            rc2Phone.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            rc2Phone.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "050-2345671");
            provider.insert(ContactsContract.Data.CONTENT_URI, rc2Phone);

            // Locale-aware sort and public columns on Contacts and Data.
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, "sort_key", "photo_file_id",
                            "times_used", "last_time_used", "contact_presence", "contact_status" },
                    null, null, "sort_key ASC")) {
                check(c != null && c.getCount() == 2 && c.moveToFirst()
                        && "+972 50-234-5678".equals(c.getString(0))
                        && c.moveToNext() && "Zed Smith".equals(c.getString(0)),
                        "ORDER BY sort_key works across the Contacts schema");
            }

            // phone_lookup loose matching: 0502345678 matches +972 50-234-5678; short "1" does NOT match 050-2345671
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("0502345678")),
                    new String[] { ContactsContract.PhoneLookup.DISPLAY_NAME,
                            ContactsContract.PhoneLookup.NORMALIZED_NUMBER },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && "+972502345678".equals(c.getString(1)),
                        "phone_lookup/0502345678 loosely matches +972 50-234-5678");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("1")),
                    new String[] { ContactsContract.PhoneLookup._ID }, null, null, null)) {
                check(c != null && c.getCount() == 0,
                        "phone_lookup/1 must not false-match 050-2345671");
            }
        } finally {
            provider.shutdown();
        }
    }

    private static void testFreshSchemaAndPersistence(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts-reopen.db");
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo info = (ProviderInfo) method(support, "contactsProviderInfo").invoke(null);
        long dataId;
        ContentProvider initial = newContactsProvider(null, null, dbFile);
        initial.attachInfo(observerRecordingContext(initial, new ArrayList<>()), info);
        try {
            Uri raw = initial.insert(ContactsContract.RawContacts.CONTENT_URI, new ContentValues());
            ContentValues phone = new ContentValues();
            phone.put(ContactsContract.Data.RAW_CONTACT_ID, ContentUris.parseId(raw));
            phone.put(ContactsContract.Data.MIMETYPE,
                    ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            phone.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "+972 50-234-5678");
            dataId = ContentUris.parseId(initial.insert(ContactsContract.Data.CONTENT_URI, phone));
        } finally {
            initial.shutdown();
        }
        try (SQLiteDatabase db = SQLiteDatabase.openDatabase(
                dbFile.getAbsolutePath(), null, SQLiteDatabase.OPEN_READONLY)) {
            check(db.getVersion() == 1, "Fresh Contacts schema starts at version 1");
        }
        ContentProvider reopened = newContactsProvider(null, null, dbFile);
        reopened.attachInfo(observerRecordingContext(reopened, new ArrayList<>()), info);
        try {
            Uri item = ContentUris.withAppendedId(ContactsContract.Data.CONTENT_URI, dataId);
            try (Cursor c = reopened.query(item,
                    new String[] {ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER},
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "+972502345678".equals(c.getString(0)),
                        "Contacts database preserves normalized data after reopen");
            }
        } finally {
            reopened.shutdown();
        }
    }

    private static void testApplyBatchAndSyncAdapterSemantics(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts-batch-sync.db");
        List<Uri> notifications = new ArrayList<>();
        List<Boolean> syncRequests = new ArrayList<>();
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo attachedInfo = (ProviderInfo) method(support, "contactsProviderInfo").invoke(null);
        ContentProvider provider = newContactsProvider(null, null, dbFile);
        provider.attachInfo(observerRecordingContext(provider, notifications, syncRequests), attachedInfo);
        try {
            // Successful applyBatch commits atomically and emits ONE coalesced notification
            ArrayList<ContentProviderOperation> okBatch = new ArrayList<>();
            Uri syncRawUri = ContactsContract.RawContacts.CONTENT_URI.buildUpon()
                    .appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true")
                    .build();
            okBatch.add(ContentProviderOperation.newInsert(syncRawUri)
                    .withValue(ContactsContract.RawContacts.ACCOUNT_TYPE, "com.example.sync")
                    .withValue(ContactsContract.RawContacts.ACCOUNT_NAME, "sync@example.org")
                    .withValue(ContactsContract.RawContacts.SOURCE_ID, "srv-42")
                    .build());
            okBatch.add(ContentProviderOperation.newInsert(ContactsContract.Data.CONTENT_URI.buildUpon()
                    .appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true").build())
                    .withValueBackReference(ContactsContract.Data.RAW_CONTACT_ID, 0)
                    .withValue(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE)
                    .withValue(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "Sync Alice")
                    .build());
            okBatch.add(ContentProviderOperation.newInsert(ContactsContract.Data.CONTENT_URI.buildUpon()
                    .appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true").build())
                    .withValueBackReference(ContactsContract.Data.RAW_CONTACT_ID, 0)
                    .withValue(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE)
                    .withValue(ContactsContract.CommonDataKinds.Phone.NUMBER, "052-9998877")
                    .build());
            ContentProviderResult[] res = provider.applyBatch(okBatch);
            check(res.length == 3 && res[0].uri != null && res[1].uri != null && res[2].uri != null,
                    "applyBatch commits 3 linked ops");
            check(notifications.size() == 1,
                    "applyBatch emits exactly 1 coalesced notification, got " + notifications.size());
            check(syncRequests.equals(List.of(false)), "Sync-only batch does not request network sync");

            long rawId = ContentUris.parseId(res[0].uri);
            try (Cursor c = provider.query(syncRawUri,
                    new String[] { ContactsContract.RawContacts._ID, ContactsContract.RawContacts.DIRTY,
                            ContactsContract.RawContacts.VERSION, ContactsContract.RawContacts.DELETED },
                    "_id = ?", new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.moveToFirst() && c.getInt(1) == 0 && c.getInt(2) == 1 && c.getInt(3) == 0,
                        "SyncAdapter insert leaves dirty=0, version=1, deleted=0");
            }

            // Failed applyBatch rolls back completely and emits ZERO notifications
            notifications.clear();
            ArrayList<ContentProviderOperation> failBatch = new ArrayList<>();
            failBatch.add(ContentProviderOperation.newInsert(ContactsContract.RawContacts.CONTENT_URI)
                    .withValue(ContactsContract.RawContacts.ACCOUNT_NAME, "should-rollback")
                    .withValue(ContactsContract.RawContacts.ACCOUNT_TYPE, "com.example.sync")
                    .build());
            // Op 1 lacks required mimetype -> IllegalArgumentException inside batch
            failBatch.add(ContentProviderOperation.newInsert(ContactsContract.Data.CONTENT_URI)
                    .withValueBackReference(ContactsContract.Data.RAW_CONTACT_ID, 0)
                    .build());
            rejects(IllegalArgumentException.class, () -> provider.applyBatch(failBatch));
            check(notifications.isEmpty(), "Failed applyBatch emits ZERO notifications");
            try (Cursor c = provider.query(ContactsContract.RawContacts.CONTENT_URI,
                    new String[] { ContactsContract.RawContacts._ID },
                    "account_name = ?", new String[] { "should-rollback" }, null)) {
                check(c != null && c.getCount() == 0, "Failed applyBatch rolls back earlier insert");
            }

            // Normal Data update sets dirty=1 and increments version
            Uri normalPhoneItem = ContentUris.withAppendedId(
                    ContactsContract.Data.CONTENT_URI, ContentUris.parseId(res[2].uri));
            ContentValues phoneUp = new ContentValues();
            phoneUp.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "+972 52-234-1111");
            String mimeSelection = ContactsContract.Data.RAW_CONTACT_ID + " = ? AND "
                    + ContactsContract.Data.MIMETYPE + " = ?";
            String[] phoneArgs = {Long.toString(rawId),
                    ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE};
            check(provider.update(ContactsContract.Data.CONTENT_URI, phoneUp,
                    mimeSelection, phoneArgs) == 1, "Data update accepts MIME and raw contact columns");
            try (Cursor c = provider.query(normalPhoneItem,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "+972522341111".equals(c.getString(0)),
                        "Update phone recomputes data4 normalized_number");
            }
            try (Cursor c = provider.query(syncRawUri,
                    new String[] { ContactsContract.RawContacts.DIRTY, ContactsContract.RawContacts.VERSION },
                    "_id = ?", new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getInt(1) == 2,
                        "Normal edit marks raw_contact dirty=1 and increments version");
            }

            // Delete through the same public Data projection on a separate local raw contact.
            Uri local = provider.insert(ContactsContract.RawContacts.CONTENT_URI, new ContentValues());
            long localId = ContentUris.parseId(local);
            ContentValues localPhone = new ContentValues(phoneUp);
            localPhone.put(ContactsContract.Data.RAW_CONTACT_ID, localId);
            localPhone.put(ContactsContract.Data.MIMETYPE,
                    ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            provider.insert(ContactsContract.Data.CONTENT_URI, localPhone);
            check(provider.delete(ContactsContract.Data.CONTENT_URI, mimeSelection,
                    new String[] {Long.toString(localId), phoneArgs[1]}) == 1,
                    "Data delete accepts MIME and raw contact columns");
            check(provider.delete(local, null, null) == 1, "Local raw contact is removed");
            try (Cursor c = provider.query(syncRawUri, null, "_id = ?",
                    new String[] {Long.toString(localId)}, null)) {
                check(c.getCount() == 0, "A local raw contact has no sync tombstone");
            }

            // Normal delete soft-deletes a raw contact that belongs to an account.
            Uri normalRawItem = ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId);
            check(provider.delete(normalRawItem, null, null) == 1, "Normal delete soft-deletes raw_contact");
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI, null, null, null, null)) {
                check(c != null && c.getCount() == 0, "Soft-deleted contact hidden from Contacts query");
            }
            try (Cursor c = provider.query(ContactsContract.RawContacts.CONTENT_URI, null, null, null, null)) {
                check(c != null && c.getCount() == 0, "Soft-deleted raw_contact hidden from normal RawContacts query");
            }
            try (Cursor c = provider.query(syncRawUri,
                    new String[] { ContactsContract.RawContacts.DELETED, ContactsContract.RawContacts.DIRTY,
                            ContactsContract.RawContacts.VERSION },
                    "_id = ?", new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getInt(1) == 1 && c.getInt(2) == 3,
                        "SyncAdapter sees deleted=1, dirty=1, version=3 after soft-delete");
            }
            try (Cursor c = provider.query(ContactsContract.DeletedContacts.CONTENT_URI,
                    new String[] { ContactsContract.DeletedContacts.CONTACT_ID }, null, null, null)) {
                check(c != null && c.getCount() == 2, "deleted_contacts records both removed aggregate contacts");
            }

            // Sync-adapter delete hard-deletes completely
            Uri syncRawItem = ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId)
                    .buildUpon().appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true").build();
            check(provider.delete(syncRawItem, null, null) == 1, "SyncAdapter delete hard-deletes raw_contact");
            try (Cursor c = provider.query(syncRawUri, null, "_id = ?",
                    new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.getCount() == 0, "Hard-deleted raw_contact removed completely");
            }

            Uri scopedAccount = syncRawUri.buildUpon()
                    .appendQueryParameter(ContactsContract.RawContacts.ACCOUNT_NAME, "scoped@example.org")
                    .appendQueryParameter(ContactsContract.RawContacts.ACCOUNT_TYPE, "com.example.sync")
                    .build();
            Uri otherDataSet = scopedAccount.buildUpon()
                    .appendQueryParameter(ContactsContract.RawContacts.DATA_SET, "other").build();
            Uri original = provider.insert(scopedAccount, new ContentValues());
            Uri preserved = provider.insert(otherDataSet, new ContentValues());
            Uri settings = ContactsContract.Settings.CONTENT_URI;
            Uri scopedSettings = settings.buildUpon()
                    .appendQueryParameter(ContactsContract.Settings.ACCOUNT_NAME, "scoped@example.org")
                    .appendQueryParameter(ContactsContract.Settings.ACCOUNT_TYPE, "com.example.sync")
                    .build();
            check(provider.insert(settings, new ContentValues()) == null,
                    "No explicit setting is inserted for the local account");
            ContentValues syncSetting = new ContentValues();
            syncSetting.put(ContactsContract.Settings.SHOULD_SYNC, 0);
            check(scopedSettings.equals(provider.insert(scopedSettings, syncSetting))
                    && scopedSettings.equals(provider.insert(scopedSettings, syncSetting)),
                    "Settings upsert returns a stable account URI");
            try (Cursor c = provider.query(scopedSettings,
                    new String[] {ContactsContract.Settings.ANY_UNSYNCED,
                            ContactsContract.Settings.UNGROUPED_COUNT}, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && c.getInt(0) == 1 && c.getInt(1) == 1,
                        "Settings upsert preserves one account row and derives its summaries");
            }
            syncSetting.put(ContactsContract.Settings.SHOULD_SYNC, 1);
            check(provider.update(scopedSettings, syncSetting, null, null) == 1,
                    "Settings update modifies the account row");
            check(provider.delete(scopedSettings, null, null) == 0,
                    "Settings cannot be removed while a raw contact references their account");
            try (Cursor c = provider.query(scopedAccount, new String[] {"_id"}, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && c.getLong(0) == ContentUris.parseId(original),
                        "Account parameters scope the query to the requested data set");
            }
            check(provider.delete(scopedAccount, null, null) == 1,
                    "Account-scoped delete removes only the requested data set");
            check(provider.delete(scopedSettings, null, null) == 1,
                    "Settings can be removed after the last account reference is deleted");
            try (Cursor c = provider.query(preserved, new String[] {"_id"}, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && c.getLong(0) == ContentUris.parseId(preserved),
                        "Account-scoped delete preserves the other data set");
            }
        } finally {
            provider.shutdown();
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
        p.setName(pkgName + ".HijackContacts");
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
    private static void testSecurityAppOpsUriGrantsAndPackageManager(File dbDir) throws Exception {
        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Unsafe unsafe = (Unsafe) unsafeField.get(null);

        ApkPackage appNone = fakeApp(unsafe, PKG_NONE, 13301, 36);
        ApkPackage appRead = fakeApp(unsafe, PKG_READ, 13302, 36, Manifest.permission.READ_CONTACTS);
        ApkPackage appWrite = fakeApp(unsafe, PKG_WRITE, 13303, 36,
                Manifest.permission.READ_CONTACTS, Manifest.permission.WRITE_CONTACTS);
        ApkPackage appLegacy = fakeApp(unsafe, PKG_LEGACY, 13304, 22, Manifest.permission.READ_CONTACTS);
        ApkPackage hijackBoot = conflictingAuthorityPkg(unsafe, "fixture.hijack.contacts", 13305, "com.android.contacts");

        PackageManagerService pm = (PackageManagerService) construct(PackageManagerService.class,
                new Class<?>[] { Collection.class, boolean.class },
                Arrays.asList(appNone, appRead, appWrite, appLegacy, hijackBoot), false);
        check(pm.get("fixture.hijack.contacts") == null,
                "PM constructor safely skips package declaring com.android.contacts authority");

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
            pm.grantRuntimePermission(PKG_READ, Manifest.permission.READ_CONTACTS, 0);
            pm.grantRuntimePermission(PKG_WRITE, Manifest.permission.READ_CONTACTS, 0);
            pm.grantRuntimePermission(PKG_WRITE, Manifest.permission.WRITE_CONTACTS, 0);
        });

        Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        List<Object> processes = (List<Object>) field(ActivityTaskManagerService.class, "mProcesses").get(atm);
        int[][] ids = { { 9301, 13301 }, { 9302, 13302 }, { 9303, 13303 }, { 9304, 13304 } };
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
        ContentProvider contactsProv = (ContentProvider) method(cpType, "contacts").invoke(providers);

        IContentObserver observer = new IContentObserver.Stub() {
            @Override public void onChange(boolean self, Uri uri, int user) { }
            @Override public void onChangeEtc(boolean self, Uri[] uris, int flags, int user) { }
        };

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
            testContactsSchemaViewsStrictAndFallback(dbDir);
            testFreshSchemaAndPersistence(dbDir);
            testApplyBatchAndSyncAdapterSemantics(dbDir);

            // Unprivileged package denied
            as(9301, 13301, () -> {
                rejects(SecurityException.class, () -> getProvider.invoke(
                        providers, procNone, PKG_NONE, "com.android.contacts"));
                rejects(SecurityException.class, () -> content.registerContentObserver(
                        ContactsContract.Contacts.CONTENT_URI, true, observer, 0, 36));
            });

            // Write app creates two contacts via IContentProvider
            Uri[] contactUris = new Uri[2];
            as(9303, 13303, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procWrite, PKG_WRITE, "com.android.contacts");
                check(h != null && h.provider != null && h.info != null
                        && Manifest.permission.READ_CONTACTS.equals(h.info.readPermission)
                        && Manifest.permission.WRITE_CONTACTS.equals(h.info.writePermission)
                        && h.info.grantUriPermissions,
                        "Write app acquires contacts holder with permissions and grantUriPermissions");
                for (int i = 0; i < 2; i++) {
                    Uri rawUri = h.provider.insert(source(13303, PKG_WRITE),
                            ContactsContract.RawContacts.CONTENT_URI, new ContentValues(), null);
                    long rawId = ContentUris.parseId(rawUri);
                    ContentValues nm = new ContentValues();
                    nm.put(ContactsContract.Data.RAW_CONTACT_ID, rawId);
                    nm.put(ContactsContract.Data.MIMETYPE,
                            ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
                    nm.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "Contact " + (i + 1));
                    h.provider.insert(source(13303, PKG_WRITE), ContactsContract.Data.CONTENT_URI, nm, null);
                    try (Cursor rc = h.provider.query(source(13303, PKG_WRITE),
                            ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId),
                            new String[] { ContactsContract.RawContacts.CONTACT_ID }, null, null)) {
                        check(rc != null && rc.moveToFirst(), "Lookup contact_id for raw_contact " + i);
                        contactUris[i] = ContentUris.withAppendedId(
                                ContactsContract.Contacts.CONTENT_URI, rc.getLong(0));
                    }
                }
            });

            // Read app queries via IContentProvider (with notification URI registration + close cleanup)
            as(9302, 13302, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procRead, PKG_READ, "com.android.contacts");
                try (Cursor c = h.provider.query(source(13302, PKG_READ),
                        ContactsContract.Contacts.CONTENT_URI,
                        new String[] { ContactsContract.Contacts._ID, ContactsContract.Contacts.DISPLAY_NAME },
                        null, null)) {
                    check(c != null && c.getCount() == 2,
                            "Read app queries contacts via IContentProvider cleanly");
                }
                rejects(SecurityException.class, () -> h.provider.insert(
                        source(13302, PKG_READ), ContactsContract.RawContacts.CONTENT_URI, new ContentValues(), null));
            });

            // Legacy targetSdk 22 app: allowed initially, denied by AppOps once FLAG_PERMISSION_REVOKED_COMPAT is set
            as(9304, 13304, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procLegacy, PKG_LEGACY, "com.android.contacts");
                try (Cursor c = h.provider.query(source(13304, PKG_LEGACY),
                        ContactsContract.Contacts.CONTENT_URI,
                        new String[] { ContactsContract.Contacts._ID }, null, null)) {
                    check(c != null && c.getCount() == 2, "Legacy targetSdk 22 app initially reads contacts");
                }
            });
            as(Process.myPid(), Process.SYSTEM_UID, () -> pm.permissions().updatePermissionFlags(
                    PKG_LEGACY, Manifest.permission.READ_CONTACTS,
                    PackageManager.FLAG_PERMISSION_REVOKED_COMPAT,
                    PackageManager.FLAG_PERMISSION_REVOKED_COMPAT, true, "default:0", 0));
            check(pm.permissions().checkPermission(PKG_LEGACY, Manifest.permission.READ_CONTACTS)
                            == PackageManager.PERMISSION_GRANTED,
                    "Legacy compat revocation preserves checkPermission == GRANTED");
            as(9304, 13304, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procLegacy, PKG_LEGACY, "com.android.contacts");
                try (Cursor c = h.provider.query(source(13304, PKG_LEGACY),
                        ContactsContract.Contacts.CONTENT_URI,
                        new String[] {ContactsContract.Contacts._ID}, null, null)) {
                    check(c != null && c.getCount() == 0,
                            "Revoked legacy AppOp returns a rejected query with no rows");
                }
            });

            // URI grant grants read access on contactUris[0] without READ_CONTACTS, denies contactUris[1]
            Uri grantedUri = contactUris[0];
            Uri ungrantedUri = contactUris[1];
            as(9302, 13302, () -> am.grantUriPermission(threadRead, PKG_NONE, grantedUri, READ_FLAG, 0));
            check((Boolean) canAccess.invoke(uriGrants, appNone, grantedUri, READ_FLAG, 0),
                    "UriGrants permits granted contact URI");
            check(!(Boolean) canAccess.invoke(uriGrants, appNone, ungrantedUri, READ_FLAG, 0),
                    "UriGrants denies ungranted contact URI");
            as(9301, 13301, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procNone, PKG_NONE, "com.android.contacts");
                check(h != null && h.provider != null, "URI-granted app acquires com.android.contacts holder");
                AttributionSource caller = source(13301, PKG_NONE);
                try (Cursor c = h.provider.query(caller, grantedUri,
                        new String[] {ContactsContract.Contacts.DISPLAY_NAME}, null, null)) {
                    check(c != null && c.getCount() == 1 && c.moveToFirst()
                            && "Contact 1".equals(c.getString(0)),
                            "URI-granted app reads granted contact row");
                }
                rejects(SecurityException.class, () -> h.provider.query(caller, ungrantedUri,
                        new String[] {ContactsContract.Contacts._ID}, null, null));
                rejects(SecurityException.class, () -> h.provider.query(caller,
                        ContactsContract.Contacts.CONTENT_URI,
                        new String[] {ContactsContract.Contacts._ID}, null, null));
                rejects(SecurityException.class, () -> h.provider.delete(caller, grantedUri, null));
            });
        } finally {
            contactsProv.shutdown();
            cache.clear();
            cache.putAll(previousCache);
            smField.set(null, previousSm);
        }

        // PackageManager resolution & legacy authority absence
        ProviderInfo contactsInfo = pm.resolveContentProvider("com.android.contacts", 0, 0);
        check(contactsInfo != null && BUILT_IN_PKG.equals(contactsInfo.packageName)
                && Manifest.permission.READ_CONTACTS.equals(contactsInfo.readPermission)
                && Manifest.permission.WRITE_CONTACTS.equals(contactsInfo.writePermission)
                && contactsInfo.grantUriPermissions,
                "PM resolves com.android.contacts with permissions and URI grants");
        check(pm.resolveContentProvider("contacts", 0, 0) == null,
                "PM must not resolve dropped legacy contacts authority");
        check(pm.resolveContentProvider("com.android.contacts", 0, 10) == null,
                "PM resolveContentProvider denies user 10");
        check(pm.getProviderInfo(new ComponentName(BUILT_IN_PKG,
                "com.android.providers.contacts.ContactsProvider2"), 0, 0) != null,
                "PM getProviderInfo resolves ContactsProvider2");
        PackageInfo builtInPkg = pm.getPackageInfo(BUILT_IN_PKG, PackageManager.GET_PROVIDERS, 0);
        check(builtInPkg != null && builtInPkg.providers != null
                && Arrays.stream(builtInPkg.providers).anyMatch(p -> "call_log".equals(p.authority))
                && Arrays.stream(builtInPkg.providers).anyMatch(p -> "com.android.contacts".equals(p.authority)),
                "Platform package exposes both ContactsProvider2 and CallLogProvider");

        check(Arrays.equals(pm.getPackagesForUid(Process.SYSTEM_UID),
                        new String[] {BUILT_IN_PKG}), "Contacts retains the platform UID package identity");

        // Admission & prepareAdd reject claiming com.android.contacts or legacy? Only com.android.contacts is system-owned
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        Method admit = method(support, "checkPackageAdmission", ApkPackage.class);
        ApkPackage hijack = conflictingAuthorityPkg(unsafe, "fixture.hijack.ct", 13398, "com.android.contacts");
        rejects(PackageParser.PackageParserException.class, () -> admit.invoke(null, hijack));
        Method prepareAdd = method(PackageManagerService.class, "prepareAdd", ApkPackage.class);
        rejects(IllegalArgumentException.class, () -> prepareAdd.invoke(pm, hijack));
    }

    public static void main(String[] args) throws Exception {
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        GesherSettingsContext.prepare();
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        String previousData = System.getenv("ANDROID_DATA");
        Path data = previousData == null
                ? Files.createTempDirectory("gesher-contacts-provider-contract-")
                : Files.createTempDirectory(Path.of(previousData), "gesher-contacts-provider-contract-");
        try {
            Os.setenv("ANDROID_DATA", data.toString(), true);
            testSecurityAppOpsUriGrantsAndPackageManager(data.toFile());
            System.out.println("ContactsProviderContractTest all passed: " + sChecks + " checks");
        } finally {
            if (previousData == null) Os.unsetenv("ANDROID_DATA");
            else Os.setenv("ANDROID_DATA", previousData, true);
            try (Stream<Path> walk = Files.walk(data)) {
                for (Path p : (Iterable<Path>) walk.sorted(Comparator.reverseOrder())::iterator) {
                    Files.deleteIfExists(p);
                }
            }
        }
    }
}
