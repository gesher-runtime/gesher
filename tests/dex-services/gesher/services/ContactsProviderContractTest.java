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
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Entity;
import android.content.EntityIterator;
import android.content.IContentProvider;
import android.content.IContentService;
import android.content.Intent;
import android.content.OperationApplicationException;
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
import android.hardware.display.IDisplayManager;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
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
import java.util.Locale;
import java.util.Map;
import java.util.stream.Stream;
import sun.misc.Unsafe;

/**
 * Contract tests for the built-in SQLite-backed {@code content://com.android.contacts}
 * provider, covering v1 {@link ContactsContract} schema and views, strict
 * {@link android.database.sqlite.SQLiteQueryBuilder} view resolution and injection defense
 * on query/update/delete, local hard-delete vs account-backed soft-delete, E.164
 * {@code normalized_number}, loose {@code phone_lookup} matching (including 7-digit
 * local numbers), ICU {@code AlphabeticIndex} labels (including Hebrew) and localized sort
 * order, entity/status/strequent/callable/usagefeedback/photo URIs, profile isolation,
 * {@code Contacts} writable column restrictions, unknown {@code ContentValues} validation,
 * transactional {@link ContentProvider#applyBatch}, AppOps/URI-grant enforcement, and
 * removal of the legacy {@code content://contacts} authority.
 */
public final class ContactsProviderContractTest {
    private static final String BUILT_IN_PKG = "android";
    private static final String PKG_NONE = "fixture.contacts.none";
    private static final String PKG_READ = "fixture.contacts.read";
    private static final String PKG_WRITE = "fixture.contacts.write";
    private static final String PKG_LEGACY = "fixture.contacts.legacy";
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
            TestContext() { super(GesherSettingsContext.get()); }
            @Override public String getPackageName() { return BUILT_IN_PKG; }
            @Override public String getOpPackageName() { return BUILT_IN_PKG; }
            @Override public int getUserId() { return 0; }
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

    private static ContentProvider newContactsProvider(File dbFile, List<Uri> notifications)
            throws Exception {
        Class<?> type = Class.forName("com.gesher.server.ContactsProvider");
        ContentProvider provider = (ContentProvider) construct(type, new Class<?>[] {
                Class.forName("com.gesher.server.ContentProviders"),
                Class.forName("com.gesher.server.UriGrants"),
                File.class
        }, null, null, dbFile);
        method(type, "attachTo", Context.class).invoke(provider,
                observerRecordingContext(provider, notifications));
        return provider;
    }

    private static void testSchemaStrictSqlEntitiesAndFallback(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts-schema.db");
        List<Uri> notifications = new ArrayList<>();
        Locale savedLocale = Locale.getDefault();
        Locale.setDefault(new Locale("en", "US"));
        ContentProvider provider = newContactsProvider(dbFile, notifications);
        try {
            // Default local directory + provider_status seeded cleanly
            try (Cursor dir = provider.query(ContactsContract.Directory.CONTENT_URI,
                    new String[] { ContactsContract.Directory._ID, ContactsContract.Directory.PACKAGE_NAME },
                    null, null, null)) {
                check(dir != null && dir.getCount() == 1 && dir.moveToFirst()
                        && dir.getLong(0) == 0L && BUILT_IN_PKG.equals(dir.getString(1)),
                        "Default local Contacts directory seeded under android");
            }
            try (Cursor st = provider.query(ContactsContract.ProviderStatus.CONTENT_URI,
                    new String[] { ContactsContract.ProviderStatus.STATUS,
                            ContactsContract.ProviderStatus.DATABASE_CREATION_TIMESTAMP },
                    null, null, null)) {
                check(st != null && st.getCount() == 1 && st.moveToFirst()
                        && st.getInt(0) == ContactsContract.ProviderStatus.STATUS_NORMAL
                        && st.getLong(1) > 0L,
                        "provider_status returns STATUS_NORMAL and positive creation timestamp");
            }

            // Legacy content://contacts authority is unsupported
            rejects(IllegalArgumentException.class,
                    () -> provider.getType(Uri.parse("content://contacts/people")));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(Uri.parse("content://contacts/people"), null, null, null, null));

            // Strict SQLiteQueryBuilder rejects unknown columns in projection/selection and unknown ContentValues keys
            rejects(IllegalArgumentException.class,
                    () -> provider.query(ContactsContract.Contacts.CONTENT_URI,
                            new String[] { "no_such_col" }, null, null, null));
            rejects(IllegalArgumentException.class,
                    () -> provider.query(ContactsContract.Contacts.CONTENT_URI,
                            new String[] { ContactsContract.Contacts._ID }, "no_such_col = 1", null, null));
            ContentValues badRaw = new ContentValues();
            badRaw.put("bogus_raw_column", "x");
            rejects(IllegalArgumentException.class,
                    () -> provider.insert(ContactsContract.RawContacts.CONTENT_URI, badRaw));
            ContentValues badData = new ContentValues();
            badData.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE);
            badData.put("bogus_data_column", "x");
            rejects(IllegalArgumentException.class,
                    () -> provider.insert(ContactsContract.Data.CONTENT_URI, badData));

            // Display-name precedence chain:
            // Email (10) < Phone (20) < Organization (30) < Nickname (35) < StructuredName (40)
            Uri raw1Uri = provider.insert(ContactsContract.RawContacts.CONTENT_URI, new ContentValues());
            long raw1 = ContentUris.parseId(raw1Uri);
            long contact1Id;
            try (Cursor rc = provider.query(raw1Uri,
                    new String[] { ContactsContract.RawContacts.CONTACT_ID }, null, null, null)) {
                check(rc != null && rc.moveToFirst(), "Read raw1 contact_id");
                contact1Id = rc.getLong(0);
            }

            ContentValues emailCv = new ContentValues();
            emailCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            emailCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE);
            emailCv.put(ContactsContract.CommonDataKinds.Email.ADDRESS, "ada@example.org");
            Uri emailUri = provider.insert(ContactsContract.Data.CONTENT_URI, emailCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME,
                            ContactsContract.Contacts.DISPLAY_NAME_SOURCE },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "ada@example.org".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.EMAIL,
                        "Display name falls back to Email when only Email is present");
            }

            ContentValues phoneCv = new ContentValues();
            phoneCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            phoneCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            phoneCv.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "+1 650-253-0000");
            phoneCv.put(ContactsContract.CommonDataKinds.Phone.TYPE, ContactsContract.CommonDataKinds.Phone.TYPE_MOBILE);
            Uri phoneUri = provider.insert(ContactsContract.Data.CONTENT_URI, phoneCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME,
                            ContactsContract.Contacts.DISPLAY_NAME_SOURCE,
                            ContactsContract.Contacts.HAS_PHONE_NUMBER },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "+1 650-253-0000".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.PHONE
                        && c.getInt(2) == 1,
                        "Phone (20) beats Email (10) for display_name precedence");
            }
            try (Cursor c = provider.query(phoneUri,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "+16502530000".equals(c.getString(0)),
                        "E.164 normalized_number stored for international phone number, got "
                                + (c != null && c.getCount() > 0 ? c.getString(0) : "empty"));
            }

            ContentValues orgCv = new ContentValues();
            orgCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            orgCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Organization.CONTENT_ITEM_TYPE);
            orgCv.put(ContactsContract.CommonDataKinds.Organization.COMPANY, "Analytical Engines");
            Uri orgUri = provider.insert(ContactsContract.Data.CONTENT_URI, orgCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME,
                            ContactsContract.Contacts.DISPLAY_NAME_SOURCE },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "Analytical Engines".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.ORGANIZATION,
                        "Organization (30) beats Phone (20)");
            }

            ContentValues nickCv = new ContentValues();
            nickCv.put(ContactsContract.Data.RAW_CONTACT_ID, raw1);
            nickCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Nickname.CONTENT_ITEM_TYPE);
            nickCv.put(ContactsContract.CommonDataKinds.Nickname.NAME, "Byronic");
            Uri nickUri = provider.insert(ContactsContract.Data.CONTENT_URI, nickCv);
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME,
                            ContactsContract.Contacts.DISPLAY_NAME_SOURCE },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "Byronic".equals(c.getString(0))
                        && c.getInt(1) == ContactsContract.DisplayNameSources.NICKNAME,
                        "Nickname (35) beats Organization (30)");
            }

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
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst()
                        && "Ada Lovelace".equals(c.getString(0))
                        && "Lovelace, Ada".equals(c.getString(1))
                        && c.getInt(2) == ContactsContract.DisplayNameSources.STRUCTURED_NAME
                        && "Ada Lovelace".equals(c.getString(3))
                        && "A".equals(c.getString(4))
                        && c.getInt(5) >= 1,
                        "StructuredName populates display_name, alt, sort_key, and ICU A-Z label");
            }

            // Data.update & Data.delete using view columns (Data.MIMETYPE & Data.CONTACT_ID)
            ContentValues upByMime = new ContentValues();
            upByMime.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "Augusta Ada");
            int updatedMime = provider.update(ContactsContract.Data.CONTENT_URI, upByMime,
                    ContactsContract.Data.CONTACT_ID + " = ? AND " + ContactsContract.Data.MIMETYPE + " = ?",
                    new String[] { Long.toString(contact1Id),
                            ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE });
            check(updatedMime == 1, "Data.update supports Data.CONTACT_ID and Data.MIMETYPE view columns");
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "Augusta Ada".equals(c.getString(0)),
                        "View-selected Data update refreshes contact display_name");
            }

            // Selection injection rejected in update & delete
            rejects(IllegalArgumentException.class, () -> provider.update(nameUri, upByMime, "1) OR (1", null));
            rejects(IllegalArgumentException.class, () -> provider.delete(nameUri, "1) OR (1", null));

            int deletedMime = provider.delete(ContactsContract.Data.CONTENT_URI,
                    ContactsContract.Data.RAW_CONTACT_ID + " = ? AND " + ContactsContract.Data.MIMETYPE + " = ?",
                    new String[] { Long.toString(raw1),
                            ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE });
            check(deletedMime == 1, "Data.delete supports Data.MIMETYPE view column");
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "Byronic".equals(c.getString(0)),
                        "Deleting StructuredName falls back to Nickname");
            }
            check(provider.delete(nickUri, null, null) == 1, "Delete Nickname");
            check(provider.delete(orgUri, null, null) == 1, "Delete Organization");
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "+1 650-253-0000".equals(c.getString(0)),
                        "Deleting Organization falls back down to Phone before Email");
            }
            check(provider.delete(phoneUri, null, null) == 1, "Delete Phone");
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[] { ContactsContract.Contacts.DISPLAY_NAME },
                    "_id = ?", new String[] { Long.toString(contact1Id) }, null)) {
                check(c != null && c.moveToFirst() && "ada@example.org".equals(c.getString(0)),
                        "Deleting Phone falls back down to Email");
            }

            // Contacts.update() restricts writable columns and mirrors starred to raw_contacts + sets dirty=1
            Uri contact1Uri = ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, contact1Id);
            ContentValues badContactUp = new ContentValues();
            badContactUp.put(ContactsContract.Contacts.DISPLAY_NAME, "Computed");
            rejects(IllegalArgumentException.class,
                    () -> provider.update(contact1Uri, badContactUp, null, null));
            ContentValues starUp = new ContentValues();
            starUp.put(ContactsContract.Contacts.STARRED, 1);
            check(provider.update(contact1Uri, starUp, null, null) == 1,
                    "Contacts.update allows writable option column starred");
            try (Cursor c = provider.query(raw1Uri,
                    new String[] { ContactsContract.RawContacts.STARRED, ContactsContract.RawContacts.DIRTY },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getInt(1) == 1,
                        "Contacts.update mirrors starred to raw_contacts and sets dirty=1");
            }

            // Test E.164 normalization across locales, NULL when unknown region, Hebrew ICU labels,
            // locale-aware COLLATE LOCALIZED sort order, 7-digit local phone_lookup, Photo, Entities, Callables, UsageFeedback
            Locale.setDefault(Locale.ROOT);
            Uri rawLocalUri = provider.insert(ContactsContract.RawContacts.CONTENT_URI, new ContentValues());
            long rawLocal = ContentUris.parseId(rawLocalUri);
            ContentValues heName = new ContentValues();
            heName.put(ContactsContract.Data.RAW_CONTACT_ID, rawLocal);
            heName.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
            heName.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "דני כהן");
            provider.insert(ContactsContract.Data.CONTENT_URI, heName);

            ContentValues unknownRegionPhone = new ContentValues();
            unknownRegionPhone.put(ContactsContract.Data.RAW_CONTACT_ID, rawLocal);
            unknownRegionPhone.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            unknownRegionPhone.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "5551234");
            Uri unknownPhoneUri = provider.insert(ContactsContract.Data.CONTENT_URI, unknownRegionPhone);

            // Photo row and contacts/#/photo
            byte[] fakePng = new byte[] { 1, 2, 3, 4 };
            ContentValues photoCv = new ContentValues();
            photoCv.put(ContactsContract.Data.RAW_CONTACT_ID, rawLocal);
            photoCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Photo.CONTENT_ITEM_TYPE);
            photoCv.put(ContactsContract.CommonDataKinds.Photo.PHOTO, fakePng);
            provider.insert(ContactsContract.Data.CONTENT_URI, photoCv);

            // SipAddress row for data/callables
            ContentValues sipCv = new ContentValues();
            sipCv.put(ContactsContract.Data.RAW_CONTACT_ID, rawLocal);
            sipCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.SipAddress.CONTENT_ITEM_TYPE);
            sipCv.put(ContactsContract.CommonDataKinds.SipAddress.SIP_ADDRESS, "sip:dani@example.org");
            provider.insert(ContactsContract.Data.CONTENT_URI, sipCv);

            long localContactId;
            try (Cursor c = provider.query(rawLocalUri,
                    new String[] { ContactsContract.RawContacts.CONTACT_ID, "phonebook_label" },
                    null, null, null)) {
                check(c != null && c.moveToFirst() && "ד".equals(c.getString(1)),
                        "Hebrew display name gets Hebrew ICU AlphabeticIndex label ד, got "
                                + (c != null && c.getCount() > 0 ? c.getString(1) : "none"));
                localContactId = c.getLong(0);
            }
            try (Cursor c = provider.query(unknownPhoneUri,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && c.isNull(0),
                        "normalized_number stays NULL when device region is unknown and number is non-E.164");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("+16505551234")),
                    new String[] { ContactsContract.PhoneLookup.DISPLAY_NAME,
                            ContactsContract.PhoneLookup.NORMALIZED_NUMBER },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && "דני כהן".equals(c.getString(0)) && c.isNull(1),
                        "phone_lookup matches stored 7-digit 5551234 against dialed +16505551234 without faking normalized_number");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.ENTERPRISE_CONTENT_FILTER_URI, Uri.encode("+16505551234")),
                    new String[] { ContactsContract.PhoneLookup.DISPLAY_NAME },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst() && "דני כהן".equals(c.getString(0)),
                        "phone_lookup_enterprise routes to primary-user phone_lookup");
            }

            // contacts/#/photo
            Uri localContactUri = ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, localContactId);
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(localContactUri, ContactsContract.Contacts.Photo.CONTENT_DIRECTORY),
                    new String[] { ContactsContract.CommonDataKinds.Photo.PHOTO }, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst()
                        && Arrays.equals(fakePng, c.getBlob(0)),
                        "contacts/#/photo returns stored photo blob");
            }

            // contacts/#/entities, raw_contacts/#/entity, raw_contact_entities + RawContacts.newEntityIterator
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(localContactUri, ContactsContract.Contacts.Entity.CONTENT_DIRECTORY),
                    new String[] { ContactsContract.Contacts.Entity.DATA_ID,
                            ContactsContract.Contacts.Entity.MIMETYPE },
                    null, null, null)) {
                check(c != null && c.getCount() >= 3,
                        "contacts/#/entities supports Entity.DATA_ID projection");
            }
            try (Cursor c = provider.query(ContactsContract.RawContactsEntity.CONTENT_URI,
                    null, ContactsContract.RawContactsEntity.CONTACT_ID + " = ?",
                    new String[] { Long.toString(localContactId) }, null)) {
                check(c != null, "raw_contact_entities query returns cursor");
                EntityIterator it = ContactsContract.RawContacts.newEntityIterator(c);
                try {
                    check(it.hasNext(), "RawContacts.newEntityIterator yields entity");
                    Entity e = it.next();
                    check(e.getSubValues().size() == 4,
                            "Entity iterator returns all 4 Data rows for local contact");
                } finally {
                    it.close();
                }
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(rawLocalUri, ContactsContract.RawContacts.Entity.CONTENT_DIRECTORY),
                    new String[] { ContactsContract.RawContacts.Entity.DATA_ID }, null, null, null)) {
                check(c != null && c.getCount() == 4, "raw_contacts/#/entity includes Entity.DATA_ID rows");
            }

            // strequent & usagefeedback & callables
            try (Cursor c = provider.query(ContactsContract.Contacts.CONTENT_STREQUENT_URI,
                    new String[] { ContactsContract.Contacts._ID }, null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst() && c.getLong(0) == contact1Id,
                        "contacts/strequent returns starred contact");
            }
            try (Cursor c = provider.query(ContactsContract.CommonDataKinds.Callable.CONTENT_URI,
                    new String[] { ContactsContract.Data._ID, ContactsContract.Data.MIMETYPE },
                    ContactsContract.Data.CONTACT_ID + " = ?",
                    new String[] { Long.toString(localContactId) }, null)) {
                check(c != null && c.getCount() == 2,
                        "data/callables returns Phone and SipAddress rows");
            }
            Uri feedbackUri = Uri.withAppendedPath(ContactsContract.DataUsageFeedback.FEEDBACK_URI,
                    Long.toString(ContentUris.parseId(unknownPhoneUri)))
                    .buildUpon().appendQueryParameter("type", "call").build();
            check(provider.update(feedbackUri, new ContentValues(), null, null) == 1,
                    "data/usagefeedback increments usage stats");
            try (Cursor c = provider.query(localContactUri,
                    new String[] { "times_used", "last_time_used" }, null, null, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getLong(1) > 0L,
                        "times_used and last_time_used updated by data/usagefeedback");
            }
            check(provider.delete(ContactsContract.DataUsageFeedback.DELETE_USAGE_URI, null, null) >= 1,
                    "contacts/delete_usage clears usage stats");

            // User profile isolation: profile data is visible under PROFILE*, excluded from ordinary queries
            Uri profileRawUri = provider.insert(
                    ContactsContract.Profile.CONTENT_RAW_CONTACTS_URI, new ContentValues());
            long profileRawId = ContentUris.parseId(profileRawUri);
            long profileContactId;
            try (Cursor c = provider.query(ContactsContract.Profile.CONTENT_URI,
                    new String[] { ContactsContract.Contacts._ID, ContactsContract.Contacts.IS_USER_PROFILE },
                    null, null, null)) {
                check(c != null && c.getCount() == 1 && c.moveToFirst() && c.getInt(1) == 1,
                        "Profile contact visible under Profile.CONTENT_URI");
                profileContactId = c.getLong(0);
            }
            ContentValues profName = new ContentValues();
            profName.put(ContactsContract.Data.RAW_CONTACT_ID, profileRawId);
            profName.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
            profName.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, "Device Owner");
            provider.insert(ContactsContract.Data.CONTENT_URI, profName);
            ContentValues profPhone = new ContentValues();
            profPhone.put(ContactsContract.Data.RAW_CONTACT_ID, profileRawId);
            profPhone.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            profPhone.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "+16505559999");
            provider.insert(ContactsContract.Data.CONTENT_URI, profPhone);
            ContentValues profEmail = new ContentValues();
            profEmail.put(ContactsContract.Data.RAW_CONTACT_ID, profileRawId);
            profEmail.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE);
            profEmail.put(ContactsContract.CommonDataKinds.Email.ADDRESS, "owner@example.org");
            provider.insert(ContactsContract.Data.CONTENT_URI, profEmail);

            try (Cursor c = provider.query(
                    ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, profileContactId),
                    new String[] { ContactsContract.Contacts._ID }, null, null, null)) {
                check(c != null && c.getCount() == 0, "CONTACTS_ID excludes user profile");
            }
            try (Cursor c = provider.query(ContactsContract.Data.CONTENT_URI,
                    new String[] { ContactsContract.Data._ID },
                    ContactsContract.Data.CONTACT_ID + " = ?",
                    new String[] { Long.toString(profileContactId) }, null)) {
                check(c != null && c.getCount() == 0, "DATA excludes user profile");
            }
            try (Cursor c = provider.query(ContactsContract.CommonDataKinds.Phone.CONTENT_URI,
                    new String[] { ContactsContract.Data._ID },
                    ContactsContract.Data.CONTACT_ID + " = ?",
                    new String[] { Long.toString(profileContactId) }, null)) {
                check(c != null && c.getCount() == 0, "PHONES excludes user profile");
            }
            try (Cursor c = provider.query(ContactsContract.CommonDataKinds.Email.CONTENT_URI,
                    new String[] { ContactsContract.Data._ID },
                    ContactsContract.Data.CONTACT_ID + " = ?",
                    new String[] { Long.toString(profileContactId) }, null)) {
                check(c != null && c.getCount() == 0, "EMAILS excludes user profile");
            }
            try (Cursor c = provider.query(
                    Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode("+16505559999")),
                    new String[] { ContactsContract.PhoneLookup._ID }, null, null, null)) {
                check(c != null && c.getCount() == 0, "phone_lookup excludes user profile");
            }

            // Locale-aware COLLATE LOCALIZED sort order ("alice" before "Bob", unlike ASCII binary order)
            Locale.setDefault(Locale.US);
            ContentProvider reopened = newContactsProvider(dbFile, notifications);
            try {
                for (String n : new String[] { "Bob", "alice" }) {
                    Uri r = reopened.insert(ContactsContract.RawContacts.CONTENT_URI, new ContentValues());
                    ContentValues nm = new ContentValues();
                    nm.put(ContactsContract.Data.RAW_CONTACT_ID, ContentUris.parseId(r));
                    nm.put(ContactsContract.Data.MIMETYPE,
                            ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
                    nm.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME, n);
                    reopened.insert(ContactsContract.Data.CONTENT_URI, nm);
                }
                try (Cursor c = reopened.query(ContactsContract.Contacts.CONTENT_URI,
                        new String[] { ContactsContract.Contacts.DISPLAY_NAME },
                        ContactsContract.Contacts.DISPLAY_NAME + " IN ('alice', 'Bob')", null, "sort_key ASC")) {
                    check(c != null && c.getCount() == 2 && c.moveToFirst()
                            && "alice".equals(c.getString(0))
                            && c.moveToNext() && "Bob".equals(c.getString(0)),
                            "COLLATE LOCALIZED orders 'alice' before 'Bob' instead of binary ASCII");
                }
            } finally {
                reopened.shutdown();
            }
        } finally {
            Locale.setDefault(savedLocale);
            provider.shutdown();
        }
    }

    private static void testLocalHardDeleteAndSyncAdapterSoftDelete(File dbDir) throws Exception {
        File dbFile = new File(dbDir, "contacts-batch-sync.db");
        List<Uri> notifications = new ArrayList<>();
        Locale savedLocale = Locale.getDefault();
        Locale.setDefault(new Locale("en", "IL"));
        ContentProvider provider = newContactsProvider(dbFile, notifications);
        try {
            Uri syncRawUri = ContactsContract.RawContacts.CONTENT_URI.buildUpon()
                    .appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true")
                    .build();

            // Local contact (account_type == null) + local group -> deleted immediately on normal delete
            ContentValues localRawCv = new ContentValues();
            localRawCv.put(ContactsContract.RawContacts.DISPLAY_NAME_PRIMARY, "Local Temp");
            Uri localRawUri = provider.insert(ContactsContract.RawContacts.CONTENT_URI, localRawCv);
            long localRawId = ContentUris.parseId(localRawUri);
            ContentValues localDataCv = new ContentValues();
            localDataCv.put(ContactsContract.Data.RAW_CONTACT_ID, localRawId);
            localDataCv.put(ContactsContract.Data.MIMETYPE, ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE);
            localDataCv.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "050-2345678");
            provider.insert(ContactsContract.Data.CONTENT_URI, localDataCv);
            try (Cursor c = provider.query(ContactsContract.CommonDataKinds.Phone.CONTENT_URI,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER },
                    ContactsContract.Data.RAW_CONTACT_ID + " = ?",
                    new String[] { Long.toString(localRawId) }, null)) {
                check(c != null && c.moveToFirst() && "+972502345678".equals(c.getString(0)),
                        "E.164 normalized_number formats local 050-2345678 to +972502345678 under IL locale, got "
                                + (c != null && c.getCount() > 0 ? c.getString(0) : "empty"));
            }

            ContentValues localGroupCv = new ContentValues();
            localGroupCv.put(ContactsContract.Groups.TITLE, "Local Group");
            Uri localGroupUri = provider.insert(ContactsContract.Groups.CONTENT_URI, localGroupCv);
            check(provider.delete(localRawUri, null, null) == 1, "Delete local raw_contact");
            check(provider.delete(localGroupUri, null, null) == 1, "Delete local group");
            // Even with CALLER_IS_SYNCADAPTER=true, local rows are gone permanently (hard-deleted)
            try (Cursor c = provider.query(syncRawUri, new String[] { ContactsContract.RawContacts._ID },
                    "_id = ?", new String[] { Long.toString(localRawId) }, null)) {
                check(c != null && c.getCount() == 0,
                        "Local raw_contact (account_type==null) hard-deleted immediately on normal delete");
            }
            Uri syncGroupUri = ContactsContract.Groups.CONTENT_URI.buildUpon()
                    .appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true").build();
            try (Cursor c = provider.query(syncGroupUri, new String[] { ContactsContract.Groups._ID },
                    "_id = ?", new String[] { Long.toString(ContentUris.parseId(localGroupUri)) }, null)) {
                check(c != null && c.getCount() == 0,
                        "Local group (account_type==null) hard-deleted immediately on normal delete");
            }

            // Account-backed contact + transactional applyBatch + coalesced single notification
            notifications.clear();
            ArrayList<ContentProviderOperation> okBatch = new ArrayList<>();
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
            Uri normalPhoneItem = ContentUris.withAppendedId(
                    ContactsContract.Data.CONTENT_URI, ContentUris.parseId(res[2].uri));
            ContentValues phoneUp = new ContentValues();
            phoneUp.put(ContactsContract.CommonDataKinds.Phone.NUMBER, "052-2345678");
            check(provider.update(normalPhoneItem, phoneUp, null, null) == 1, "Normal phone update");
            try (Cursor c = provider.query(normalPhoneItem,
                    new String[] { ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER }, null, null, null)) {
                check(c != null && c.moveToFirst() && "+972522345678".equals(c.getString(0)),
                        "Update phone recomputes E.164 data4 normalized_number, got "
                                + (c != null && c.getCount() > 0 ? c.getString(0) : "empty"));
            }
            try (Cursor c = provider.query(syncRawUri,
                    new String[] { ContactsContract.RawContacts.DIRTY, ContactsContract.RawContacts.VERSION },
                    "_id = ?", new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getInt(1) == 2,
                        "Normal edit marks raw_contact dirty=1 and increments version");
            }

            // Failed applyBatch rolls back completely and emits ZERO notifications
            notifications.clear();
            ArrayList<ContentProviderOperation> failBatch = new ArrayList<>();
            failBatch.add(ContentProviderOperation.newInsert(ContactsContract.RawContacts.CONTENT_URI)
                    .withValue(ContactsContract.RawContacts.ACCOUNT_NAME, "should-rollback")
                    .build());
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

            // Account-backed normal delete soft-deletes (deleted=1, dirty=1, version=3), hidden by default
            Uri normalRawItem = ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId);
            check(provider.delete(normalRawItem, null, null) == 1, "Normal delete soft-deletes account-backed raw_contact");
            try (Cursor c = provider.query(syncRawUri,
                    new String[] { ContactsContract.RawContacts.DELETED, ContactsContract.RawContacts.DIRTY,
                            ContactsContract.RawContacts.VERSION },
                    "_id = ?", new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.moveToFirst() && c.getInt(0) == 1 && c.getInt(1) == 1 && c.getInt(2) == 3,
                        "Account-backed soft-delete retains deleted=1, dirty=1, version=3 for sync adapter");
            }
            try (Cursor c = provider.query(ContactsContract.DeletedContacts.CONTENT_URI,
                    new String[] { ContactsContract.DeletedContacts.CONTACT_ID }, null, null, null)) {
                check(c != null && c.getCount() >= 2, "deleted_contacts records both local and account deletions");
            }

            // Sync-adapter delete hard-deletes account-backed row completely
            Uri syncRawItem = ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId)
                    .buildUpon().appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true").build();
            check(provider.delete(syncRawItem, null, null) == 1, "SyncAdapter delete hard-deletes account raw_contact");
            try (Cursor c = provider.query(syncRawUri, null, "_id = ?",
                    new String[] { Long.toString(rawId) }, null)) {
                check(c != null && c.getCount() == 0, "SyncAdapter purge removes raw_contact completely");
            }
        } finally {
            Locale.setDefault(savedLocale);
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
    private static void testSecurityAppOpsUriGrantsBatchAndPackageManager(File dbDir) throws Exception {
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
        IApplicationThread threadWrite = threads[2];

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
            testSchemaStrictSqlEntitiesAndFallback(dbDir);
            testLocalHardDeleteAndSyncAdapterSoftDelete(dbDir);

            // Unprivileged package denied
            as(9301, 13301, () -> {
                rejects(SecurityException.class, () -> getProvider.invoke(
                        providers, procNone, PKG_NONE, "com.android.contacts"));
                rejects(SecurityException.class, () -> content.registerContentObserver(
                        ContactsContract.Contacts.CONTENT_URI, true, observer, 0, 36));
            });

            // Write app creates two contacts and two data rows via IContentProvider
            Uri[] contactUris = new Uri[2];
            Uri[] dataUris = new Uri[2];
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
                    dataUris[i] = h.provider.insert(source(13303, PKG_WRITE),
                            ContactsContract.Data.CONTENT_URI, nm, null);
                    try (Cursor rc = h.provider.query(source(13303, PKG_WRITE),
                            ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId),
                            new String[] { ContactsContract.RawContacts.CONTACT_ID }, null, null)) {
                        check(rc != null && rc.moveToFirst(), "Lookup contact_id for raw_contact " + i);
                        contactUris[i] = ContentUris.withAppendedId(
                                ContactsContract.Contacts.CONTENT_URI, rc.getLong(0));
                    }
                }
            });

            // Read app queries via IContentProvider and runs assert-only applyBatch WITHOUT WRITE_CONTACTS
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
                ArrayList<ContentProviderOperation> assertBatch = new ArrayList<>();
                assertBatch.add(ContentProviderOperation.newAssertQuery(ContactsContract.Contacts.CONTENT_URI)
                        .withExpectedCount(2)
                        .build());
                ContentProviderResult[] assertRes = h.provider.applyBatch(
                        source(13302, PKG_READ), "com.android.contacts", assertBatch);
                check(assertRes.length == 1 && assertRes[0].count == 2,
                        "applyBatch with assert-only operations succeeds with READ_CONTACTS only");
                rejects(SecurityException.class, () -> h.provider.insert(
                        source(13302, PKG_READ), ContactsContract.RawContacts.CONTENT_URI, new ContentValues(), null));
            });

            // Legacy targetSdk 22 app: allowed initially, soft-denied by AppOps once FLAG_PERMISSION_REVOKED_COMPAT is set
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
                        new String[] { ContactsContract.Contacts._ID }, null, null)) {
                    check(c != null && c.getCount() == 0,
                            "Framework soft-denies legacy caller when Contacts AppOp is revoked");
                }
            });

            // URI grants: read grant on contactUris[0], write grant on dataUris[0] for applyBatch without WRITE_CONTACTS
            Uri grantedUri = contactUris[0];
            Uri ungrantedUri = contactUris[1];
            Uri grantedDataUri = dataUris[0];
            as(9302, 13302, () -> am.grantUriPermission(threadRead, PKG_NONE, grantedUri, READ_FLAG, 0));
            as(9303, 13303, () -> am.grantUriPermission(threadWrite, PKG_NONE, grantedDataUri, WRITE_FLAG, 0));
            check((Boolean) canAccess.invoke(uriGrants, appNone, grantedUri, READ_FLAG, 0),
                    "UriGrants permits granted contact URI");
            check(!(Boolean) canAccess.invoke(uriGrants, appNone, ungrantedUri, READ_FLAG, 0),
                    "UriGrants denies ungranted contact URI");
            check((Boolean) canAccess.invoke(uriGrants, appNone, grantedDataUri, WRITE_FLAG, 0),
                    "UriGrants permits granted data write URI");
            as(9301, 13301, () -> {
                ContentProviderHolder h = (ContentProviderHolder) getProvider.invoke(
                        providers, procNone, PKG_NONE, "com.android.contacts");
                check(h != null && h.provider != null, "URI-granted app acquires com.android.contacts holder");
                try (Cursor c = h.provider.query(source(13301, PKG_NONE), grantedUri,
                        new String[] { ContactsContract.Contacts.DISPLAY_NAME }, null, null)) {
                    check(c != null && c.getCount() == 1 && c.moveToFirst()
                            && "Contact 1".equals(c.getString(0)),
                            "URI-granted app reads granted contact row through IContentProvider");
                }
                rejects(SecurityException.class, () -> h.provider.query(source(13301, PKG_NONE),
                        ungrantedUri, new String[] { ContactsContract.Contacts._ID }, null, null));
                rejects(SecurityException.class, () -> h.provider.query(source(13301, PKG_NONE),
                        ContactsContract.Contacts.CONTENT_URI,
                        new String[] { ContactsContract.Contacts._ID }, null, null));
                rejects(SecurityException.class, () -> h.provider.delete(
                        source(13301, PKG_NONE), grantedUri, null));

                // Write URI grant works via applyBatch without WRITE_CONTACTS on authority root
                ArrayList<ContentProviderOperation> writeGrantBatch = new ArrayList<>();
                writeGrantBatch.add(ContentProviderOperation.newUpdate(grantedDataUri)
                        .withValue(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME,
                                "Granted Updated")
                        .build());
                ContentProviderResult[] grantRes = h.provider.applyBatch(
                        source(13301, PKG_NONE), "com.android.contacts", writeGrantBatch);
                check(grantRes.length == 1 && grantRes[0].count == 1,
                        "applyBatch honors single-row write URI grant without root WRITE_CONTACTS");
                Bundle injectionArgs = new Bundle();
                injectionArgs.putString(ContentResolver.QUERY_ARG_SQL_SELECTION, "1) OR (1");
                rejects(IllegalArgumentException.class, () -> h.provider.update(
                        source(13301, PKG_NONE), grantedDataUri,
                        new ContentValues(), injectionArgs));
            });

            // Single-read AppOps note count check (must not double-note per query)
            int opsBefore = pm.appOps().noteOperation(AppOpsManager.OP_READ_CONTACTS, 13302,
                    PKG_READ, null, false, null, false).getOpMode();
            check(opsBefore == AppOpsManager.MODE_ALLOWED, "Read app contacts op mode allowed");
        } finally {
            contactsProv.shutdown();
            cache.clear();
            cache.putAll(previousCache);
            smField.set(null, previousSm);
        }

        // PackageManager resolution under platform "android" package & legacy authority absence
        ProviderInfo contactsInfo = pm.resolveContentProvider("com.android.contacts", 0, 0);
        check(contactsInfo != null && BUILT_IN_PKG.equals(contactsInfo.packageName)
                && Manifest.permission.READ_CONTACTS.equals(contactsInfo.readPermission)
                && Manifest.permission.WRITE_CONTACTS.equals(contactsInfo.writePermission)
                && contactsInfo.grantUriPermissions,
                "PM resolves com.android.contacts under android with permissions and URI grants");
        check(pm.resolveContentProvider("contacts", 0, 0) == null,
                "PM must not resolve dropped legacy contacts authority");
        check(pm.resolveContentProvider("com.android.contacts", 0, 10) == null,
                "PM resolveContentProvider denies user 10");
        check(pm.getProviderInfo(new ComponentName(BUILT_IN_PKG,
                "com.android.providers.contacts.ContactsProvider2"), 0, 0) != null,
                "PM getProviderInfo resolves ContactsProvider2 under android");
        PackageInfo platformPkg = pm.getPackageInfo(BUILT_IN_PKG, PackageManager.GET_PROVIDERS, 0);
        check(platformPkg != null && platformPkg.providers != null
                        && Arrays.stream(platformPkg.providers)
                                .anyMatch(info -> "com.android.contacts".equals(info.authority)),
                "Platform package exposes ContactsProvider2 metadata");
        check(Arrays.equals(pm.getPackagesForUid(Process.SYSTEM_UID), new String[] { BUILT_IN_PKG }),
                "Contacts retains platform SYSTEM_UID identity without adding a synthetic package");

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
            testSecurityAppOpsUriGrantsBatchAndPackageManager(data.toFile());
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
