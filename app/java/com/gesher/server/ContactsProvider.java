/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.app.GesherSettingsContext;
import android.content.AttributionSource;
import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.OperationApplicationException;
import android.content.PermissionChecker;
import android.content.UriMatcher;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.DatabaseUtils;
import android.database.MatrixCursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.icu.text.AlphabeticIndex;
import android.net.Uri;
import android.os.Binder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.SharedMemory;
import android.os.UserHandle;
import android.provider.ContactsContract;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.OsConstants;
import android.telephony.PhoneNumberUtils;

import com.gesher.app.locale.SystemLocale;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * Built-in SQLite-backed {@code content://com.android.contacts} provider for
 * {@link ContactsContract}. Starts with an empty address book and a local default
 * directory entry, enforces {@code READ_CONTACTS} / {@code WRITE_CONTACTS}, AppOps,
 * and URI grants, and implements transactional {@link #applyBatch}, sync-adapter
 * soft/hard deletes, loose {@code phone_lookup} matching, and display-name fallback.
 * The legacy {@code contacts} authority, usage feedback, frequent contacts,
 * provider status, callable queries and dedicated contact photo URIs are outside
 * this implementation.
 *
 * <p><b>Limitation (1:1 contacts without multi-raw-contact aggregation):</b>
 * Inserting a {@code raw_contact} creates its own aggregate {@code contacts}
 * row. Automatic cross-account raw-contact aggregation and
 * split/join heuristics are not implemented.
 */
final class ContactsProvider extends ContentProvider {
    private static final Set<String> CONTACT_OPTIONS = Set.of(
            "starred", "custom_ringtone", "send_to_voicemail");
    private static final Set<String> RAW_WRITE_COLUMNS = Set.of(
            "account_name", "account_type", "data_set", "sourceid", "version", "dirty", "deleted",
            "aggregation_mode", "starred", "pinned", "custom_ringtone", "send_to_voicemail",
            "sync1", "sync2", "sync3", "sync4");
    private static final Set<String> DATA_WRITE_COLUMNS = Set.of(
            "is_primary", "is_super_primary", "is_read_only", "data1", "data2",
            "data3", "data4", "data5", "data6", "data7", "data8", "data9", "data10", "data11",
            "data12", "data13", "data14", "data15", "data_sync1", "data_sync2", "data_sync3",
            "data_sync4", "carrier_presence");
    private static final Set<String> GROUP_WRITE_COLUMNS = Set.of(
            "account_name", "account_type", "data_set", "sourceid", "version", "dirty", "title",
            "title_res", "res_package", "notes", "system_id", "deleted", "group_visible",
            "should_sync", "auto_add", "favorites", "group_is_read_only", "sync1", "sync2", "sync3",
            "sync4");
    private static final Set<String> SETTINGS_WRITE_COLUMNS = Set.of(
            "account_name", "account_type", "data_set", "ungrouped_visible", "should_sync");
    private static final Set<String> SETTINGS_UPDATE_COLUMNS = Set.of("ungrouped_visible", "should_sync");
    private static final int CONTACTS = 1;
    private static final int CONTACTS_ID = 2;
    private static final int CONTACTS_FILTER_EMPTY = 3;
    private static final int CONTACTS_FILTER = 4;
    private static final int CONTACTS_LOOKUP = 5;
    private static final int CONTACTS_LOOKUP_ID = 6;
    private static final int CONTACTS_DATA = 7;
    private static final int CONTACTS_ENTITY = 8;

    private static final int RAW_CONTACTS = 10;
    private static final int RAW_CONTACTS_ID = 11;
    private static final int RAW_CONTACTS_DATA = 12;
    private static final int RAW_CONTACT_ENTITY = 13;
    private static final int RAW_CONTACT_ENTITIES = 14;

    private static final int DATA = 20;
    private static final int DATA_ID = 21;
    private static final int PHONES = 22;
    private static final int PHONES_ID = 23;
    private static final int PHONES_FILTER_EMPTY = 24;
    private static final int PHONES_FILTER = 25;
    private static final int EMAILS = 26;
    private static final int EMAILS_ID = 27;
    private static final int EMAILS_FILTER_EMPTY = 28;
    private static final int EMAILS_FILTER = 29;
    private static final int POSTALS = 30;
    private static final int POSTALS_ID = 31;

    private static final int PHONE_LOOKUP = 40;
    private static final int GROUPS = 50;
    private static final int GROUPS_ID = 51;
    private static final int GROUPS_SUMMARY = 52;
    private static final int DELETED_CONTACTS = 60;
    private static final int DIRECTORIES = 70;
    private static final int DIRECTORIES_ID = 71;
    private static final int SETTINGS = 80;
    private static final int PROFILE = 90;
    private static final int PROFILE_SYNC_STATE = 91;
    private static final int PROFILE_SYNC_STATE_ID = 92;
    private static final int PROFILE_VCARD = 93;
    private static final int CONTACT_THUMBNAIL = 94;
    private static final int CONTACT_DISPLAY_PHOTO = 95;
    private static final int RAW_DISPLAY_PHOTO = 96;
    private static final int DISPLAY_PHOTO_FILE = 97;
    private static final int PHOTO_DIMENSIONS = 98;
    private static final Executor sProfilePhotoWriters = new ThreadPoolExecutor(
            0, 2, 30, TimeUnit.SECONDS, new SynchronousQueue<>(), task -> {
                Thread worker = new Thread(task, "ContactsProfilePhoto");
                worker.setDaemon(true);
                return worker;
            });
    private static final String MIME_PHOTO = ContactsContract.CommonDataKinds.Photo.CONTENT_ITEM_TYPE;
    private static final Map<String, String> PROFILE_SYNC_COLUMNS =
            identityMap("_id", "account_name", "account_type", "data");
    private static final Map<String, String> PROFILE_VCARD_COLUMNS = Map.of(
            "_id", "_id",
            OpenableColumns.DISPLAY_NAME, "display_name || '.vcf' AS " + OpenableColumns.DISPLAY_NAME,
            OpenableColumns.SIZE, "NULL AS " + OpenableColumns.SIZE);
    private static final String PROFILE_LOOKUP = "profile";

    private static final String MIME_STRUCTURED_NAME =
            ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE;
    private static final String MIME_NICKNAME =
            ContactsContract.CommonDataKinds.Nickname.CONTENT_ITEM_TYPE;
    private static final String MIME_ORGANIZATION =
            ContactsContract.CommonDataKinds.Organization.CONTENT_ITEM_TYPE;
    private static final String MIME_PHONE =
            ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE;
    private static final String MIME_EMAIL =
            ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE;
    private static final String MIME_POSTAL =
            ContactsContract.CommonDataKinds.StructuredPostal.CONTENT_ITEM_TYPE;

    private static final int DB_VERSION = 1;
    private static Locale sPhonebookLocale;
    private static AlphabeticIndex.ImmutableIndex<Void> sPhonebookIndex;
    private static final Uri NOTIFY_URI = ContactsContract.AUTHORITY_URI;

    private static final UriMatcher sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);
    private static final Map<String, String> sContactsProjectionMap;
    private static final Map<String, String> sRawContactsProjectionMap;
    private static final Map<String, String> sDataProjectionMap;
    private static final Map<String, String> sContactsEntityProjectionMap;
    private static final Map<String, String> sRawEntityProjectionMap;
    private static final Map<String, String> sPhoneLookupProjectionMap;
    private static final Map<String, String> sGroupsProjectionMap;
    private static final Map<String, String> sGroupsSummaryProjectionMap;
    private static final Map<String, String> sDeletedContactsProjectionMap;
    private static final Map<String, String> sDirectoriesProjectionMap;
    private static final Map<String, String> sSettingsProjectionMap;

    private static Map<String, String> identityMap(String... cols) {
        Map<String, String> map = new HashMap<>();
        for (String col : cols) map.put(col, col);
        return Collections.unmodifiableMap(map);
    }

    static {
        String auth = ContactsProviderSupport.CONTACTS_AUTHORITY;
        sUriMatcher.addURI(auth, "contacts", CONTACTS);
        sUriMatcher.addURI(auth, "contacts/#", CONTACTS_ID);
        sUriMatcher.addURI(auth, "contacts/filter", CONTACTS_FILTER_EMPTY);
        sUriMatcher.addURI(auth, "contacts/filter/*", CONTACTS_FILTER);
        sUriMatcher.addURI(auth, "contacts/lookup/*", CONTACTS_LOOKUP);
        sUriMatcher.addURI(auth, "contacts/lookup/*/#", CONTACTS_LOOKUP_ID);
        sUriMatcher.addURI(auth, "contacts/#/data", CONTACTS_DATA);
        sUriMatcher.addURI(auth, "contacts/#/entities", CONTACTS_ENTITY);

        sUriMatcher.addURI(auth, "raw_contacts", RAW_CONTACTS);
        sUriMatcher.addURI(auth, "raw_contacts/#", RAW_CONTACTS_ID);
        sUriMatcher.addURI(auth, "raw_contacts/#/data", RAW_CONTACTS_DATA);
        sUriMatcher.addURI(auth, "raw_contacts/#/entity", RAW_CONTACT_ENTITY);
        sUriMatcher.addURI(auth, "raw_contact_entities", RAW_CONTACT_ENTITIES);

        sUriMatcher.addURI(auth, "data", DATA);
        sUriMatcher.addURI(auth, "data/#", DATA_ID);
        sUriMatcher.addURI(auth, "data/phones", PHONES);
        sUriMatcher.addURI(auth, "data/phones/#", PHONES_ID);
        sUriMatcher.addURI(auth, "data/phones/filter", PHONES_FILTER_EMPTY);
        sUriMatcher.addURI(auth, "data/phones/filter/*", PHONES_FILTER);
        sUriMatcher.addURI(auth, "data/emails", EMAILS);
        sUriMatcher.addURI(auth, "data/emails/#", EMAILS_ID);
        sUriMatcher.addURI(auth, "data/emails/filter", EMAILS_FILTER_EMPTY);
        sUriMatcher.addURI(auth, "data/emails/filter/*", EMAILS_FILTER);
        sUriMatcher.addURI(auth, "data/postals", POSTALS);
        sUriMatcher.addURI(auth, "data/postals/#", POSTALS_ID);

        sUriMatcher.addURI(auth, "phone_lookup/*", PHONE_LOOKUP);
        sUriMatcher.addURI(auth, "phone_lookup_enterprise/*", PHONE_LOOKUP);
        sUriMatcher.addURI(auth, "groups", GROUPS);
        sUriMatcher.addURI(auth, "groups/#", GROUPS_ID);
        sUriMatcher.addURI(auth, "groups_summary", GROUPS_SUMMARY);
        sUriMatcher.addURI(auth, "deleted_contacts", DELETED_CONTACTS);
        sUriMatcher.addURI(auth, "directories", DIRECTORIES);
        sUriMatcher.addURI(auth, "directories/#", DIRECTORIES_ID);
        sUriMatcher.addURI(auth, "settings", SETTINGS);
        sUriMatcher.addURI(auth, "profile", PROFILE);
        sUriMatcher.addURI(auth, "profile/entities", CONTACTS_ENTITY);
        sUriMatcher.addURI(auth, "profile/raw_contacts", RAW_CONTACTS);
        sUriMatcher.addURI(auth, "profile/raw_contacts/#", RAW_CONTACTS_ID);
        sUriMatcher.addURI(auth, "profile/raw_contacts/#/data", RAW_CONTACTS_DATA);
        sUriMatcher.addURI(auth, "profile/raw_contacts/#/entity", RAW_CONTACT_ENTITY);
        sUriMatcher.addURI(auth, "profile/raw_contact_entities", RAW_CONTACT_ENTITIES);
        sUriMatcher.addURI(auth, "profile/data", DATA);
        sUriMatcher.addURI(auth, "profile/data/#", DATA_ID);
        sUriMatcher.addURI(auth, "profile/syncstate", PROFILE_SYNC_STATE);
        sUriMatcher.addURI(auth, "profile/syncstate/#", PROFILE_SYNC_STATE_ID);
        sUriMatcher.addURI(auth, "profile/as_vcard", PROFILE_VCARD);
        sUriMatcher.addURI(auth, "contacts/as_vcard/*", PROFILE_VCARD);
        sUriMatcher.addURI(auth, "profile/photo", CONTACT_THUMBNAIL);
        sUriMatcher.addURI(auth, "contacts/#/photo", CONTACT_THUMBNAIL);
        sUriMatcher.addURI(auth, "contacts/lookup/*/photo", CONTACT_THUMBNAIL);
        sUriMatcher.addURI(auth, "contacts/lookup/*/#/photo", CONTACT_THUMBNAIL);
        sUriMatcher.addURI(auth, "profile/display_photo", CONTACT_DISPLAY_PHOTO);
        sUriMatcher.addURI(auth, "contacts/#/display_photo", CONTACT_DISPLAY_PHOTO);
        sUriMatcher.addURI(auth, "contacts/lookup/*/display_photo", CONTACT_DISPLAY_PHOTO);
        sUriMatcher.addURI(auth, "contacts/lookup/*/#/display_photo", CONTACT_DISPLAY_PHOTO);
        sUriMatcher.addURI(auth, "profile/raw_contacts/#/display_photo", RAW_DISPLAY_PHOTO);
        sUriMatcher.addURI(auth, "raw_contacts/#/display_photo", RAW_DISPLAY_PHOTO);
        sUriMatcher.addURI(auth, "display_photo/#", DISPLAY_PHOTO_FILE);
        sUriMatcher.addURI(auth, "photo_dimensions", PHOTO_DIMENSIONS);

        sContactsProjectionMap = identityMap(
                "_id", "lookup", "name_raw_contact_id", "photo_id", "photo_file_id",
                "photo_uri", "photo_thumb_uri", "display_name", "display_name_alt",
                "display_name_source", "phonetic_name", "sort_key", "sort_key_alt",
                "phonebook_label", "phonebook_label_alt", "phonebook_bucket",
                "phonebook_bucket_alt", "in_visible_group", "in_default_directory",
                "has_phone_number", "times_contacted", "times_used",
                "last_time_contacted", "last_time_used", "starred", "pinned",
                "custom_ringtone", "send_to_voicemail", "contact_presence",
                "contact_status", "contact_status_ts", "contact_status_res_package",
                "contact_status_label", "contact_status_icon",
                "contact_last_updated_timestamp", "is_user_profile");

        sRawContactsProjectionMap = identityMap(
                "_id", "contact_id", "account_name", "account_type", "data_set",
                "sourceid", "version", "dirty", "deleted", "aggregation_mode",
                "display_name", "display_name_alt", "display_name_source",
                "phonetic_name", "sort_key", "sort_key_alt", "phonebook_label",
                "phonebook_label_alt", "phonebook_bucket", "phonebook_bucket_alt",
                "starred", "pinned", "custom_ringtone", "send_to_voicemail",
                "times_contacted", "times_used", "last_time_contacted",
                "last_time_used", "raw_contact_is_user_profile", "sync1", "sync2",
                "sync3", "sync4", "lookup", "has_phone_number", "in_visible_group",
                "in_default_directory", "is_user_profile", "photo_id",
                "photo_file_id", "photo_uri", "photo_thumb_uri",
                "contact_presence", "contact_status",
                "contact_last_updated_timestamp");

        sDataProjectionMap = identityMap(
                "_id", "raw_contact_id", "contact_id", "lookup", "mimetype",
                "is_primary", "is_super_primary", "is_read_only", "data_version",
                "data1", "data2", "data3", "data4", "data5", "data6", "data7", "data8",
                "data9", "data10", "data11", "data12", "data13", "data14", "data15",
                "data_sync1", "data_sync2", "data_sync3", "data_sync4",
                "carrier_presence", "display_name", "display_name_alt",
                "display_name_source", "phonetic_name", "sort_key", "sort_key_alt",
                "phonebook_label", "phonebook_label_alt", "phonebook_bucket",
                "phonebook_bucket_alt", "photo_id", "photo_file_id", "photo_uri",
                "photo_thumb_uri", "in_visible_group", "in_default_directory",
                "has_phone_number", "starred", "pinned", "custom_ringtone",
                "send_to_voicemail", "times_contacted", "times_used",
                "last_time_contacted", "last_time_used", "contact_presence",
                "contact_status", "contact_status_ts", "contact_last_updated_timestamp",
                "account_name", "account_type", "data_set", "sourceid", "version",
                "dirty", "deleted", "name_raw_contact_id",
                "raw_contact_is_user_profile", "is_user_profile");

        sRawEntityProjectionMap = entityProjection(false);
        sContactsEntityProjectionMap = entityProjection(true);

        sPhoneLookupProjectionMap = identityMap(
                "_id", "contact_id", "raw_contact_id", "data_id", "lookup",
                "display_name", "display_name_alt", "sort_key", "sort_key_alt",
                "phonebook_label", "phonebook_bucket", "number", "type", "label",
                "normalized_number", "photo_id", "photo_file_id", "photo_uri",
                "photo_thumb_uri", "starred", "pinned", "custom_ringtone",
                "send_to_voicemail", "in_visible_group", "in_default_directory",
                "has_phone_number", "times_contacted", "times_used",
                "last_time_contacted", "last_time_used", "contact_presence",
                "contact_status", "contact_last_updated_timestamp");

        sGroupsProjectionMap = identityMap(
                "_id", "account_name", "account_type", "data_set", "sourceid",
                "version", "dirty", "title", "title_res", "res_package", "notes",
                "system_id", "deleted", "group_visible", "should_sync", "auto_add",
                "favorites", "group_is_read_only", "sync1", "sync2", "sync3",
                "sync4");
        Map<String, String> summary = new HashMap<>(sGroupsProjectionMap);
        summary.put("summ_count", "summ_count");
        summary.put("summ_phones", "summ_phones");
        sGroupsSummaryProjectionMap = Collections.unmodifiableMap(summary);

        sDeletedContactsProjectionMap = identityMap(
                "contact_id", "contact_deleted_timestamp");

        sDirectoriesProjectionMap = identityMap(
                "_id", "packageName", "typeResourceId", "displayName",
                "accountType", "accountName", "exportSupport", "shortcutSupport",
                "photoSupport");

        sSettingsProjectionMap = identityMap(
                "account_name", "account_type", "data_set", "ungrouped_visible",
                "should_sync", ContactsContract.Settings.ANY_UNSYNCED,
                ContactsContract.Settings.UNGROUPED_COUNT, ContactsContract.Settings.UNGROUPED_WITH_PHONES);
    }

    private static Map<String, String> entityProjection(boolean aggregate) {
        Set<String> columns = new TreeSet<>(sRawContactsProjectionMap.keySet());
        columns.addAll(sDataProjectionMap.keySet());
        if (aggregate) columns.addAll(sContactsProjectionMap.keySet());
        columns.add("data_id");
        return identityMap(columns.toArray(new String[0]));
    }

    private final AttributionSource mDataSource = new AttributionSource.Builder(Process.SYSTEM_UID)
            .setPackageName(ContactsProviderSupport.PACKAGE).build();
    private final CallerIdentity mCallers;
    private final UriGrants mGrants;
    private final File mDatabase;
    private final ContentProviders.RegisteredProvider mRegistration;
    private final Object mDbLock = new Object();
    private SQLiteDatabase mDb;
    private boolean mInitialized;
    private Throwable mInitializationFailure;
    private int mBatchDepth;
    private boolean mBatchDirty;
    private boolean mBatchSyncToNetwork;
    private boolean mBatchProfileDirty;

    ContactsProvider(ContentProviders providers, PackageManagerService pm,
            ActivityTaskManagerService atm, UriGrants grants) {
        this(providers, pm, atm, grants,
                new File(ContactsProviderSupport.dataDirectory(), "databases/contacts2.db"));
    }

    ContactsProvider(PackageManagerService pm, ActivityTaskManagerService atm, File databaseFile) {
        this(null, pm, atm, null, databaseFile);
    }

    ContactsProvider(ContentProviders providers, PackageManagerService pm,
            ActivityTaskManagerService atm, UriGrants grants, File databaseFile) {
        mCallers = providers != null ? providers.callers()
                : pm == null ? null : new CallerIdentity(pm, atm);
        mGrants = grants;
        mDatabase = databaseFile.getAbsoluteFile();
        if (providers != null) {
            mRegistration = new ContentProviders.RegisteredProvider(
                    ContactsProviderSupport.contactsProviderInfo(), getIContentProvider());
            providers.registerProvider(mRegistration);
        } else {
            mRegistration = null;
        }
    }

    ProviderInfo info() {
        return ContactsProviderSupport.contactsProviderInfo();
    }

    synchronized ContentProviderHolder holder() {
        if (mInitializationFailure != null) {
            throw new IllegalStateException("Contacts provider initialization failed",
                    mInitializationFailure);
        }
        ProviderInfo declared = info();
        if (!mInitialized) {
            try {
                attachInfo(ContactsProviderSupport.contextForProvider(
                        GesherSettingsContext.get(), mGrants, mRegistration),
                        declared);
                mInitialized = true;
            } catch (RuntimeException | Error failure) {
                mInitializationFailure = failure;
                shutdown();
                throw failure;
            }
        }
        return ContactsProviderSupport.holder(declared, getIContentProvider());
    }

    @Override
    protected int enforceReadPermissionInner(Uri uri, AttributionSource source) {
        return enforceContactsPermission(uri, source, ContactsProviderSupport.READ_CONTACTS,
                Intent.FLAG_GRANT_READ_URI_PERMISSION);
    }

    @Override
    protected int enforceWritePermissionInner(Uri uri, AttributionSource source) {
        return enforceContactsPermission(uri, source, ContactsProviderSupport.WRITE_CONTACTS,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
    }

    private int enforceContactsPermission(Uri uri, AttributionSource source, String permission,
            int mode) {
        int pid = Binder.getCallingPid();
        int uid = Binder.getCallingUid();
        if (pid == Process.myPid()) return PermissionChecker.PERMISSION_GRANTED;
        ContactsProviderSupport.authenticate(mCallers, source);
        if (UserHandle.getUserId(uid) != 0) {
            throw new SecurityException("Only the primary user's address book is available");
        }

        Context context = getContext();
        long identity = Binder.clearCallingIdentity();
        int result;
        try {
            result = PermissionChecker.checkPermissionForDataDeliveryFromDataSource(
                    context, permission, -1, new AttributionSource(mDataSource, source), null);
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
        if (result != PermissionChecker.PERMISSION_GRANTED
                && context.checkUriPermission(uri, pid, uid, mode) == PackageManager.PERMISSION_GRANTED) {
            return PermissionChecker.PERMISSION_GRANTED;
        }
        if (result == PermissionChecker.PERMISSION_HARD_DENIED) {
            throw new SecurityException("Contacts access requires " + permission);
        }
        return result;
    }

    @Override
    public void shutdown() {
        synchronized (mDbLock) {
            if (mDb != null) {
                mDb.close();
                mDb = null;
            }
        }
    }

    private SQLiteDatabase openDatabase() {
        synchronized (mDbLock) {
            if (mDb != null && mDb.isOpen()) return mDb;
            File parent = mDatabase.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs() && !parent.isDirectory()) {
                throw new IllegalStateException("Failed to create Contacts database directory: " + parent);
            }
            SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(mDatabase, null);
            try {
                db.execSQL("PRAGMA foreign_keys = ON;");
                int oldVersion = db.getVersion();
                if (oldVersion < DB_VERSION) {
                    db.beginTransaction();
                    try {
                        if (oldVersion == 0) {
                            onCreateDatabase(db);
                        } else {
                            onUpgradeDatabase(db, oldVersion, DB_VERSION);
                        }
                        db.setVersion(DB_VERSION);
                        db.setTransactionSuccessful();
                    } finally {
                        db.endTransaction();
                    }
                }
            } catch (RuntimeException | Error failure) {
                db.close();
                throw failure;
            }
            mDb = db;
            return db;
        }
    }

    private static void onCreateDatabase(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE IF NOT EXISTS contacts ("
                + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "lookup TEXT,"
                + "name_raw_contact_id INTEGER,"
                + "photo_id INTEGER DEFAULT 0,"
                + "photo_file_id INTEGER,"
                + "photo_uri TEXT,"
                + "photo_thumb_uri TEXT,"
                + "display_name TEXT,"
                + "display_name_alt TEXT,"
                + "display_name_source INTEGER DEFAULT 0,"
                + "phonetic_name TEXT,"
                + "sort_key TEXT COLLATE LOCALIZED,"
                + "sort_key_alt TEXT COLLATE LOCALIZED,"
                + "phonebook_label TEXT,"
                + "phonebook_label_alt TEXT,"
                + "phonebook_bucket INTEGER DEFAULT 0,"
                + "phonebook_bucket_alt INTEGER DEFAULT 0,"
                + "in_visible_group INTEGER NOT NULL DEFAULT 1,"
                + "in_default_directory INTEGER NOT NULL DEFAULT 1,"
                + "has_phone_number INTEGER NOT NULL DEFAULT 0,"
                + "times_contacted INTEGER NOT NULL DEFAULT 0,"
                + "times_used INTEGER NOT NULL DEFAULT 0,"
                + "last_time_contacted INTEGER,"
                + "last_time_used INTEGER,"
                + "starred INTEGER NOT NULL DEFAULT 0,"
                + "pinned INTEGER NOT NULL DEFAULT 0,"
                + "custom_ringtone TEXT,"
                + "send_to_voicemail INTEGER DEFAULT 0,"
                + "contact_presence INTEGER,"
                + "contact_status TEXT,"
                + "contact_status_ts INTEGER,"
                + "contact_status_res_package TEXT,"
                + "contact_status_label INTEGER,"
                + "contact_status_icon INTEGER,"
                + "contact_last_updated_timestamp INTEGER DEFAULT 0,"
                + "is_user_profile INTEGER NOT NULL DEFAULT 0"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS raw_contacts ("
                + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "contact_id INTEGER REFERENCES contacts(_id) ON DELETE SET NULL,"
                + "account_name TEXT,"
                + "account_type TEXT,"
                + "data_set TEXT,"
                + "sourceid TEXT,"
                + "version INTEGER NOT NULL DEFAULT 1,"
                + "dirty INTEGER NOT NULL DEFAULT 0,"
                + "deleted INTEGER NOT NULL DEFAULT 0,"
                + "aggregation_mode INTEGER NOT NULL DEFAULT 0,"
                + "display_name TEXT,"
                + "display_name_alt TEXT,"
                + "display_name_source INTEGER DEFAULT 0,"
                + "phonetic_name TEXT,"
                + "sort_key TEXT COLLATE LOCALIZED,"
                + "sort_key_alt TEXT COLLATE LOCALIZED,"
                + "phonebook_label TEXT,"
                + "phonebook_label_alt TEXT,"
                + "phonebook_bucket INTEGER DEFAULT 0,"
                + "phonebook_bucket_alt INTEGER DEFAULT 0,"
                + "starred INTEGER NOT NULL DEFAULT 0,"
                + "pinned INTEGER NOT NULL DEFAULT 0,"
                + "custom_ringtone TEXT,"
                + "send_to_voicemail INTEGER DEFAULT 0,"
                + "times_contacted INTEGER NOT NULL DEFAULT 0,"
                + "times_used INTEGER NOT NULL DEFAULT 0,"
                + "last_time_contacted INTEGER,"
                + "last_time_used INTEGER,"
                + "raw_contact_is_user_profile INTEGER NOT NULL DEFAULT 0,"
                + "sync1 TEXT, sync2 TEXT, sync3 TEXT, sync4 TEXT"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS mimetypes ("
                + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "mimetype TEXT UNIQUE NOT NULL"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS data ("
                + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "raw_contact_id INTEGER NOT NULL REFERENCES raw_contacts(_id) ON DELETE CASCADE,"
                + "mimetype_id INTEGER NOT NULL REFERENCES mimetypes(_id),"
                + "is_primary INTEGER NOT NULL DEFAULT 0,"
                + "is_super_primary INTEGER NOT NULL DEFAULT 0,"
                + "is_read_only INTEGER NOT NULL DEFAULT 0,"
                + "data_version INTEGER NOT NULL DEFAULT 0,"
                + "data1 TEXT, data2 TEXT, data3 TEXT, data4 TEXT, data5 TEXT,"
                + "data6 TEXT, data7 TEXT, data8 TEXT, data9 TEXT, data10 TEXT,"
                + "data11 TEXT, data12 TEXT, data13 TEXT, data14 TEXT, data15 BLOB,"
                + "data_sync1 TEXT, data_sync2 TEXT, data_sync3 TEXT, data_sync4 TEXT,"
                + "carrier_presence INTEGER NOT NULL DEFAULT 0"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS groups ("
                + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "account_name TEXT,"
                + "account_type TEXT,"
                + "data_set TEXT,"
                + "sourceid TEXT,"
                + "version INTEGER NOT NULL DEFAULT 1,"
                + "dirty INTEGER NOT NULL DEFAULT 0,"
                + "title TEXT,"
                + "title_res INTEGER,"
                + "res_package TEXT,"
                + "notes TEXT,"
                + "system_id TEXT,"
                + "deleted INTEGER NOT NULL DEFAULT 0,"
                + "group_visible INTEGER NOT NULL DEFAULT 1,"
                + "should_sync INTEGER NOT NULL DEFAULT 1,"
                + "auto_add INTEGER NOT NULL DEFAULT 0,"
                + "favorites INTEGER NOT NULL DEFAULT 0,"
                + "group_is_read_only INTEGER NOT NULL DEFAULT 0,"
                + "sync1 TEXT, sync2 TEXT, sync3 TEXT, sync4 TEXT"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS deleted_contacts ("
                + "contact_id INTEGER PRIMARY KEY,"
                + "contact_deleted_timestamp INTEGER NOT NULL DEFAULT 0"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS directories ("
                + "_id INTEGER PRIMARY KEY,"
                + "packageName TEXT,"
                + "typeResourceId INTEGER,"
                + "displayName TEXT,"
                + "accountType TEXT,"
                + "accountName TEXT,"
                + "exportSupport INTEGER NOT NULL DEFAULT 0,"
                + "shortcutSupport INTEGER NOT NULL DEFAULT 2,"
                + "photoSupport INTEGER NOT NULL DEFAULT 3"
                + ");");
        db.execSQL("INSERT OR IGNORE INTO directories (_id, packageName, displayName) VALUES "
                + "(0, '" + ContactsProviderSupport.PACKAGE + "', 'Contacts');");
        db.execSQL("CREATE TABLE IF NOT EXISTS settings ("
                + "account_name TEXT,"
                + "account_type TEXT,"
                + "data_set TEXT,"
                + "ungrouped_visible INTEGER NOT NULL DEFAULT 0,"
                + "should_sync INTEGER NOT NULL DEFAULT 1"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS profile_display_photos ("
                + " data_id INTEGER PRIMARY KEY REFERENCES data(_id) ON DELETE CASCADE,"
                + " contents BLOB NOT NULL)");
        db.execSQL("CREATE TABLE IF NOT EXISTS profile_sync_state ("
                + " _id INTEGER PRIMARY KEY, account_name TEXT NOT NULL, account_type TEXT NOT NULL,"
                + " data BLOB, UNIQUE(account_name, account_type))");
        createIndexesAndViews(db);
    }

    private static void onUpgradeDatabase(SQLiteDatabase db, int oldVersion, int newVersion) {
        throw new IllegalStateException("No Contacts schema upgrade from " + oldVersion
                + " to " + newVersion);
    }

    private static void createIndexesAndViews(SQLiteDatabase db) {
        db.execSQL("CREATE INDEX IF NOT EXISTS contacts_lookup_idx ON contacts(lookup);");
        db.execSQL("CREATE UNIQUE INDEX IF NOT EXISTS contacts_single_profile"
                + " ON contacts(is_user_profile) WHERE is_user_profile = 1");
        db.execSQL("CREATE TABLE IF NOT EXISTS contact_row_ids ("
                + " table_name TEXT NOT NULL, profile INTEGER NOT NULL, last_id INTEGER NOT NULL,"
                + " PRIMARY KEY (table_name, profile))");
        db.execSQL("CREATE INDEX IF NOT EXISTS contacts_sort_key_idx ON contacts(sort_key);");
        db.execSQL("CREATE INDEX IF NOT EXISTS raw_contacts_contact_idx ON raw_contacts(contact_id);");
        db.execSQL("CREATE INDEX IF NOT EXISTS data_raw_contact_idx ON data(raw_contact_id);");
        db.execSQL("CREATE INDEX IF NOT EXISTS data_mimetype_idx ON data(mimetype_id);");
        db.execSQL("CREATE UNIQUE INDEX IF NOT EXISTS settings_account_idx ON settings("
                + "COALESCE(account_name, ''), COALESCE(account_type, ''), COALESCE(data_set, ''))");
        db.execSQL("CREATE TRIGGER IF NOT EXISTS data_update_version AFTER UPDATE ON data "
                + "WHEN new.data_version = old.data_version BEGIN "
                + "UPDATE data SET data_version = data_version + 1 WHERE _id = new._id; END");

        db.execSQL("CREATE VIEW IF NOT EXISTS view_contacts AS SELECT "
                + "_id, lookup, name_raw_contact_id, photo_id, photo_file_id, "
                + "photo_uri, photo_thumb_uri, display_name, display_name_alt, "
                + "display_name_source, phonetic_name, sort_key, sort_key_alt, "
                + "phonebook_label, phonebook_label_alt, phonebook_bucket, "
                + "phonebook_bucket_alt, in_visible_group, in_default_directory, "
                + "has_phone_number, times_contacted, times_used, "
                + "last_time_contacted, last_time_used, starred, pinned, "
                + "custom_ringtone, send_to_voicemail, contact_presence, "
                + "contact_status, contact_status_ts, contact_status_res_package, "
                + "contact_status_label, contact_status_icon, "
                + "contact_last_updated_timestamp, is_user_profile "
                + "FROM contacts WHERE EXISTS ("
                + "SELECT 1 FROM raw_contacts r WHERE r.contact_id = contacts._id AND r.deleted = 0"
                + ");");

        db.execSQL("CREATE VIEW IF NOT EXISTS view_raw_contacts AS SELECT "
                + "raw_contacts._id AS _id,"
                + "raw_contacts.contact_id AS contact_id,"
                + "raw_contacts.account_name AS account_name,"
                + "raw_contacts.account_type AS account_type,"
                + "raw_contacts.data_set AS data_set,"
                + "raw_contacts.sourceid AS sourceid,"
                + "raw_contacts.version AS version,"
                + "raw_contacts.dirty AS dirty,"
                + "raw_contacts.deleted AS deleted,"
                + "raw_contacts.aggregation_mode AS aggregation_mode,"
                + "raw_contacts.display_name AS display_name,"
                + "raw_contacts.display_name_alt AS display_name_alt,"
                + "raw_contacts.display_name_source AS display_name_source,"
                + "raw_contacts.phonetic_name AS phonetic_name,"
                + "raw_contacts.sort_key AS sort_key,"
                + "raw_contacts.sort_key_alt AS sort_key_alt,"
                + "raw_contacts.phonebook_label AS phonebook_label,"
                + "raw_contacts.phonebook_label_alt AS phonebook_label_alt,"
                + "raw_contacts.phonebook_bucket AS phonebook_bucket,"
                + "raw_contacts.phonebook_bucket_alt AS phonebook_bucket_alt,"
                + "raw_contacts.starred AS starred,"
                + "raw_contacts.pinned AS pinned,"
                + "raw_contacts.custom_ringtone AS custom_ringtone,"
                + "raw_contacts.send_to_voicemail AS send_to_voicemail,"
                + "raw_contacts.times_contacted AS times_contacted,"
                + "raw_contacts.times_used AS times_used,"
                + "raw_contacts.last_time_contacted AS last_time_contacted,"
                + "raw_contacts.last_time_used AS last_time_used,"
                + "raw_contacts.raw_contact_is_user_profile AS raw_contact_is_user_profile,"
                + "raw_contacts.sync1 AS sync1, raw_contacts.sync2 AS sync2,"
                + "raw_contacts.sync3 AS sync3, raw_contacts.sync4 AS sync4,"
                + "contacts.lookup AS lookup,"
                + "contacts.has_phone_number AS has_phone_number,"
                + "contacts.in_visible_group AS in_visible_group,"
                + "contacts.in_default_directory AS in_default_directory,"
                + "contacts.is_user_profile AS is_user_profile,"
                + "contacts.photo_id AS photo_id,"
                + "contacts.photo_file_id AS photo_file_id,"
                + "contacts.photo_uri AS photo_uri,"
                + "contacts.photo_thumb_uri AS photo_thumb_uri,"
                + "contacts.contact_presence AS contact_presence,"
                + "contacts.contact_status AS contact_status,"
                + "contacts.contact_last_updated_timestamp AS contact_last_updated_timestamp "
                + "FROM raw_contacts "
                + "LEFT JOIN contacts ON raw_contacts.contact_id = contacts._id;");

        db.execSQL("CREATE VIEW IF NOT EXISTS view_data AS SELECT "
                + "data._id AS _id,"
                + "data.raw_contact_id AS raw_contact_id,"
                + "raw_contacts.contact_id AS contact_id,"
                + "contacts.lookup AS lookup,"
                + "mimetypes.mimetype AS mimetype,"
                + "data.is_primary AS is_primary,"
                + "data.is_super_primary AS is_super_primary,"
                + "data.is_read_only AS is_read_only,"
                + "data.data_version AS data_version,"
                + "data.data1 AS data1, data.data2 AS data2, data.data3 AS data3,"
                + "data.data4 AS data4, data.data5 AS data5, data.data6 AS data6,"
                + "data.data7 AS data7, data.data8 AS data8, data.data9 AS data9,"
                + "data.data10 AS data10, data.data11 AS data11, data.data12 AS data12,"
                + "data.data13 AS data13,"
                + "CASE WHEN raw_contacts.raw_contact_is_user_profile = 1 AND mimetypes.mimetype = '"
                + MIME_PHOTO + "' THEN profile_display_photos.data_id ELSE data.data14 END AS data14,"
                + "data.data15 AS data15,"
                + "data.data_sync1 AS data_sync1, data.data_sync2 AS data_sync2,"
                + "data.data_sync3 AS data_sync3, data.data_sync4 AS data_sync4,"
                + "data.carrier_presence AS carrier_presence,"
                + "contacts.display_name AS display_name,"
                + "contacts.display_name_alt AS display_name_alt,"
                + "contacts.display_name_source AS display_name_source,"
                + "contacts.phonetic_name AS phonetic_name,"
                + "contacts.sort_key AS sort_key,"
                + "contacts.sort_key_alt AS sort_key_alt,"
                + "contacts.phonebook_label AS phonebook_label,"
                + "contacts.phonebook_label_alt AS phonebook_label_alt,"
                + "contacts.phonebook_bucket AS phonebook_bucket,"
                + "contacts.phonebook_bucket_alt AS phonebook_bucket_alt,"
                + "contacts.photo_id AS photo_id,"
                + "contacts.photo_file_id AS photo_file_id,"
                + "contacts.photo_uri AS photo_uri,"
                + "contacts.photo_thumb_uri AS photo_thumb_uri,"
                + "contacts.in_visible_group AS in_visible_group,"
                + "contacts.in_default_directory AS in_default_directory,"
                + "contacts.has_phone_number AS has_phone_number,"
                + "contacts.starred AS starred,"
                + "contacts.pinned AS pinned,"
                + "contacts.custom_ringtone AS custom_ringtone,"
                + "contacts.send_to_voicemail AS send_to_voicemail,"
                + "contacts.times_contacted AS times_contacted,"
                + "contacts.times_used AS times_used,"
                + "contacts.last_time_contacted AS last_time_contacted,"
                + "contacts.last_time_used AS last_time_used,"
                + "contacts.contact_presence AS contact_presence,"
                + "contacts.contact_status AS contact_status,"
                + "contacts.contact_status_ts AS contact_status_ts,"
                + "contacts.contact_last_updated_timestamp AS contact_last_updated_timestamp,"
                + "raw_contacts.account_name AS account_name,"
                + "raw_contacts.account_type AS account_type,"
                + "raw_contacts.data_set AS data_set,"
                + "raw_contacts.sourceid AS sourceid,"
                + "raw_contacts.version AS version,"
                + "raw_contacts.dirty AS dirty,"
                + "raw_contacts.deleted AS deleted,"
                + "raw_contacts.raw_contact_is_user_profile AS raw_contact_is_user_profile,"
                + "contacts.is_user_profile AS is_user_profile,"
                + "contacts.name_raw_contact_id AS name_raw_contact_id "
                + "FROM data "
                + "LEFT JOIN profile_display_photos ON profile_display_photos.data_id = data._id "
                + "JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                + "JOIN raw_contacts ON data.raw_contact_id = raw_contacts._id "
                + "LEFT JOIN contacts ON raw_contacts.contact_id = contacts._id "
                + "WHERE raw_contacts.deleted = 0;");

        db.execSQL("CREATE VIEW IF NOT EXISTS view_phone_lookup AS SELECT "
                + "contacts._id AS _id,"
                + "contacts._id AS contact_id,"
                + "raw_contacts._id AS raw_contact_id,"
                + "data._id AS data_id,"
                + "contacts.lookup AS lookup,"
                + "contacts.display_name AS display_name,"
                + "contacts.display_name_alt AS display_name_alt,"
                + "contacts.sort_key AS sort_key,"
                + "contacts.sort_key_alt AS sort_key_alt,"
                + "contacts.phonebook_label AS phonebook_label,"
                + "contacts.phonebook_bucket AS phonebook_bucket,"
                + "data.data1 AS number,"
                + "data.data2 AS type,"
                + "data.data3 AS label,"
                + "data.data4 AS normalized_number,"
                + "contacts.photo_id AS photo_id,"
                + "contacts.photo_file_id AS photo_file_id,"
                + "contacts.photo_uri AS photo_uri,"
                + "contacts.photo_thumb_uri AS photo_thumb_uri,"
                + "contacts.starred AS starred,"
                + "contacts.pinned AS pinned,"
                + "contacts.custom_ringtone AS custom_ringtone,"
                + "contacts.send_to_voicemail AS send_to_voicemail,"
                + "contacts.in_visible_group AS in_visible_group,"
                + "contacts.in_default_directory AS in_default_directory,"
                + "contacts.has_phone_number AS has_phone_number,"
                + "contacts.times_contacted AS times_contacted,"
                + "contacts.times_used AS times_used,"
                + "contacts.last_time_contacted AS last_time_contacted,"
                + "contacts.last_time_used AS last_time_used,"
                + "contacts.contact_presence AS contact_presence,"
                + "contacts.contact_status AS contact_status,"
                + "contacts.contact_last_updated_timestamp AS contact_last_updated_timestamp "
                + "FROM data "
                + "JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                + "JOIN raw_contacts ON data.raw_contact_id = raw_contacts._id "
                + "JOIN contacts ON raw_contacts.contact_id = contacts._id "
                + "WHERE mimetypes.mimetype = '" + MIME_PHONE + "' AND raw_contacts.deleted = 0 "
                + "AND contacts.is_user_profile = 0;");

        createEntityView(db, "view_contacts_entity", sContactsEntityProjectionMap, true);
        createEntityView(db, "view_raw_entities", sRawEntityProjectionMap, false);
        String membership = " FROM view_data member WHERE member.mimetype = '"
                + ContactsContract.CommonDataKinds.GroupMembership.CONTENT_ITEM_TYPE
                + "' AND member.data1 = CAST(groups._id AS TEXT) AND member.is_user_profile = 0";
        db.execSQL("CREATE VIEW IF NOT EXISTS view_groups_summary AS SELECT groups.*, "
                + "(SELECT COUNT(DISTINCT member.contact_id)" + membership + ") AS summ_count, "
                + "(SELECT COUNT(DISTINCT member.contact_id)" + membership
                + " AND member.has_phone_number != 0) AS summ_phones FROM groups");
        String ungrouped = " FROM raw_contacts raw JOIN contacts c ON c._id = raw.contact_id"
                + " WHERE " + accountMatchesSettings("raw")
                + " AND raw.deleted = 0 AND c.is_user_profile = 0 AND NOT EXISTS ("
                + "SELECT 1 FROM view_data member JOIN groups g ON member.data1 = CAST(g._id AS TEXT)"
                + " WHERE member.raw_contact_id = raw._id AND g.deleted = 0 AND member.mimetype = '"
                + ContactsContract.CommonDataKinds.GroupMembership.CONTENT_ITEM_TYPE + "')";
        db.execSQL("CREATE VIEW IF NOT EXISTS view_settings AS SELECT settings.*, "
                + "(settings.should_sync = 0 OR EXISTS (SELECT 1 FROM groups g WHERE g.deleted = 0"
                + " AND g.should_sync = 0 AND " + accountMatchesSettings("g") + ")) AS any_unsynced,"
                + "(SELECT COUNT(DISTINCT c._id)" + ungrouped + ") AS summ_count, "
                + "(SELECT COUNT(DISTINCT c._id)" + ungrouped
                + " AND c.has_phone_number != 0) AS summ_phones FROM settings");
    }

    private static String accountMatchesSettings(String alias) {
        return alias + ".account_name IS settings.account_name AND "
                + alias + ".account_type IS settings.account_type AND "
                + alias + ".data_set IS settings.data_set";
    }

    private static void createEntityView(SQLiteDatabase db, String view, Map<String, String> columns,
            boolean aggregate) {
        List<String> projections = new ArrayList<>();
        for (String name : new TreeSet<>(columns.keySet())) {
            String value;
            switch (name) {
                case "_id":
                    value = aggregate ? "raw.contact_id" : "raw._id";
                    break;
                case "raw_contact_id":
                    value = "raw._id";
                    break;
                case "data_id":
                    value = "data._id";
                    break;
                default:
                    if (aggregate && sContactsProjectionMap.containsKey(name)) {
                        value = "contact." + name;
                    } else if (sRawContactsProjectionMap.containsKey(name)) {
                        value = "raw." + name;
                    } else {
                        value = "data." + name;
                    }
                    break;
            }
            projections.add(value + " AS " + name);
        }
        db.execSQL("CREATE VIEW IF NOT EXISTS " + view + " AS SELECT "
                + String.join(", ", projections) + " FROM view_raw_contacts raw "
                + "LEFT JOIN view_data data ON data.raw_contact_id = raw._id "
                + (aggregate ? "JOIN view_contacts contact ON contact._id = raw.contact_id " : ""));
    }

    private static boolean hasProfilePath(Uri uri) {
        List<String> segments = uri.getPathSegments();
        return !segments.isEmpty() && "profile".equals(segments.get(0));
    }

    private static String itemId(Uri uri, int match) {
        switch (match) {
            case CONTACTS_LOOKUP_ID:
                return uri.getPathSegments().get(3);
            case CONTACT_THUMBNAIL:
            case CONTACT_DISPLAY_PHOTO: {
                List<String> segments = uri.getPathSegments();
                if (hasProfilePath(uri)) return null;
                if ("lookup".equals(segments.get(1))) {
                    return segments.size() == 5 ? segments.get(3) : null;
                }
                return segments.get(1);
            }
            case CONTACTS_ENTITY:
                if (hasProfilePath(uri)) return null;
                // A contact ID precedes the entity path segment.
                return uri.getPathSegments().get(1);
            case CONTACTS_ID:
            case CONTACTS_DATA:
            case RAW_CONTACTS_ID:
            case RAW_CONTACTS_DATA:
            case RAW_CONTACT_ENTITY:
            case RAW_DISPLAY_PHOTO:
            case DATA_ID:
            case PROFILE_SYNC_STATE_ID:
                return uri.getPathSegments().get(hasProfilePath(uri) ? 2 : 1);
            case PHONES_ID:
            case EMAILS_ID:
            case POSTALS_ID:
            case DISPLAY_PHOTO_FILE:
                return uri.getLastPathSegment();
            default:
                return null;
        }
    }

    private static boolean isProfileRequest(Uri uri, int match) {
        if (hasProfilePath(uri)) return true;
        String id = itemId(uri, match);
        if (id != null) return isProfileId(id);
        if (match == CONTACTS_LOOKUP || match == PROFILE_VCARD) {
            return PROFILE_LOOKUP.equals(uri.getPathSegments().get(2));
        }
        if (match == CONTACT_THUMBNAIL || match == CONTACT_DISPLAY_PHOTO) {
            List<String> segments = uri.getPathSegments();
            return segments.size() == 4 && PROFILE_LOOKUP.equals(segments.get(2));
        }
        return false;
    }

    private static boolean isProfileId(String id) {
        return id != null && ContactsContract.isProfileId(Long.parseLong(id));
    }

    /**
     * Contacts and Profile have disjoint ID ranges. Store the last ID in the
     * transaction that inserts the row, so successful IDs stay monotonic even
     * after deletion and a Profile row never advances the ordinary allocator.
     */
    private static long insertContactRow(SQLiteDatabase db, String table, boolean profile,
            ContentValues values) {
        String range = profile ? "1" : "0";
        long last = profile ? ContactsContract.Profile.MIN_ID - 1 : 0;
        try (Cursor c = db.rawQuery("SELECT last_id FROM contact_row_ids"
                + " WHERE table_name = ? AND profile = ?", new String[] {table, range})) {
            if (c.moveToFirst()) last = c.getLong(0);
        }
        if (last == (profile ? Long.MAX_VALUE : ContactsContract.Profile.MIN_ID - 1)) {
            throw new IllegalStateException("Contact ID range exhausted");
        }
        long id = last + 1;
        ContentValues sequence = new ContentValues();
        sequence.put("table_name", table);
        sequence.put("profile", profile ? 1 : 0);
        sequence.put("last_id", id);
        db.replaceOrThrow("contact_row_ids", null, sequence);
        values.put("_id", id);
        return db.insertOrThrow(table, null, values);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        switch (sUriMatcher.match(uri)) {
            case CONTACTS:
            case CONTACTS_FILTER_EMPTY:
            case CONTACTS_FILTER:
                return ContactsContract.Contacts.CONTENT_TYPE;
            case CONTACTS_ID:
            case CONTACTS_LOOKUP:
            case CONTACTS_LOOKUP_ID:
            case PROFILE:
                return ContactsContract.Contacts.CONTENT_ITEM_TYPE;
            case CONTACTS_DATA:
            case RAW_CONTACTS_DATA:
            case DATA:
                return ContactsContract.Data.CONTENT_TYPE;
            case CONTACTS_ENTITY:
            case RAW_CONTACT_ENTITY:
            case RAW_CONTACT_ENTITIES:
                return ContactsContract.RawContactsEntity.CONTENT_TYPE;
            case DATA_ID:
                if (isProfileRequest(uri, DATA_ID)) {
                    synchronized (mDbLock) {
                        try (Cursor c = openDatabase().rawQuery(
                                "SELECT mimetype FROM view_data WHERE _id = ? AND is_user_profile = 1",
                                new String[] {itemId(uri, DATA_ID)})) {
                            return c.moveToFirst() ? c.getString(0) : null;
                        }
                    }
                }
                return "vnd.android.cursor.item/data";
            case RAW_CONTACTS:
                return ContactsContract.RawContacts.CONTENT_TYPE;
            case RAW_CONTACTS_ID:
                return ContactsContract.RawContacts.CONTENT_ITEM_TYPE;
            case PHONES:
            case PHONES_FILTER_EMPTY:
            case PHONES_FILTER:
                return ContactsContract.CommonDataKinds.Phone.CONTENT_TYPE;
            case PHONES_ID:
                return ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE;
            case EMAILS:
            case EMAILS_FILTER_EMPTY:
            case EMAILS_FILTER:
                return ContactsContract.CommonDataKinds.Email.CONTENT_TYPE;
            case EMAILS_ID:
                return ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE;
            case POSTALS:
                return ContactsContract.CommonDataKinds.StructuredPostal.CONTENT_TYPE;
            case POSTALS_ID:
                return ContactsContract.CommonDataKinds.StructuredPostal.CONTENT_ITEM_TYPE;
            case PHONE_LOOKUP:
                return ContactsContract.Contacts.CONTENT_TYPE;
            case GROUPS:
            case GROUPS_SUMMARY:
                return ContactsContract.Groups.CONTENT_TYPE;
            case GROUPS_ID:
                return ContactsContract.Groups.CONTENT_ITEM_TYPE;
            case DELETED_CONTACTS:
                return "vnd.android.cursor.dir/deleted_contacts";
            case DIRECTORIES:
                return ContactsContract.Directory.CONTENT_TYPE;
            case DIRECTORIES_ID:
                return ContactsContract.Directory.CONTENT_ITEM_TYPE;
            case SETTINGS:
                return ContactsContract.Settings.CONTENT_TYPE;
            case PROFILE_SYNC_STATE:
            case PROFILE_SYNC_STATE_ID:
                return "vnd.android.cursor.item/syncstate";
            case PROFILE_VCARD:
                if (isProfileRequest(uri, PROFILE_VCARD)) return ContactsContract.Contacts.CONTENT_VCARD_TYPE;
                throw new IllegalArgumentException("Unknown URL " + uri);
            case CONTACT_THUMBNAIL:
            case CONTACT_DISPLAY_PHOTO:
            case RAW_DISPLAY_PHOTO:
            case DISPLAY_PHOTO_FILE:
                if (isProfileRequest(uri, sUriMatcher.match(uri))) return "image/jpeg";
                throw new IllegalArgumentException("Unknown URL " + uri);
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        String directory = uri.getQueryParameter(ContactsContract.DIRECTORY_PARAM_KEY);
        boolean localInvisible = false;
        if (directory != null) {
            try {
                long value = Long.parseLong(directory);
                if (value != ContactsContract.Directory.DEFAULT
                        && value != ContactsContract.Directory.LOCAL_INVISIBLE) return null;
                localInvisible = value == ContactsContract.Directory.LOCAL_INVISIBLE;
            } catch (NumberFormatException ignored) {
                return null;
            }
        }
        int match = sUriMatcher.match(uri);
        if (match == PHOTO_DIMENSIONS) return profilePhotoDimensions(projection);
        boolean profile = isProfileRequest(uri, match);
        String id = itemId(uri, match);
        String limit = ContactsProviderSupport.parseLimit(uri);
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        String effSelection = selection;
        String[] effArgs = selectionArgs;
        String phoneLookupQuery = null;
        boolean syncAdapter = isCallerSyncAdapter(uri);

        switch (match) {
            case CONTACTS:
            case CONTACTS_FILTER_EMPTY:
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = scopedSelection(selection, "is_user_profile = 0");
                break;
            case CONTACTS_ID:
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = scopedSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, id);
                break;
            case CONTACTS_FILTER: {
                String pattern = "%" + uri.getLastPathSegment() + "%";
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = scopedSelection(selection,
                        "is_user_profile = 0 AND display_name LIKE ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, pattern);
                break;
            }
            case CONTACTS_LOOKUP: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = scopedSelection(selection, "lookup = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, segments.get(2));
                break;
            }
            case CONTACTS_LOOKUP_ID: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = scopedSelection(selection, "lookup = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, segments.get(2));
                break;
            }
            case CONTACTS_DATA: {
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "contact_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
                break;
            }
            case RAW_CONTACTS:
                qb.setTables("view_raw_contacts");
                qb.setProjectionMap(sRawContactsProjectionMap);
                if (!syncAdapter) {
                    effSelection = scopedSelection(selection, "deleted = 0");
                }
                break;
            case RAW_CONTACTS_ID:
                qb.setTables("view_raw_contacts");
                qb.setProjectionMap(sRawContactsProjectionMap);
                effSelection = scopedSelection(selection,
                        syncAdapter ? "_id = ?" : "_id = ? AND deleted = 0");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, id);
                break;
            case RAW_CONTACTS_DATA: {
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "raw_contact_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
                break;
            }
            case CONTACTS_ENTITY:
                qb.setTables("view_contacts_entity");
                qb.setProjectionMap(sContactsEntityProjectionMap);
                qb.appendWhereStandalone("deleted = 0");
                if (id != null) {
                    effSelection = scopedSelection(selection, "contact_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
                }
                if (sortOrder == null) sortOrder = "raw_contact_id, data_id";
                break;
            case RAW_CONTACT_ENTITY:
            case RAW_CONTACT_ENTITIES:
                qb.setTables("view_raw_entities");
                qb.setProjectionMap(sRawEntityProjectionMap);
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                if (match == RAW_CONTACT_ENTITY) {
                    effSelection = scopedSelection(selection, "raw_contact_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
                }
                if (sortOrder == null) sortOrder = "raw_contact_id, data_id";
                break;
            case DATA:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                break;
            case DATA_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, id);
                break;
            case PHONES:
            case PHONES_FILTER_EMPTY:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "mimetype = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_PHONE);
                break;
            case PHONES_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "mimetype = ? AND _id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_PHONE, id);
                break;
            case PHONES_FILTER: {
                String pattern = "%" + uri.getLastPathSegment() + "%";
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection,
                        "mimetype = ? AND (display_name LIKE ? OR data1 LIKE ? OR data4 LIKE ?)");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_PHONE, pattern, pattern, pattern);
                break;
            }
            case EMAILS:
            case EMAILS_FILTER_EMPTY:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "mimetype = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_EMAIL);
                break;
            case EMAILS_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "mimetype = ? AND _id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_EMAIL, id);
                break;
            case EMAILS_FILTER: {
                String pattern = "%" + uri.getLastPathSegment() + "%";
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection,
                        "mimetype = ? AND (display_name LIKE ? OR data1 LIKE ?)");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_EMAIL, pattern, pattern);
                break;
            }
            case POSTALS:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "mimetype = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_POSTAL);
                break;
            case POSTALS_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = scopedSelection(selection, "mimetype = ? AND _id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_POSTAL, id);
                break;
            case PHONE_LOOKUP:
                qb.setTables("view_phone_lookup");
                qb.setProjectionMap(sPhoneLookupProjectionMap);
                phoneLookupQuery = uri.getLastPathSegment();
                break;
            case GROUPS:
            case GROUPS_SUMMARY:
                qb.setTables(match == GROUPS_SUMMARY ? "view_groups_summary" : "groups");
                qb.setProjectionMap(match == GROUPS_SUMMARY ? sGroupsSummaryProjectionMap : sGroupsProjectionMap);
                if (!syncAdapter) {
                    effSelection = scopedSelection(selection, "deleted = 0");
                }
                break;
            case GROUPS_ID:
                qb.setTables("groups");
                qb.setProjectionMap(sGroupsProjectionMap);
                effSelection = scopedSelection(selection,
                        syncAdapter ? "_id = ?" : "_id = ? AND deleted = 0");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case DELETED_CONTACTS:
                qb.setTables("deleted_contacts");
                qb.setProjectionMap(sDeletedContactsProjectionMap);
                break;
            case DIRECTORIES:
                qb.setTables("directories");
                qb.setProjectionMap(sDirectoriesProjectionMap);
                break;
            case DIRECTORIES_ID:
                qb.setTables("directories");
                qb.setProjectionMap(sDirectoriesProjectionMap);
                effSelection = scopedSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case SETTINGS:
                qb.setTables("view_settings");
                qb.setProjectionMap(sSettingsProjectionMap);
                break;
            case PROFILE:
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                break;
            case PROFILE_VCARD:
                if (!profile) throw new IllegalArgumentException("Unknown URL " + uri);
                qb.setTables("view_contacts");
                qb.setProjectionMap(PROFILE_VCARD_COLUMNS);
                break;
            case CONTACT_THUMBNAIL:
                if (!profile) throw new IllegalArgumentException("Unknown URL " + uri);
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                qb.appendWhereStandalone("_id = photo_id");
                if (id != null) {
                    effSelection = scopedSelection(selection, "contact_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
                }
                break;
            case PROFILE_SYNC_STATE:
            case PROFILE_SYNC_STATE_ID:
                qb.setTables("profile_sync_state");
                qb.setProjectionMap(PROFILE_SYNC_COLUMNS);
                if (id != null) {
                    effSelection = scopedSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
                }
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }

        String scope = profileColumn(qb.getTables());
        if (scope != null) qb.appendWhereStandalone(scope + (profile ? " = 1" : " = 0"));
        if (localInvisible) qb.appendWhereStandalone("0");
        if (qb.getProjectionMap().containsKey("account_name")) {
            AccountFilter account = AccountFilter.fromUri(uri);
            if (account != null && (match == PROFILE_SYNC_STATE || match == PROFILE_SYNC_STATE_ID)) {
                qb.appendWhereStandalone("account_name = ? AND account_type = ?");
                effArgs = ContactsProviderSupport.prependArgs(effArgs, account.name, account.type);
            } else if (account != null) {
                qb.appendWhereStandalone(account.selection());
                effArgs = account.withArgs(effArgs);
            }
        }
        Cursor cursor;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            if (phoneLookupQuery != null) {
                List<Long> matchedDataIds = findMatchingPhoneDataIds(db, phoneLookupQuery);
                String idClause = idInClause("data_id", matchedDataIds);
                effSelection = scopedSelection(effSelection, idClause);
            }
            cursor = qb.query(db, projection, effSelection, effArgs,
                    null, null, sortOrder, limit);
        }
        try {
            ContactsProviderSupport.setNotificationUri(cursor, getContext(), uri);
            return cursor;
        } catch (RuntimeException | Error failure) {
            cursor.close();
            throw failure;
        }
    }

    private static List<Long> findMatchingPhoneDataIds(SQLiteDatabase db, String queryNumber) {
        List<Long> dataIds = new ArrayList<>();
        if (queryNumber == null || queryNumber.isEmpty()) return dataIds;
        try (Cursor c = db.rawQuery(
                "SELECT data_id, number, normalized_number FROM view_phone_lookup", null)) {
            while (c.moveToNext()) {
                long dataId = c.getLong(0);
                String number = c.getString(1);
                String normalized = c.getString(2);
                if (PhoneNumberUtils.compareLoosely(number, queryNumber)
                        || PhoneNumberUtils.compareLoosely(normalized, queryNumber)) {
                    dataIds.add(dataId);
                }
            }
        }
        return dataIds;
    }

    private static Cursor profilePhotoDimensions(String[] projection) {
        String[] columns = projection == null ? new String[] {
                ContactsContract.DisplayPhoto.DISPLAY_MAX_DIM,
                ContactsContract.DisplayPhoto.THUMBNAIL_MAX_DIM
        } : projection;
        Object[] dimensions = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            dimensions[i] = switch (columns[i]) {
                case ContactsContract.DisplayPhoto.DISPLAY_MAX_DIM -> ProfilePhoto.DISPLAY_DIMENSION;
                case ContactsContract.DisplayPhoto.THUMBNAIL_MAX_DIM -> ProfilePhoto.THUMBNAIL_DIMENSION;
                default -> throw new IllegalArgumentException("Invalid column " + columns[i]);
            };
        }
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        cursor.addRow(dimensions);
        return cursor;
    }

    private static String profileColumn(String table) {
        switch (table) {
            case "view_raw_contacts":
            case "view_raw_entities":
                return "raw_contact_is_user_profile";
            case "view_contacts":
            case "view_contacts_entity":
            case "view_data":
                return "is_user_profile";
            default:
                return null;
        }
    }

    private static String scopedSelection(String selection, String scope) {
        return DatabaseUtils.concatenateWhere(scope, selection);
    }

    private static final class AccountFilter {
        final String name;
        final String type;
        final String dataSet;

        private AccountFilter(String name, String type, String dataSet) {
            this.name = name;
            this.type = type;
            this.dataSet = dataSet;
        }

        static AccountFilter fromUri(Uri uri) {
            return fromIdentity(uri.getQueryParameter(ContactsContract.RawContacts.ACCOUNT_NAME),
                    uri.getQueryParameter(ContactsContract.RawContacts.ACCOUNT_TYPE),
                    uri.getQueryParameter(ContactsContract.RawContacts.DATA_SET));
        }

        static AccountFilter fromValues(ContentValues values) {
            return fromIdentity(values.getAsString("account_name"), values.getAsString("account_type"),
                    values.getAsString("data_set"));
        }

        private static AccountFilter fromIdentity(String name, String type, String dataSet) {
            boolean hasName = name != null && !name.isEmpty();
            boolean hasType = type != null && !type.isEmpty();
            if (hasName != hasType) {
                throw new IllegalArgumentException("Specify both account name and account type");
            }
            if (!hasName) return null;
            return new AccountFilter(name, type, dataSet == null || dataSet.isEmpty() ? null : dataSet);
        }

        String selection() {
            return "account_name = ? AND account_type = ? AND "
                    + (dataSet == null ? "data_set IS NULL" : "data_set = ?");
        }

        String[] withArgs(String[] args) {
            return dataSet == null ? ContactsProviderSupport.prependArgs(args, name, type)
                    : ContactsProviderSupport.prependArgs(args, name, type, dataSet);
        }

        void applyTo(ContentValues values) {
            setValue(values, "account_name", name);
            setValue(values, "account_type", type);
            setValue(values, "data_set", dataSet);
        }

        Uri settingsUri() {
            Uri.Builder uri = ContactsContract.Settings.CONTENT_URI.buildUpon()
                    .appendQueryParameter(ContactsContract.RawContacts.ACCOUNT_NAME, name)
                    .appendQueryParameter(ContactsContract.RawContacts.ACCOUNT_TYPE, type);
            if (dataSet != null) uri.appendQueryParameter(ContactsContract.RawContacts.DATA_SET, dataSet);
            return uri.build();
        }

        private static void setValue(ContentValues values, String column, String expected) {
            String supplied = values.getAsString(column);
            if ("data_set".equals(column) && "".equals(supplied)) supplied = null;
            if (values.containsKey(column) && !Objects.equals(supplied, expected)) {
                throw new IllegalArgumentException("Conflicting account " + column);
            }
            values.put(column, expected);
        }
    }

    private static String idInClause(String column, Collection<Long> ids) {
        if (ids.isEmpty()) return "1 = 0";
        StringBuilder sb = new StringBuilder(column).append(" IN (");
        boolean first = true;
        for (long id : ids) {
            if (!first) sb.append(',');
            sb.append(id);
            first = false;
        }
        sb.append(')');
        return sb.toString();
    }

    @Override
    public ContentProviderResult[] applyBatch(String authority,
            ArrayList<ContentProviderOperation> operations) throws OperationApplicationException {
        if (authority != null && !ContactsProviderSupport.CONTACTS_AUTHORITY.equals(authority)) {
            throw new SecurityException("Unsupported Contacts authority in applyBatch: " + authority);
        }
        return applyBatch(operations);
    }

    @Override
    public ContentProviderResult[] applyBatch(ArrayList<ContentProviderOperation> operations)
            throws OperationApplicationException {
        if (operations == null || operations.isEmpty()) {
            return new ContentProviderResult[0];
        }
        ContentProviderResult[] results;
        boolean shouldNotify = false;
        boolean shouldSync = false;
        boolean profileChanged = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            boolean outermost = (mBatchDepth == 0);
            if (outermost) {
                mBatchDirty = false;
                mBatchSyncToNetwork = false;
                mBatchProfileDirty = false;
            }
            mBatchDepth++;
            boolean committed = false;
            try {
                results = super.applyBatch(operations);
                db.setTransactionSuccessful();
                committed = true;
            } finally {
                try {
                    db.endTransaction();
                } finally {
                    mBatchDepth--;
                    if (outermost) {
                        shouldNotify = committed && mBatchDirty;
                        shouldSync = mBatchSyncToNetwork;
                        profileChanged = mBatchProfileDirty;
                        mBatchDirty = false;
                        mBatchSyncToNetwork = false;
                        mBatchProfileDirty = false;
                    }
                }
            }
        }
        if (shouldNotify) {
            notifyObservers(shouldSync, profileChanged);
        }
        return results;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        Uri result;
        boolean profileChanged = false;
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                result = insertLocked(db, uri, values);
                db.setTransactionSuccessful();
                if (result != null) {
                    profileChanged = isProfileRequest(uri, sUriMatcher.match(uri))
                            || isProfileRequest(result, sUriMatcher.match(result));
                    notifyNow = markMutation(uri, profileChanged);
                }
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            notifyObservers(shouldSyncToNetwork(uri), profileChanged);
        }
        return result;
    }

    @Override
    public int bulkInsert(Uri uri, ContentValues[] values) {
        if (values == null || values.length == 0) return 0;
        int inserted = 0;
        boolean profileChanged = isProfileRequest(uri, sUriMatcher.match(uri));
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                for (ContentValues item : values) {
                    Uri result = insertLocked(db, uri, item);
                    if (result != null) {
                        inserted++;
                        profileChanged |= isProfileRequest(result, sUriMatcher.match(result));
                    }
                }
                db.setTransactionSuccessful();
                if (inserted > 0) notifyNow = markMutation(uri, profileChanged);
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            notifyObservers(shouldSyncToNetwork(uri), profileChanged);
        }
        return inserted;
    }

    private Uri insertLocked(SQLiteDatabase db, Uri uri, ContentValues values) {
        int match = sUriMatcher.match(uri);
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        long now = System.currentTimeMillis();
        boolean syncAdapter = isCallerSyncAdapter(uri);
        AccountFilter account = AccountFilter.fromUri(uri);
        switch (match) {
            case PROFILE:
                throw new UnsupportedOperationException("The profile contact is created automatically");
            case RAW_CONTACTS: {
                boolean isProfile = hasProfilePath(uri);
                copy.remove("contact_id");
                requireKeys(copy, RAW_WRITE_COLUMNS);
                if (account != null) account.applyTo(copy);
                long contactId;
                if (isProfile) {
                    copy.put("raw_contact_is_user_profile", 1);
                    Integer deleted = copy.getAsInteger("deleted");
                    if (deleted == null || deleted == 0) {
                        contactId = ensureProfileContact(db, now);
                        copy.put("contact_id", contactId);
                    } else {
                        contactId = 0;
                        copy.putNull("contact_id");
                        copy.put("aggregation_mode", ContactsContract.RawContacts.AGGREGATION_MODE_DISABLED);
                    }
                } else {
                    ContentValues contactValues = new ContentValues();
                    contactValues.put("contact_last_updated_timestamp", now);
                    contactId = insertContactRow(db, "contacts", false, contactValues);
                    copy.put("contact_id", contactId);
                }
                if (!syncAdapter && !copy.containsKey("dirty")) {
                    copy.put("dirty", 1);
                }
                long rawId = insertContactRow(db, "raw_contacts", isProfile, copy);
                if (!isProfile) ensureSettingsRow(db, copy);
                if (contactId == 0) return ContentUris.withAppendedId(uri, rawId);
                ContentValues contactUpdate = new ContentValues();
                contactUpdate.put("lookup", isProfile ? PROFILE_LOOKUP : "0r" + rawId);
                contactUpdate.put("name_raw_contact_id", rawId);
                contactUpdate.put("contact_last_updated_timestamp", now);
                db.update("contacts", contactUpdate, "_id = ? AND lookup IS NULL",
                        new String[] { Long.toString(contactId) });
                return ContentUris.withAppendedId(uri, rawId);
            }
            case RAW_CONTACTS_DATA: {
                String id = itemId(uri, match);
                copy.put("raw_contact_id", Long.parseLong(id));
                return ContentUris.withAppendedId(dataCollection(uri, match),
                        insertDataLocked(db, copy, null, account, isProfileRequest(uri, match),
                                now, syncAdapter));
            }
            case DATA:
            case PHONES:
            case EMAILS:
            case POSTALS:
                boolean profile = hasProfilePath(uri)
                        || isProfileId(copy.getAsString("raw_contact_id"));
                return ContentUris.withAppendedId(uri,
                        insertDataLocked(db, copy, forcedMime(match), account, profile, now, syncAdapter));
            case GROUPS:
                requireKeys(copy, GROUP_WRITE_COLUMNS);
                if (account != null) account.applyTo(copy);
                if (!syncAdapter && !copy.containsKey("dirty")) copy.put("dirty", 1);
                ensureSettingsRow(db, copy);
                return ContentUris.withAppendedId(uri, db.insertOrThrow("groups", null, copy));
            case SETTINGS:
                return insertSettings(db, copy, account);
            case PROFILE_SYNC_STATE:
                return insertProfileSyncState(db, uri, copy);
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private static Uri insertProfileSyncState(SQLiteDatabase db, Uri uri, ContentValues values) {
        requireKeys(values, Set.of("account_name", "account_type", "data"));
        AccountFilter account = AccountFilter.fromValues(values);
        if (account == null) throw new IllegalArgumentException("Sync state requires an account");
        String[] args = {account.name, account.type};
        try (Cursor c = db.rawQuery("SELECT _id FROM profile_sync_state "
                + "WHERE account_name = ? AND account_type = ?", args)) {
            if (c.moveToFirst()) {
                long id = c.getLong(0);
                db.update("profile_sync_state", values, "_id = ?", new String[] {Long.toString(id)});
                return ContentUris.withAppendedId(uri, id);
            }
        }
        return ContentUris.withAppendedId(uri, insertContactRow(db, "profile_sync_state", true, values));
    }

    private static long findProfileId(SQLiteDatabase db) {
        try (Cursor c = db.rawQuery("SELECT _id FROM contacts WHERE is_user_profile = 1", null)) {
            return c.moveToFirst() ? c.getLong(0) : 0;
        }
    }

    private static long ensureProfileContact(SQLiteDatabase db, long now) {
        long id = findProfileId(db);
        if (id != 0) return id;
        ContentValues contact = new ContentValues();
        contact.put("is_user_profile", 1);
        contact.put("contact_last_updated_timestamp", now);
        return insertContactRow(db, "contacts", true, contact);
    }

    private static void reconcileProfileContacts(SQLiteDatabase db, long now) {
        ContentValues detached = new ContentValues();
        detached.putNull("contact_id");
        detached.put("aggregation_mode", ContactsContract.RawContacts.AGGREGATION_MODE_DISABLED);
        db.update("raw_contacts", detached, "raw_contact_is_user_profile = 1 AND deleted != 0", null);
        Set<Long> remaining = queryIds(db,
                "SELECT _id FROM raw_contacts WHERE raw_contact_is_user_profile = 1 AND deleted = 0", null);
        if (remaining.isEmpty()) {
            db.delete("contacts", "is_user_profile = 1", null);
            return;
        }
        long contactId = ensureProfileContact(db, now);
        ContentValues linked = new ContentValues();
        linked.put("contact_id", contactId);
        db.update("raw_contacts", linked,
                "raw_contact_is_user_profile = 1 AND deleted = 0 AND contact_id IS NULL", null);
        ContentValues lookup = new ContentValues();
        lookup.put("lookup", PROFILE_LOOKUP);
        db.update("contacts", lookup, "_id = ?", new String[] {Long.toString(contactId)});
        refreshRawAndParentContacts(db, remaining, now, false);
    }

    private static Uri dataCollection(Uri uri, int match) {
        if (!isProfileRequest(uri, match)) return uri;
        return Uri.withAppendedPath(ContactsContract.Profile.CONTENT_URI, "data")
                .buildUpon().encodedQuery(uri.getEncodedQuery()).build();
    }

    private static Uri insertSettings(SQLiteDatabase db, ContentValues values, AccountFilter account) {
        requireKeys(values, SETTINGS_WRITE_COLUMNS);
        if (account == null) account = AccountFilter.fromValues(values);
        if (account == null) return null;
        account.applyTo(values);
        if (db.update("settings", values, account.selection(), account.withArgs(null)) == 0) {
            db.insertOrThrow("settings", null, values);
        }
        return account.settingsUri();
    }

    private static void ensureSettingsRow(SQLiteDatabase db, ContentValues source) {
        ContentValues identity = new ContentValues();
        AccountFilter account = AccountFilter.fromValues(source);
        if (account != null) {
            account.applyTo(identity);
        } else {
            identity.putNull("account_name");
            identity.putNull("account_type");
            identity.putNull("data_set");
        }
        db.insertWithOnConflict("settings", null, identity, SQLiteDatabase.CONFLICT_IGNORE);
    }

    private long insertDataLocked(SQLiteDatabase db, ContentValues copy, String forcedMime,
            AccountFilter account, boolean profile, long now, boolean syncAdapter) {
        String mimetype = forcedMime != null ? forcedMime : copy.getAsString("mimetype");
        copy.remove("mimetype");
        Long rawContactId = copy.getAsLong("raw_contact_id");
        copy.remove("raw_contact_id");
        requireKeys(copy, DATA_WRITE_COLUMNS);
        if (rawContactId == null || !hasRemainingRows(db, "raw_contacts",
                "_id = ? AND deleted = 0 AND raw_contact_is_user_profile = " + (profile ? 1 : 0),
                rawContactId)) {
            throw new IllegalArgumentException("Data insert requires an existing raw contact");
        }
        if (account != null && queryIds(db, "SELECT _id FROM raw_contacts WHERE "
                + account.selection() + " AND _id = ?",
                account.withArgs(new String[] {Long.toString(rawContactId)})).isEmpty()) {
            throw new IllegalArgumentException("Data raw contact belongs to another account");
        }
        copy.put("raw_contact_id", rawContactId);
        if (mimetype == null || mimetype.isEmpty()) {
            throw new IllegalArgumentException("Data insert requires mimetype");
        }
        if (MIME_PHONE.equals(mimetype)) {
            normalizePhoneValues(copy);
        }
        if (profile && MIME_PHOTO.equals(mimetype)) normalizeProfileThumbnail(copy);
        copy.put("mimetype_id", resolveOrCreateMimetypeId(db, mimetype));
        long id = insertContactRow(db, "data", profile, copy);
        refreshRawAndParentContacts(db, Collections.singleton(rawContactId), now, !syncAdapter);
        return id;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        int match = sUriMatcher.match(uri);
        boolean profileChanged = isProfileRequest(uri, match);
        int count;
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                count = updateLocked(db, match, uri, values, selection, selectionArgs);
                db.setTransactionSuccessful();
                if (count > 0) notifyNow = markMutation(uri, profileChanged);
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            notifyObservers(shouldSyncToNetwork(uri), profileChanged);
        }
        return count;
    }

    private int updateLocked(SQLiteDatabase db, int match, Uri uri, ContentValues values,
            String selection, String[] selectionArgs) {
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        long now = System.currentTimeMillis();
        boolean syncAdapter = isCallerSyncAdapter(uri);
        if (match == SETTINGS) return mutateSettings(db, uri, copy, selection, selectionArgs);
        if (match == PROFILE_SYNC_STATE || match == PROFILE_SYNC_STATE_ID) {
            return mutateProfileSyncState(db, match, uri, copy, selection, selectionArgs);
        }
        String rows = mutationRows(db, match, uri, selection, selectionArgs, syncAdapter);
        switch (match) {
            case CONTACTS:
            case CONTACTS_ID:
            case PROFILE: {
                requireKeys(copy, CONTACT_OPTIONS);
                if (copy.size() == 0) return 0;
                String rawRows = "contact_id IN (SELECT _id FROM contacts WHERE " + rows + ")";
                db.update("raw_contacts", copy, rawRows, null);
                if (!syncAdapter) {
                    bumpRawContactsVersionAndDirty(db,
                            queryIds(db, "SELECT _id FROM raw_contacts WHERE " + rawRows, null));
                }
                copy.put("contact_last_updated_timestamp", now);
                return db.update("contacts", copy, rows, null);
            }
            case RAW_CONTACTS:
            case RAW_CONTACTS_ID: {
                requireKeys(copy, RAW_WRITE_COLUMNS);
                if (copy.size() == 0) return 0;
                Set<Long> rawIds = queryIds(db, "SELECT _id FROM raw_contacts WHERE " + rows, null);
                int updated = db.update("raw_contacts", copy, rows, null);
                if (updated > 0) {
                    if (!syncAdapter && !copy.containsKey("dirty") && !copy.containsKey("version")) {
                        bumpRawContactsVersionAndDirty(db, rawIds);
                    }
                    if (copy.containsKey("deleted") && isProfileRequest(uri, match)) {
                        reconcileProfileContacts(db, now);
                    } else if (copy.containsKey("display_name") || copy.containsKey("deleted")) {
                        refreshRawAndParentContacts(db, rawIds, now, false);
                    }
                }
                return updated;
            }
            case DATA:
            case DATA_ID:
            case PHONES:
            case PHONES_ID:
            case EMAILS:
            case EMAILS_ID:
            case POSTALS:
            case POSTALS_ID:
            case RAW_CONTACTS_DATA: {
                requireKeys(copy, DATA_WRITE_COLUMNS);
                if (copy.size() == 0) return 0;
                Set<Long> rawIds = queryIds(db, "SELECT DISTINCT raw_contact_id FROM data"
                        + " WHERE " + rows, null);
                int updated = updateDataRows(db, copy, rows, isProfileRequest(uri, match));
                if (updated > 0) {
                    refreshRawAndParentContacts(db, rawIds, now, !syncAdapter);
                }
                return updated;
            }
            case GROUPS:
            case GROUPS_ID: {
                requireKeys(copy, GROUP_WRITE_COLUMNS);
                if (copy.size() == 0) return 0;
                if (!syncAdapter && !copy.containsKey("dirty")) {
                    copy.put("dirty", 1);
                }
                return db.update("groups", copy, rows, null);
            }
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private static void requireKeys(ContentValues values, Set<String> writable) {
        for (String key : values.keySet()) {
            if (!writable.contains(key)) throw new IllegalArgumentException("Invalid column " + key);
        }
    }

    private static int mutateProfileSyncState(SQLiteDatabase db, int match, Uri uri,
            ContentValues update, String selection, String[] selectionArgs) {
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables("profile_sync_state");
        qb.setProjectionMap(PROFILE_SYNC_COLUMNS);
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        String id = itemId(uri, match);
        if (id != null) {
            qb.appendWhereStandalone("_id = ?");
            selectionArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
        }
        if (update == null) return qb.delete(db, selection, selectionArgs);
        requireKeys(update, Set.of("data"));
        return update.size() == 0 ? 0 : qb.update(db, update, selection, selectionArgs);
    }

    private static int mutateSettings(SQLiteDatabase db, Uri uri, ContentValues update,
            String selection, String[] selectionArgs) {
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables("settings");
        qb.setProjectionMap(sSettingsProjectionMap);
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        AccountFilter account = AccountFilter.fromUri(uri);
        if (account != null) {
            qb.appendWhereStandalone(account.selection());
            selectionArgs = account.withArgs(selectionArgs);
        }
        if (update == null) {
            qb.appendWhereStandalone("NOT EXISTS (SELECT 1 FROM raw_contacts r WHERE "
                    + accountMatchesSettings("r") + ") AND NOT EXISTS (SELECT 1 FROM groups g WHERE "
                    + accountMatchesSettings("g") + ")");
            return qb.delete(db, selection, selectionArgs);
        }
        requireKeys(update, SETTINGS_UPDATE_COLUMNS);
        return update.size() == 0 ? 0 : qb.update(db, update, selection, selectionArgs);
    }

    // Resolve only public view columns with untrusted SQL. Subsequent writes use
    // only IDs read from that view and run in the same transaction.
    private static String mutationRows(SQLiteDatabase db, int match, Uri uri, String selection,
            String[] selectionArgs, boolean syncAdapter) {
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        String where = null;
        String id = itemId(uri, match);
        boolean item = false;
        switch (match) {
            case CONTACTS:
            case CONTACTS_ID:
            case PROFILE:
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                item = match == CONTACTS_ID;
                break;
            case RAW_CONTACTS:
            case RAW_CONTACTS_ID:
                qb.setTables("view_raw_contacts");
                qb.setProjectionMap(sRawContactsProjectionMap);
                if (!syncAdapter) where = "deleted = 0";
                item = match == RAW_CONTACTS_ID;
                break;
            case DATA:
            case DATA_ID:
            case PHONES:
            case PHONES_ID:
            case EMAILS:
            case EMAILS_ID:
            case POSTALS:
            case POSTALS_ID:
            case RAW_CONTACTS_DATA: {
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                if (!syncAdapter) where = "is_read_only = 0";
                if (match == RAW_CONTACTS_DATA) {
                    where = scopedSelection(where, "raw_contact_id = ?");
                    selectionArgs = ContactsProviderSupport.prependArgs(selectionArgs, id);
                }
                String mime = forcedMime(match);
                if (mime != null) {
                    where = scopedSelection(where, "mimetype = ?");
                    selectionArgs = ContactsProviderSupport.prependArgs(selectionArgs, mime);
                }
                item = match == DATA_ID || match == PHONES_ID || match == EMAILS_ID
                        || match == POSTALS_ID;
                break;
            }
            case GROUPS:
            case GROUPS_ID:
                qb.setTables("groups");
                qb.setProjectionMap(sGroupsProjectionMap);
                if (!syncAdapter) where = "deleted = 0 AND group_is_read_only = 0";
                item = match == GROUPS_ID;
                if (item) id = uri.getLastPathSegment();
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
        if (item) {
            where = scopedSelection(where, "_id = ?");
            selectionArgs = ContactsProviderSupport.prependArgs(
                    selectionArgs, id);
        }
        String scope = profileColumn(qb.getTables());
        if (scope != null) {
            qb.appendWhereStandalone(scope + (isProfileRequest(uri, match) ? " = 1" : " = 0"));
        }
        if (qb.getProjectionMap().containsKey("account_name")) {
            AccountFilter account = AccountFilter.fromUri(uri);
            if (account != null) {
                qb.appendWhereStandalone(account.selection());
                selectionArgs = account.withArgs(selectionArgs);
            }
        }
        if (where != null) qb.appendWhereStandalone(where);
        Set<Long> ids = new HashSet<>();
        try (Cursor c = qb.query(db, new String[] { "_id" }, selection, selectionArgs,
                null, null, null)) {
            while (c.moveToNext()) ids.add(c.getLong(0));
        }
        return idInClause("_id", ids);
    }

    private int updateDataRows(SQLiteDatabase db, ContentValues values, String rows, boolean profile) {
        int photosUpdated = 0;
        if (profile && values.containsKey("data15")) {
            String photoMime = "mimetype_id IN (SELECT _id FROM mimetypes WHERE mimetype = '"
                    + MIME_PHOTO + "')";
            String photoRows = scopedSelection(rows, photoMime);
            try (Cursor c = db.rawQuery("SELECT 1 FROM data WHERE " + photoRows + " LIMIT 1", null)) {
                if (c.moveToFirst()) {
                    ContentValues photoValues = new ContentValues(values);
                    normalizeProfileThumbnail(photoValues);
                    db.delete("profile_display_photos",
                            "data_id IN (SELECT _id FROM data WHERE " + photoRows + ")", null);
                    photosUpdated = db.update("data", photoValues, photoRows, null);
                    rows = scopedSelection(rows, "NOT (" + photoMime + ")");
                }
            }
        }
        int otherUpdated = values.containsKey("data1") || values.containsKey("data4")
                ? updateDataWithPhoneNormalization(db, values, rows)
                : db.update("data", values, rows, null);
        return photosUpdated + otherUpdated;
    }

    private static void normalizeProfileThumbnail(ContentValues values) {
        // Direct Data writes provide a thumbnail. A display-photo file exists only
        // after a successful stream write to the raw contact's display-photo URI.
        values.remove("data14");
        byte[] bytes = values.getAsByteArray("data15");
        if (bytes == null || bytes.length == 0) return;
        if (bytes.length > ProfilePhoto.MAX_INPUT_BYTES) {
            throw new IllegalArgumentException("Contacts Profile photo exceeds maximum input size");
        }
        try {
            values.put("data15", ProfilePhoto.decode(bytes).thumbnail());
        } catch (IOException failure) {
            throw new IllegalArgumentException("Invalid Contacts Profile photo", failure);
        }
    }

    private int updateDataWithPhoneNormalization(SQLiteDatabase db, ContentValues copy, String rows) {
        String sql = "SELECT _id, (SELECT mimetype FROM mimetypes WHERE mimetypes._id = data.mimetype_id) "
                + "FROM data WHERE " + rows;
        List<Long> phoneIds = new ArrayList<>();
        List<Long> otherIds = new ArrayList<>();
        try (Cursor c = db.rawQuery(sql, null)) {
            while (c.moveToNext()) {
                long id = c.getLong(0);
                if (MIME_PHONE.equals(c.getString(1))) phoneIds.add(id);
                else otherIds.add(id);
            }
        }
        int total = 0;
        if (!phoneIds.isEmpty()) {
            ContentValues phoneCopy = new ContentValues(copy);
            normalizePhoneValues(phoneCopy);
            if (phoneCopy.size() > 0) {
                total += db.update("data", phoneCopy, idInClause("_id", phoneIds), null);
            }
        }
        if (!otherIds.isEmpty()) {
            total += db.update("data", copy, idInClause("_id", otherIds), null);
        }
        return total;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        int match = sUriMatcher.match(uri);
        boolean profileChanged = isProfileRequest(uri, match);
        int count;
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                count = deleteLocked(db, match, uri, selection, selectionArgs);
                db.setTransactionSuccessful();
                if (count > 0) notifyNow = markMutation(uri, profileChanged);
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            notifyObservers(shouldSyncToNetwork(uri), profileChanged);
        }
        return count;
    }

    private int deleteLocked(SQLiteDatabase db, int match, Uri uri, String selection,
            String[] selectionArgs) {
        long now = System.currentTimeMillis();
        boolean syncAdapter = isCallerSyncAdapter(uri);
        if (match == SETTINGS) return mutateSettings(db, uri, null, selection, selectionArgs);
        if (match == PROFILE_SYNC_STATE || match == PROFILE_SYNC_STATE_ID) {
            return mutateProfileSyncState(db, match, uri, null, selection, selectionArgs);
        }
        if (match == PROFILE) {
            throw new UnsupportedOperationException("Delete the raw contacts that make up the profile");
        }
        String rows = mutationRows(db, match, uri, selection, selectionArgs, syncAdapter);
        switch (match) {
            case CONTACTS:
            case CONTACTS_ID: {
                Set<Long> contactIds = queryIds(db, "SELECT _id FROM contacts WHERE " + rows, null);
                deleteRawContacts(db, idInClause("contact_id", contactIds), syncAdapter, now);
                return contactIds.size();
            }
            case RAW_CONTACTS:
            case RAW_CONTACTS_ID:
                return deleteRawContacts(db, rows, syncAdapter, now);
            case DATA:
            case DATA_ID:
            case PHONES:
            case PHONES_ID:
            case EMAILS:
            case EMAILS_ID:
            case POSTALS:
            case POSTALS_ID:
            case RAW_CONTACTS_DATA:
            {
                Set<Long> rawIds = queryIds(db,
                        "SELECT DISTINCT raw_contact_id FROM data WHERE " + rows, null);
                int deleted = db.delete("data", rows, null);
                if (deleted > 0) {
                    refreshRawAndParentContacts(db, rawIds, now, !syncAdapter);
                }
                return deleted;
            }
            case GROUPS:
            case GROUPS_ID:
                return deleteGroups(db, rows, syncAdapter, now);
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private static int deleteRawContacts(SQLiteDatabase db, String rows, boolean syncAdapter,
            long now) {
        Set<Long> contactIds = queryIds(db,
                "SELECT DISTINCT contact_id FROM raw_contacts WHERE " + rows, null);
        int count = deleteLocalOrTombstone(db, "raw_contacts", rows, syncAdapter);
        for (long contactId : contactIds) {
            if (!hasRemainingRows(db, "raw_contacts", "contact_id = ? AND deleted = 0", contactId)) {
                db.delete("contacts", "_id = ?", new String[] { Long.toString(contactId) });
                if (!ContactsContract.isProfileId(contactId)) recordDeletedContact(db, contactId, now);
            } else {
                Set<Long> remaining = queryIds(db,
                        "SELECT _id FROM raw_contacts WHERE contact_id = ? AND deleted = 0",
                        new String[] { Long.toString(contactId) });
                refreshRawAndParentContacts(db, remaining, now, false);
            }
        }
        return count;
    }

    private static int deleteGroups(SQLiteDatabase db, String rows, boolean syncAdapter, long now) {
        String members = "mimetype_id IN (SELECT _id FROM mimetypes WHERE mimetype = ?)"
                + " AND data1 IN (SELECT CAST(_id AS TEXT) FROM groups WHERE " + rows + ")";
        String[] args = {ContactsContract.CommonDataKinds.GroupMembership.CONTENT_ITEM_TYPE};
        Set<Long> rawIds = queryIds(db,
                "SELECT DISTINCT raw_contact_id FROM data WHERE " + members, args);
        db.delete("data", members, args);
        int count = deleteLocalOrTombstone(db, "groups", rows, syncAdapter);
        refreshRawAndParentContacts(db, rawIds, now, !syncAdapter);
        return count;
    }

    private static int deleteLocalOrTombstone(SQLiteDatabase db, String table, String rows,
            boolean syncAdapter) {
        if (syncAdapter) return db.delete(table, rows, null);
        int hard = db.delete(table, rows + " AND (account_type IS NULL OR account_type = '')", null);
        String detach = "raw_contacts".equals(table) ? ", contact_id = NULL, aggregation_mode = "
                + ContactsContract.RawContacts.AGGREGATION_MODE_DISABLED : "";
        db.execSQL("UPDATE " + table + " SET deleted = 1, dirty = 1, version = version + 1" + detach
                + " WHERE (" + rows + ") AND deleted = 0");
        return hard + changesCount(db);
    }

    private static int changesCount(SQLiteDatabase db) {
        try (Cursor c = db.rawQuery("SELECT changes()", null)) {
            return c.moveToFirst() ? c.getInt(0) : 0;
        }
    }

    private boolean markMutation(Uri uri, boolean profileChanged) {
        if (mBatchDepth == 0) return true;
        mBatchDirty = true;
        mBatchSyncToNetwork |= shouldSyncToNetwork(uri);
        mBatchProfileDirty |= profileChanged;
        return false;
    }

    private static boolean shouldSyncToNetwork(Uri uri) {
        int match = sUriMatcher.match(uri);
        return match != PROFILE_SYNC_STATE && match != PROFILE_SYNC_STATE_ID
                && !isCallerSyncAdapter(uri);
    }

    private void notifyObservers(boolean syncToNetwork, boolean profileChanged) {
        if (getContext() == null) return;
        long identity = Binder.clearCallingIdentity();
        try {
            try {
                getContext().getContentResolver().notifyChange(NOTIFY_URI, null, syncToNetwork);
            } catch (RuntimeException failure) {
                SystemServer.log("contacts: observer notification failed: " + failure);
            }
            if (profileChanged) {
                try {
                    getContext().sendBroadcast(new Intent(ContactsContract.Intents.ACTION_PROFILE_CHANGED),
                            ContactsProviderSupport.READ_CONTACTS);
                } catch (RuntimeException failure) {
                    SystemServer.log("contacts: Profile change broadcast failed: " + failure);
                }
            }
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }

    private static boolean isCallerSyncAdapter(Uri uri) {
        return uri != null && "true".equals(
                uri.getQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER));
    }

    private static void bumpRawContactsVersionAndDirty(SQLiteDatabase db, Set<Long> rawIds) {
        if (rawIds == null || rawIds.isEmpty()) return;
        for (long rawId : rawIds) {
            db.execSQL("UPDATE raw_contacts SET dirty = 1, version = version + 1 WHERE _id = ?",
                    new Object[] { rawId });
        }
    }

    private static void normalizePhoneValues(ContentValues values) {
        if (!values.containsKey("data1")) {
            values.remove("data4");
            return;
        }
        String raw = values.getAsString("data1");
        String supplied = values.getAsString("data4");
        String e164 = null;
        if (raw != null && !raw.isEmpty()) {
            if (supplied != null && supplied.startsWith("+")
                    && PhoneNumberUtils.compareLoosely(raw, supplied)) {
                e164 = PhoneNumberUtils.formatNumberToE164(supplied, "ZZ");
            }
            if (e164 == null) {
                String region = SystemLocale.locales().get(0).getCountry();
                if (raw.startsWith("+") || !region.isEmpty()) {
                    e164 = PhoneNumberUtils.formatNumberToE164(raw,
                            region.isEmpty() ? "ZZ" : region);
                }
            }
        }
        values.put("data4", e164);
    }

    private static String forcedMime(int match) {
        switch (match) {
            case PHONES:
            case PHONES_ID:
            case PHONES_FILTER_EMPTY:
            case PHONES_FILTER:
                return MIME_PHONE;
            case EMAILS:
            case EMAILS_ID:
            case EMAILS_FILTER_EMPTY:
            case EMAILS_FILTER:
                return MIME_EMAIL;
            case POSTALS:
            case POSTALS_ID:
                return MIME_POSTAL;
            default:
                return null;
        }
    }

    private static long resolveOrCreateMimetypeId(SQLiteDatabase db, String mimetype) {
        try (Cursor c = db.rawQuery("SELECT _id FROM mimetypes WHERE mimetype = ?",
                new String[] { mimetype })) {
            if (c.moveToFirst()) return c.getLong(0);
        }
        ContentValues v = new ContentValues();
        v.put("mimetype", mimetype);
        return db.insertOrThrow("mimetypes", null, v);
    }

    private static void refreshRawAndParentContacts(SQLiteDatabase db, Set<Long> rawContactIds,
            long now, boolean markDirty) {
        if (rawContactIds == null || rawContactIds.isEmpty()) return;
        if (markDirty) {
            bumpRawContactsVersionAndDirty(db, rawContactIds);
        }
        Set<Long> contactIds = new HashSet<>();
        for (long rawId : rawContactIds) {
            NameResolution resolved = resolveDisplayNameForRawContact(db, rawId);
            ContentValues rawUpdate = new ContentValues();
            applyNameDerivedFields(rawUpdate, resolved.displayName, resolved.displayNameAlt,
                    resolved.source);
            db.update("raw_contacts", rawUpdate, "_id = ?", new String[] { Long.toString(rawId) });

            try (Cursor c = db.rawQuery("SELECT contact_id FROM raw_contacts WHERE _id = ?",
                    new String[] { Long.toString(rawId) })) {
                if (c.moveToFirst() && !c.isNull(0)) contactIds.add(c.getLong(0));
            }
        }
        for (long contactId : contactIds) {
            boolean hasPhone = false;
            try (Cursor c = db.rawQuery(
                    "SELECT 1 FROM data JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                            + "JOIN raw_contacts ON data.raw_contact_id = raw_contacts._id "
                            + "WHERE raw_contacts.contact_id = ? AND raw_contacts.deleted = 0 "
                            + "AND mimetypes.mimetype = ? LIMIT 1",
                    new String[] { Long.toString(contactId), MIME_PHONE })) {
                hasPhone = c.moveToFirst();
            }
            NameResolution best = NameResolution.EMPTY;
            long bestRawId = 0;
            try (Cursor c = db.rawQuery(
                    "SELECT _id FROM raw_contacts WHERE contact_id = ? AND deleted = 0 ORDER BY _id ASC",
                    new String[] { Long.toString(contactId) })) {
                while (c.moveToNext()) {
                    long rawId = c.getLong(0);
                    NameResolution candidate = resolveDisplayNameForRawContact(db, rawId);
                    if (bestRawId == 0 || candidate.source > best.source
                            || (candidate.source == best.source && best.displayName == null
                                    && candidate.displayName != null)) {
                        best = candidate;
                        bestRawId = rawId;
                    }
                }
            }
            ContentValues update = new ContentValues();
            update.put("has_phone_number", hasPhone ? 1 : 0);
            update.put("contact_last_updated_timestamp", now);
            if (bestRawId != 0) update.put("name_raw_contact_id", bestRawId);
            if (ContactsContract.isProfileId(contactId)) {
                updateProfilePhotoMetadata(db, contactId, update);
            }
            applyNameDerivedFields(update, best.displayName, best.displayNameAlt, best.source);
            db.update("contacts", update, "_id = ?", new String[] { Long.toString(contactId) });
        }
    }

    private static void updateProfilePhotoMetadata(SQLiteDatabase db, long contactId, ContentValues values) {
        try (Cursor c = db.rawQuery("SELECT _id FROM view_data WHERE contact_id = ? AND mimetype = ?"
                + " AND data15 IS NOT NULL AND length(data15) > 0"
                + " ORDER BY is_super_primary DESC, is_primary DESC, _id LIMIT 1",
                new String[] {Long.toString(contactId), MIME_PHOTO})) {
            if (c.moveToFirst()) {
                values.put("photo_id", c.getLong(0));
                Uri contact = ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, contactId);
                String thumbnail = Uri.withAppendedPath(contact, "photo").toString();
                values.put("photo_thumb_uri", thumbnail);
                long photoId = c.getLong(0);
                if (hasRemainingRows(db, "profile_display_photos", "data_id = ?", photoId)) {
                    values.put("photo_file_id", photoId);
                    values.put("photo_uri",
                            ContentUris.withAppendedId(ContactsContract.DisplayPhoto.CONTENT_URI, photoId).toString());
                } else {
                    values.putNull("photo_file_id");
                    values.put("photo_uri", thumbnail);
                }
            } else {
                values.putNull("photo_id");
                values.putNull("photo_file_id");
                values.putNull("photo_uri");
                values.putNull("photo_thumb_uri");
            }
        }
    }

    @Override
    public AssetFileDescriptor openAssetFile(Uri uri, String mode) throws FileNotFoundException {
        int match = sUriMatcher.match(uri);
        boolean isAsset = match == PROFILE_VCARD || match == CONTACT_THUMBNAIL
                || match == CONTACT_DISPLAY_PHOTO || match == RAW_DISPLAY_PHOTO || match == DISPLAY_PHOTO_FILE;
        if (!isAsset || !isProfileRequest(uri, match)) return super.openAssetFile(uri, mode);
        String id = itemId(uri, match);
        if (match == RAW_DISPLAY_PHOTO && mode.contains("w")) {
            return openProfilePhotoWriter(uri, Long.parseLong(id));
        }
        if (!"r".equals(mode)) throw new FileNotFoundException("Profile asset is read-only: " + uri);
        byte[] contents;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            contents = switch (match) {
                case PROFILE_VCARD -> profileVCard(db, uri);
                case CONTACT_THUMBNAIL -> profileThumbnail(db, id);
                default -> profileDisplayPhoto(db, match, id);
            };
        }
        if (contents == null || contents.length == 0) throw new FileNotFoundException(uri.toString());
        return snapshotAsset(contents);
    }

    private static byte[] profileDisplayPhoto(SQLiteDatabase db, int match, String id) {
        String where = switch (match) {
            case RAW_DISPLAY_PHOTO -> " d.raw_contact_id = ?";
            case DISPLAY_PHOTO_FILE -> " p.data_id = ?";
            default -> " d._id = d.photo_id" + (id == null ? "" : " AND d.contact_id = ?");
        };
        try (Cursor c = db.rawQuery("SELECT p.contents FROM profile_display_photos p"
                + " JOIN view_data d ON p.data_id = d._id WHERE d.is_user_profile = 1 AND"
                + where + " ORDER BY d.is_super_primary DESC, d.is_primary DESC, d._id LIMIT 1",
                id == null ? null : new String[] {id})) {
            return c.moveToFirst() ? c.getBlob(0) : null;
        }
    }

    private AssetFileDescriptor openProfilePhotoWriter(Uri uri, long rawId) throws FileNotFoundException {
        synchronized (mDbLock) {
            if (!hasRemainingRows(openDatabase(), "raw_contacts",
                    "_id = ? AND deleted = 0 AND raw_contact_is_user_profile = 1", rawId)) {
                throw new FileNotFoundException(uri.toString());
            }
        }
        ParcelFileDescriptor[] pipe;
        try {
            pipe = ParcelFileDescriptor.createPipe();
        } catch (IOException failure) {
            throw assetFailure("Could not open Contacts Profile photo stream", failure);
        }
        try {
            sProfilePhotoWriters.execute(() -> receiveProfilePhoto(uri, rawId, pipe[0]));
        } catch (RejectedExecutionException busy) {
            for (ParcelFileDescriptor end : pipe) {
                try {
                    end.close();
                } catch (IOException failure) {
                    busy.addSuppressed(failure);
                }
            }
            throw assetFailure("Contacts Profile photo writers are busy", busy);
        }
        return new AssetFileDescriptor(pipe[1], 0, AssetFileDescriptor.UNKNOWN_LENGTH);
    }

    private void receiveProfilePhoto(Uri uri, long rawId, ParcelFileDescriptor source) {
        try (InputStream stream = new ParcelFileDescriptor.AutoCloseInputStream(source)) {
            byte[] encoded = stream.readNBytes(ProfilePhoto.MAX_INPUT_BYTES + 1);
            if (encoded.length > ProfilePhoto.MAX_INPUT_BYTES) {
                throw new IOException("Contacts Profile photo exceeds maximum input size");
            }
            ProfilePhoto photo = ProfilePhoto.decode(encoded);
            boolean syncAdapter = isCallerSyncAdapter(uri);
            synchronized (mDbLock) {
                SQLiteDatabase db = openDatabase();
                if (!hasRemainingRows(db, "raw_contacts",
                        "_id = ? AND deleted = 0 AND raw_contact_is_user_profile = 1", rawId)) return;
                db.beginTransaction();
                try {
                    storeProfilePhoto(db, rawId, photo, !syncAdapter);
                    db.setTransactionSuccessful();
                } finally {
                    db.endTransaction();
                }
            }
            notifyObservers(!syncAdapter, true);
        } catch (IOException | RuntimeException failure) {
            SystemServer.log("contacts: Profile photo write failed: " + failure);
        }
    }

    private static void storeProfilePhoto(SQLiteDatabase db, long rawId, ProfilePhoto photo,
            boolean markDirty) {
        long mimeId = resolveOrCreateMimetypeId(db, MIME_PHOTO);
        long dataId = 0;
        try (Cursor c = db.rawQuery("SELECT _id FROM data WHERE raw_contact_id = ? AND mimetype_id = ?"
                + " ORDER BY is_super_primary DESC, is_primary DESC, _id LIMIT 1",
                new String[] {Long.toString(rawId), Long.toString(mimeId)})) {
            if (c.moveToFirst()) dataId = c.getLong(0);
        }
        ContentValues values = new ContentValues();
        values.put("data15", photo.thumbnail());
        if (dataId == 0) {
            values.put("mimetype_id", mimeId);
            values.put("raw_contact_id", rawId);
            values.put("is_primary", 1);
            dataId = insertContactRow(db, "data", true, values);
        } else {
            db.update("data", values, "_id = ?", new String[] {Long.toString(dataId)});
        }
        if (photo.display() == null) {
            db.delete("profile_display_photos", "data_id = ?", new String[] {Long.toString(dataId)});
        } else {
            ContentValues display = new ContentValues();
            display.put("data_id", dataId);
            display.put("contents", photo.display());
            db.replaceOrThrow("profile_display_photos", null, display);
        }
        refreshRawAndParentContacts(db, Collections.singleton(rawId), System.currentTimeMillis(), markDirty);
    }

    private static byte[] profileThumbnail(SQLiteDatabase db, String contactId) {
        String scope = contactId == null ? "is_user_profile = 1" : "is_user_profile = 1 AND contact_id = ?";
        String[] args = contactId == null ? null : new String[] {contactId};
        try (Cursor c = db.rawQuery("SELECT data15 FROM view_data WHERE _id = photo_id AND " + scope, args)) {
            return c.moveToFirst() ? c.getBlob(0) : null;
        }
    }

    private static byte[] profileVCard(SQLiteDatabase db, Uri uri) {
        String displayName;
        long contactId;
        long nameRawId;
        try (Cursor c = db.rawQuery("SELECT _id, display_name, name_raw_contact_id"
                + " FROM contacts WHERE is_user_profile = 1", null)) {
            if (!c.moveToFirst()) return null;
            contactId = c.getLong(0);
            displayName = c.getString(1);
            nameRawId = c.getLong(2);
        }
        try (Cursor c = db.rawQuery("SELECT mimetype, data1, data2, data3, data4, data5,"
                + " data6, data7, data8, data9, data10, data15 FROM view_data WHERE contact_id = ?"
                + " ORDER BY (raw_contact_id = ?) DESC, is_super_primary DESC, is_primary DESC, _id",
                new String[] {Long.toString(contactId), Long.toString(nameRawId)})) {
            boolean exportPhoto = !uri.getBooleanQueryParameter(
                    ContactsContract.Contacts.QUERY_PARAMETER_VCARD_NO_PHOTO, false);
            return ProfileVCard.encode(displayName, c, exportPhoto);
        }
    }

    private static AssetFileDescriptor snapshotAsset(byte[] bytes) throws FileNotFoundException {
        try (SharedMemory memory = SharedMemory.create("contacts-profile", bytes.length)) {
            ByteBuffer mapping = memory.mapReadWrite();
            try {
                mapping.put(bytes);
            } finally {
                SharedMemory.unmap(mapping);
            }
            if (!memory.setProtect(OsConstants.PROT_READ)) {
                throw new IOException("Could not protect Contacts Profile snapshot");
            }
            return new AssetFileDescriptor(memory.getFdDup(), 0, bytes.length);
        } catch (IOException | ErrnoException failure) {
            throw assetFailure("Could not export Contacts Profile", failure);
        }
    }

    private static FileNotFoundException assetFailure(String message, Exception failure) {
        FileNotFoundException error = new FileNotFoundException(message);
        error.initCause(failure);
        return error;
    }

    private static final class NameResolution {
        static final NameResolution EMPTY = new NameResolution(
                null, null, ContactsContract.DisplayNameSources.UNDEFINED);
        final String displayName;
        final String displayNameAlt;
        final int source;

        NameResolution(String displayName, String displayNameAlt, int source) {
            this.displayName = displayName;
            this.displayNameAlt = displayNameAlt;
            this.source = source;
        }
    }

    /**
     * Resolves display name following AOSP precedence:
     * {@code StructuredName} -> {@code Nickname} -> {@code Organization} ->
     * {@code Phone} -> {@code Email}.
     */
    private static NameResolution resolveDisplayNameForRawContact(SQLiteDatabase db, long rawContactId) {
        String[] order = new String[] {
                MIME_STRUCTURED_NAME, MIME_NICKNAME, MIME_ORGANIZATION, MIME_PHONE, MIME_EMAIL
        };
        int[] sources = new int[] {
                ContactsContract.DisplayNameSources.STRUCTURED_NAME,
                ContactsContract.DisplayNameSources.NICKNAME,
                ContactsContract.DisplayNameSources.ORGANIZATION,
                ContactsContract.DisplayNameSources.PHONE,
                ContactsContract.DisplayNameSources.EMAIL
        };
        for (int i = 0; i < order.length; i++) {
            try (Cursor c = db.rawQuery(
                    "SELECT data.data1, data.data2, data.data3, data.data4, data.data5, data.data6 "
                            + "FROM data JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                            + "WHERE data.raw_contact_id = ? AND mimetypes.mimetype = ? "
                            + "ORDER BY data.is_super_primary DESC, data.is_primary DESC, data._id ASC LIMIT 1",
                    new String[] { Long.toString(rawContactId), order[i] })) {
                if (!c.moveToFirst()) continue;
                String d1 = trimOrNull(c.getString(0));
                String d2 = trimOrNull(c.getString(1));
                String d3 = trimOrNull(c.getString(2));
                String d4 = trimOrNull(c.getString(3));
                String d5 = trimOrNull(c.getString(4));
                String d6 = trimOrNull(c.getString(5));
                if (MIME_STRUCTURED_NAME.equals(order[i])) {
                    String display = d1 != null ? d1 : joinNonEmpty(" ", d4, d2, d5, d3, d6);
                    if (display == null) continue;
                    String alt = (d3 != null && d2 != null) ? d3 + ", " + d2 : display;
                    return new NameResolution(display, alt, sources[i]);
                } else if (MIME_ORGANIZATION.equals(order[i]) || MIME_EMAIL.equals(order[i])) {
                    String display = d1 != null ? d1 : d4;
                    if (display != null) return new NameResolution(display, display, sources[i]);
                } else if (d1 != null) {
                    return new NameResolution(d1, d1, sources[i]);
                }
            }
        }
        return NameResolution.EMPTY;
    }

    private static void applyNameDerivedFields(ContentValues target, String displayName,
            String displayNameAlt, int source) {
        target.put("display_name", displayName);
        target.put("display_name_alt", displayNameAlt);
        target.put("display_name_source", source);
        target.put("sort_key", displayName);
        target.put("sort_key_alt", displayNameAlt);
        AlphabeticIndex.ImmutableIndex<Void> index = phonebookIndex(SystemLocale.locales().get(0));
        putPhonebookEntry(target, index, displayName, "");
        putPhonebookEntry(target, index, displayNameAlt, "_alt");
    }

    private static synchronized AlphabeticIndex.ImmutableIndex<Void> phonebookIndex(Locale locale) {
        if (!locale.equals(sPhonebookLocale)) {
            sPhonebookIndex = new AlphabeticIndex<Void>(locale).addLabels(Locale.ENGLISH)
                    .buildImmutableIndex();
            sPhonebookLocale = locale;
        }
        return sPhonebookIndex;
    }

    private static void putPhonebookEntry(ContentValues values,
            AlphabeticIndex.ImmutableIndex<Void> index, String name, String suffix) {
        int bucket = name == null || name.isEmpty() ? 0 : index.getBucketIndex(name);
        String label = name == null || name.isEmpty() ? "" : index.getBucket(bucket).getLabel();
        values.put("phonebook_bucket" + suffix, bucket);
        values.put("phonebook_label" + suffix, label);
    }

    private static String trimOrNull(String value) {
        if (value == null) return null;
        String trimmed = value.trim();
        return trimmed.isEmpty() ? null : trimmed;
    }

    private static String joinNonEmpty(String delimiter, String... parts) {
        StringBuilder sb = new StringBuilder();
        for (String p : parts) {
            if (p == null || p.isEmpty()) continue;
            if (sb.length() > 0) sb.append(delimiter);
            sb.append(p);
        }
        return sb.length() == 0 ? null : sb.toString();
    }

    private static Set<Long> queryIds(SQLiteDatabase db, String sql, String[] args) {
        Set<Long> ids = new HashSet<>();
        try (Cursor c = db.rawQuery(sql, args)) {
            while (c.moveToNext()) {
                if (!c.isNull(0)) ids.add(c.getLong(0));
            }
        }
        return ids;
    }

    private static boolean hasRemainingRows(SQLiteDatabase db, String table, String where, long id) {
        try (Cursor c = db.rawQuery("SELECT 1 FROM " + table + " WHERE " + where + " LIMIT 1",
                new String[] { Long.toString(id) })) {
            return c.moveToFirst();
        }
    }

    private static void recordDeletedContact(SQLiteDatabase db, long contactId, long timestamp) {
        ContentValues v = new ContentValues();
        v.put("contact_id", contactId);
        v.put("contact_deleted_timestamp", timestamp);
        db.insertWithOnConflict("deleted_contacts", null, v, SQLiteDatabase.CONFLICT_REPLACE);
    }
}
