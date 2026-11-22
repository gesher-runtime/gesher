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
 * provider, covering complete {@link ContactsContract} v2 schema and views, v1->v2 DB
 * upgrade, strict {@code SQLiteQueryBuilder}, loose {@code phone_lookup} matching and
 * auto-{@code data4}, transactional {@link ContentProvider#applyBatch}, sync-adapter
 * soft/hard deletes, display-name fallback chain, AppOps/URI-grant enforcement, and
 * removal of the legacy {@code content://contacts} authority.
 */
public final class ContactsProviderContractTest {
    private static final String BUILT_IN_PKG = "com.android.providers.contacts";
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

            // Insert two contacts for sort_key ordering and extended v2 columns
            ContentValues rc1 = new ContentValues();
            rc1.put(ContactsContract.RawContacts.ACCOUNT_TYPE, "local");
            rc1.put(ContactsContract.RawContacts.ACCOUNT_NAME, "device");
            Uri raw1Uri = provider.insert(ContactsContract.RawContacts.CONTENT_URI, rc1);
            long raw1 = ContentUris.parseId(raw1Uri);

            // Display-name fallback chain exercise on raw1:
            // Phone -> Email -> Organization -> Nickname -> Composed StructuredName -> DisplayName
            ContentValues phoneCv = new ContentValues();
            phoneCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            phoneCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            phoneCv.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "+972 50-123-4567");
            phoneCv.put(ContactsContract.CommonDataKinds.Phone.TYPE, ContactsContract.CommonDataKinds.Phone.TYPE_MOBILE);
            Uri phoneUri = provider.insert(ContactsContract.Data.CONTENT_URI, phoneCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.DISPLAY_NAME_SOURCE,
                            ContactsContract.Contacts.HAS_PHONE_NUMBER, "sort_key", "phonebook_label" },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "+972 50-123-4567".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.PHONE
                        && c.getInt(2) == 1 && "+972 50-123-4567".equals(c.getString(3)),
                        "Display name falls back to Phone when higher sources absent");
            }
            // Verify auto-populated data4 normalized phone number
            try (Cursor c = provider.query(phoneUri,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "+972501234567".equals(c.getString(0)),
                        "Auto-computes data4 normalized_number on Phone insert");
            }

            ContentValues emailCv = new ContentValues();
            emailCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            emailCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE);
            emailCv.put(ContactsContract.CommonDataKinds.Email.ADDRESS, "ada@example.org");
            Uri emailUri = provider.insert(ContactsContract.Data.CONTENT_URI, emailCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, ContactsContract.Contacts.DISPLAY_NAME_SOURCE },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "ada@example.org".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.EMAIL,
                        "Email takes precedence over Phone for display_name");
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
                        "Organization takes precedence over Email for display_name");
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
                        && c.getInt(5) == 1,
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
                check(c != null && c.moveToFirst() && "+972 50-123-4567".equals(c.getString(0)),
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
            rc2Phone.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "050-1234561");
            provider.insert(ContactsContract.Data.CONTENT_URI, rc2Phone);

            // ORDER BY sort_key & extended v2 columns on Contacts & Data
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, "sort_key", "photo_file_id",
                            "times_used", "last_time_used", "contact_presence", "contact_status" },
                    null, null, "sort_key ASC")) {
                check(c != null && c.getCount() == 2 && c.moveToFirst()
                        && "+972 50-123-4567".equals(c.getString(0))
                        && c.moveToNext() && "Zed Smith".equals(c.getString(0)),
                        "ORDER BY sort_key works across v2 Contacts schema");
            }

            // phone_lookup loose matching: 0501234567 matches +972 50-123-4567; short "1" does NOT match 050-1234561
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("0501234567")),
                    new String[] { ContactsContract.PhoneLookup.DISPLAY_NAME,
                            ContactsContract.PhoneLookup.NORMALIZED_NUMBER },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && "+972501234567".equals(c.getString(1)),
                        "phone_lookup/0501234567 loosely matches +972 50-123-4567");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("1")),
                    new String[] { ContactsContract.PhoneLookup._ID }, null, null, null)) {
                check(c != null && c.getCount() == 0,
                        "phone_lookup/1 must not false-match 050-1234561");
            }
        } finally {
            provider.shutdown();
        }
    }

    private static void testV1ToV2UpgradeMigration(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts-v1.db");
        try (SQLiteDatabase legacy = SQLiteDatabase.openOrCreateDatabase(dbFile, null)) {
            legacy.execSQL("CREATE TABLE contacts ("
                    + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    + "lookup TEXT, name_raw_contact_id INTEGER, photo_id INTEGER DEFAULT 0,"
                    + "photo_uri TEXT, photo_thumb_uri TEXT, display_name TEXT,"
                    + "display_name_alt TEXT, display_name_source INTEGER DEFAULT 0, phonetic_name TEXT,"
                    + "in_visible_group INTEGER NOT NULL DEFAULT 1, has_phone_number INTEGER NOT NULL DEFAULT 0,"
                    + "times_contacted INTEGER NOT NULL DEFAULT 0, last_time_contacted INTEGER,"
                    + "starred INTEGER NOT NULL DEFAULT 0, pinned INTEGER NOT NULL DEFAULT 0,"
                    + "custom_ringtone TEXT, send_to_voicemail INTEGER DEFAULT 0,"
                    + "contact_last_updated_timestamp INTEGER DEFAULT 0, is_user_profile INTEGER NOT NULL DEFAULT 0);");
            legacy.execSQL("CREATE TABLE raw_contacts ("
                    + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    + "contact_id INTEGER REFERENCES contacts(_id) ON DELETE SET NULL,"
                    + "account_name TEXT, account_type TEXT, data_set TEXT, sourceid TEXT,"
                    + "version INTEGER NOT NULL DEFAULT 1, dirty INTEGER NOT NULL DEFAULT 0,"
                    + "deleted INTEGER NOT NULL DEFAULT 0, aggregation_mode INTEGER NOT NULL DEFAULT 0,"
                    + "display_name TEXT, display_name_alt TEXT, display_name_source INTEGER DEFAULT 0,"
                    + "starred INTEGER NOT NULL DEFAULT 0, pinned INTEGER NOT NULL DEFAULT 0,"
                    + "custom_ringtone TEXT, send_to_voicemail INTEGER DEFAULT 0,"
                    + "times_contacted INTEGER NOT NULL DEFAULT 0, last_time_contacted INTEGER,"
                    + "sync1 TEXT, sync2 TEXT, sync3 TEXT, sync4 TEXT);");
            legacy.execSQL("CREATE TABLE mimetypes (_id INTEGER PRIMARY KEY AUTOINCREMENT, mimetype TEXT UNIQUE NOT NULL);");
            legacy.execSQL("CREATE TABLE data ("
                    + "_id INTEGER PRIMARY KEY AUTOINCREMENT, raw_contact_id INTEGER NOT NULL,"
                    + "mimetype_id INTEGER NOT NULL, is_primary INTEGER NOT NULL DEFAULT 0,"
                    + "is_super_primary INTEGER NOT NULL DEFAULT 0, data_version INTEGER NOT NULL DEFAULT 0,"
                    + "data1 TEXT, data2 TEXT, data3 TEXT, data4 TEXT, data5 TEXT, data6 TEXT, data7 TEXT,"
                    + "data8 TEXT, data9 TEXT, data10 TEXT, data11 TEXT, data12 TEXT, data13 TEXT, data14 TEXT,"
                    + "data15 BLOB, data_sync1 TEXT, data_sync2 TEXT, data_sync3 TEXT, data_sync4 TEXT,"
                    + "carrier_presence INTEGER NOT NULL DEFAULT 0);");
            legacy.execSQL("CREATE TABLE groups (_id INTEGER PRIMARY KEY AUTOINCREMENT, account_name TEXT,"
                    + "account_type TEXT, data_set TEXT, title TEXT, notes TEXT, system_id TEXT,"
                    + "deleted INTEGER NOT NULL DEFAULT 0, group_visible INTEGER NOT NULL DEFAULT 1,"
                    + "should_sync INTEGER NOT NULL DEFAULT 1, auto_add INTEGER NOT NULL DEFAULT 0,"
                    + "favorites INTEGER NOT NULL DEFAULT 0, group_is_read_only INTEGER NOT NULL DEFAULT 0,"
                    + "sync1 TEXT, sync2 TEXT, sync3 TEXT, sync4 TEXT);");
            legacy.execSQL("CREATE TABLE deleted_contacts (contact_id INTEGER PRIMARY KEY, contact_deleted_timestamp INTEGER NOT NULL DEFAULT 0);");
            legacy.execSQL("CREATE TABLE directories (_id INTEGER PRIMARY KEY, packageName TEXT, typeResourceId INTEGER,"
                    + "displayName TEXT, accountType TEXT, accountName TEXT, exportSupport INTEGER NOT NULL DEFAULT 0,"
                    + "shortcutSupport INTEGER NOT NULL DEFAULT 2, photoSupport INTEGER NOT NULL DEFAULT 3);");
            legacy.execSQL("CREATE TABLE settings (account_name TEXT, account_type TEXT, data_set TEXT,"
                    + "ungrouped_visible INTEGER NOT NULL DEFAULT 0, should_sync INTEGER NOT NULL DEFAULT 1,"
                    + "any_synced INTEGER NOT NULL DEFAULT 0, ungrouped_count INTEGER NOT NULL DEFAULT 0, ungrouped_with_phones INTEGER NOT NULL DEFAULT 0);");
            legacy.execSQL("CREATE VIEW view_data AS SELECT data._id AS _id FROM data;");

            legacy.execSQL("INSERT INTO mimetypes(_id, mimetype) VALUES(1, 'vnd.android.cursor.item/name');");
            legacy.execSQL("INSERT INTO mimetypes(_id, mimetype) VALUES(2, 'vnd.android.cursor.item/phone_v2');");
            legacy.execSQL("INSERT INTO contacts(_id, lookup, name_raw_contact_id, display_name) VALUES(1, '0r1', 1, 'Migrated Grace');");
            legacy.execSQL("INSERT INTO raw_contacts(_id, contact_id, display_name) VALUES(1, 1, 'Migrated Grace');");
            legacy.execSQL("INSERT INTO data(_id, raw_contact_id, mimetype_id, data1) VALUES(1, 1, 1, 'Migrated Grace');");
            legacy.execSQL("INSERT INTO data(_id, raw_contact_id, mimetype_id, data1) VALUES(2, 1, 2, '050-5551234');");
            legacy.setVersion(1);
        }

        ContentProvider provider = newContactsProvider(null, null, dbFile);
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo attachedInfo = (ProviderInfo) method(support, "contactsProviderInfo").invoke(null);
        provider.attachInfo(observerRecordingContext(provider, new ArrayList<>()), attachedInfo);
        try {
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME, "sort_key", "phonebook_label",
                            "times_used", "photo_file_id" },
                    null, null, "sort_key ASC")) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && "Migrated Grace".equals(c.getString(0))
                        && "Migrated Grace".equals(c.getString(1))
                        && "M".equals(c.getString(2))
                        && c.getInt(3) == 0,
                        "v1->v2 Contacts migration preserves rows and backfills sort_key/phonebook_label");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("+972505551234")),
                    new String[] { ContactsContract.PhoneLookup.DISPLAY_NAME, ContactsContract.PhoneLookup.NORMALIZED_NUMBER },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && "Migrated Grace".equals(c.getString(0))
                        && "0505551234".equals(c.getString(1)),
                        "v1->v2 upgrade backfills data4 normalized_number and recreates view_phone_lookup");
            }
        } finally {
            provider.shutdown();
        }
    }

    private static void testApplyBatchAndSyncAdapterSemantics(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts-batch-sync.db");
        List<Uri> notifications = new ArrayList<>();
        Class<?> support = Class.forName("com.gesher.server.ContactsProviderSupport");
        ProviderInfo attachedInfo = (ProviderInfo) method(support, "contactsProviderInfo").invoke(null);
        ContentProvider provider = newContactsProvider(null, null, dbFile);
        provider.attachInfo(observerRecordingContext(provider, notifications), attachedInfo);
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
            phoneUp.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "052-0001111");
            check(provider.update(normalPhoneItem, phoneUp, null, null) == 1, "Normal phone update");
            try (Cursor c = provider.query(normalPhoneItem,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "0520001111".equals(c.getString(0)),
                        "Update phone recomputes data4 normalized_number");
            }
            try (Cursor c = provider.query(syncRawUri,
                    new String[] { ContactsContract.RawContacts.DIRTY, ContactsContract.RawContacts.VERSION },
                    "_id = ?", new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getInt(1) == 2,
                        "Normal edit marks raw_contact dirty=1 and increments version");
            }

            // Normal delete soft-deletes (deleted=1, dirty=1, version++, deleted_contacts row, hidden by default)
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
                check(c != null && c.getCount() == 1, "deleted_contacts records soft-deleted contact_id");
            }

            // Sync-adapter delete hard-deletes completely
            Uri syncRawItem = ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId)
                    .buildUpon().appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true").build();
            check(provider.delete(syncRawItem, null, null) == 1, "SyncAdapter delete hard-deletes raw_contact");
            try (Cursor c = provider.query(syncRawUri, null, "_id = ?",
                    new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.getCount() == 0, "Hard-deleted raw_contact removed completely");
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
    private static void testSecurityAppOpsUriGrantsAndPackageManager() throws Exception {
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
            // Unprivileged package denied
            as(9301, 13301, () -> {
                rejects(SecurityException.class, () -> getProvider.invoke(
                        providers, procNone, PKG_NONE, "com.android.contacts"));
                rejects(SecurityException.class, () -> content.registerContentObserver(
                        ContactsContract.Contacts.CONTENT_URI, true, observer, 0, 36));
                rejects(SecurityException.class, () -> contactsProv.query(
                        ContactsContract.Contacts.CONTENT_URI, null, null, null, null));
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
                rejects(SecurityException.class, () -> contactsProv.query(
                        ContactsContract.Contacts.CONTENT_URI, new String[] { ContactsContract.Contacts._ID },
                        null, null, null));
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procLegacy, PKG_LEGACY, "com.android.contacts");
                rejects(SecurityException.class, () -> h.provider.query(source(13304, PKG_LEGACY),
                        ContactsContract.Contacts.CONTENT_URI,
                        new String[] { ContactsContract.Contacts._ID }, null, null));
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
                try (Cursor c = contactsProv.query(grantedUri,
                        new String[] { ContactsContract.Contacts.DISPLAY_NAME }, null, null, null)) {
                    check(c != null && c.getCount() == 1 && c.moveToFirst()
                            && "Contact 1".equals(c.getString(0)),
                            "URI-granted app reads granted contact row");
                }
                rejects(SecurityException.class, () -> contactsProv.query(ungrantedUri,
                        new String[] { ContactsContract.Contacts._ID }, null, null, null));
                rejects(SecurityException.class, () -> contactsProv.query(
                        ContactsContract.Contacts.CONTENT_URI,
                        new String[] { ContactsContract.Contacts._ID }, null, null, null));
                rejects(SecurityException.class, () -> contactsProv.delete(grantedUri, null, null));
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
        check(builtInPkg != null && builtInPkg.versionName != null && !builtInPkg.versionName.isEmpty()
                && builtInPkg.providers != null && builtInPkg.providers.length == 2,
                "PM exposes both ContactsProvider2 and CallLogProvider with non-null versionName");

        for (Object item : pm.getInstalledPackages(0, 0).getList()) {
            PackageInfo installed = (PackageInfo) item;
            check(!BUILT_IN_PKG.equals(installed.packageName),
                    "getInstalledPackages must not include synthetic " + BUILT_IN_PKG);
        }

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
        String previousSpoof = System.getenv("GESHER_MICROG_SPOOF");
        Path data = Files.createTempDirectory("gesher-contacts-provider-contract-");
        try {
            Os.setenv("ANDROID_DATA", data.toString(), true);
            Os.setenv("GESHER_MICROG_SPOOF", "1", true);
            testContactsSchemaViewsStrictAndFallback(data.toFile());
            testV1ToV2UpgradeMigration(data.toFile());
            testApplyBatchAndSyncAdapterSemantics(data.toFile());
            testSecurityAppOpsUriGrantsAndPackageManager();
            System.out.println("ContactsProviderContractTest all passed: " + sChecks + " checks");
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
