/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.AttributionSource;
import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Intent;
import android.content.OperationApplicationException;
import android.content.UriMatcher;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.net.Uri;
import android.provider.ContactsContract;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Built-in SQLite-backed {@code content://com.android.contacts} provider for
 * {@link ContactsContract}. Starts with an empty address book and a local default
 * directory entry, enforces {@code READ_CONTACTS} / {@code WRITE_CONTACTS}, AppOps,
 * and URI grants, and implements transactional {@link #applyBatch}, sync-adapter
 * soft/hard deletes, loose {@code phone_lookup} matching, and display-name fallback.
 *
 * <p><b>Limitation (1:1 contacts without multi-raw-contact aggregation):</b>
 * Inserting a {@code raw_contact} without an explicit {@code contact_id} creates a
 * 1:1 {@code contacts} row; automatic cross-account raw-contact aggregation and
 * split/join heuristics are not implemented.
 */
final class ContactsProvider extends ContentProvider {
    private static final int CONTACTS = 1;
    private static final int CONTACTS_ID = 2;
    private static final int CONTACTS_FILTER_EMPTY = 3;
    private static final int CONTACTS_FILTER = 4;
    private static final int CONTACTS_LOOKUP = 5;
    private static final int CONTACTS_LOOKUP_ID = 6;
    private static final int CONTACTS_DATA = 7;

    private static final int RAW_CONTACTS = 10;
    private static final int RAW_CONTACTS_ID = 11;
    private static final int RAW_CONTACTS_DATA = 12;

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
    private static final int PROFILE_RAW_CONTACTS = 91;
    private static final int PROFILE_DATA = 92;

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

    private static final int DB_VERSION = 2;
    private static final Uri NOTIFY_URI = ContactsContract.AUTHORITY_URI;

    private static final UriMatcher sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);
    private static final Map<String, String> sContactsProjectionMap;
    private static final Map<String, String> sRawContactsProjectionMap;
    private static final Map<String, String> sDataProjectionMap;
    private static final Map<String, String> sPhoneLookupProjectionMap;
    private static final Map<String, String> sGroupsProjectionMap;
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
        sUriMatcher.addURI(auth, "contacts/#/entities", CONTACTS_DATA);

        sUriMatcher.addURI(auth, "raw_contacts", RAW_CONTACTS);
        sUriMatcher.addURI(auth, "raw_contacts/#", RAW_CONTACTS_ID);
        sUriMatcher.addURI(auth, "raw_contacts/#/data", RAW_CONTACTS_DATA);
        sUriMatcher.addURI(auth, "raw_contacts/#/entity", RAW_CONTACTS_DATA);

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
        sUriMatcher.addURI(auth, "groups", GROUPS);
        sUriMatcher.addURI(auth, "groups/#", GROUPS_ID);
        sUriMatcher.addURI(auth, "groups_summary", GROUPS_SUMMARY);
        sUriMatcher.addURI(auth, "deleted_contacts", DELETED_CONTACTS);
        sUriMatcher.addURI(auth, "directories", DIRECTORIES);
        sUriMatcher.addURI(auth, "directories/#", DIRECTORIES_ID);
        sUriMatcher.addURI(auth, "settings", SETTINGS);
        sUriMatcher.addURI(auth, "profile", PROFILE);
        sUriMatcher.addURI(auth, "profile/raw_contacts", PROFILE_RAW_CONTACTS);
        sUriMatcher.addURI(auth, "profile/data", PROFILE_DATA);

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

        sDeletedContactsProjectionMap = identityMap(
                "contact_id", "contact_deleted_timestamp");

        sDirectoriesProjectionMap = identityMap(
                "_id", "packageName", "typeResourceId", "displayName",
                "accountType", "accountName", "exportSupport", "shortcutSupport",
                "photoSupport");

        sSettingsProjectionMap = identityMap(
                "account_name", "account_type", "data_set", "ungrouped_visible",
                "should_sync", "any_synced", "ungrouped_count",
                "ungrouped_with_phones");
    }

    private final PackageManagerService mPm;
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
        mPm = pm;
        mCallers = pm == null ? null : new CallerIdentity(pm, atm);
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
                attachInfo(ContactsProviderSupport.contextForProvider(mGrants, mRegistration),
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
        return ContactsProviderSupport.enforcePermissionInner(
                mPm, mCallers, mGrants, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION,
                ContactsProviderSupport.READ_CONTACTS, source);
    }

    @Override
    protected int enforceWritePermissionInner(Uri uri, AttributionSource source) {
        return ContactsProviderSupport.enforcePermissionInner(
                mPm, mCallers, mGrants, uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
                ContactsProviderSupport.WRITE_CONTACTS, source);
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
                + "sort_key TEXT,"
                + "sort_key_alt TEXT,"
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
                + "sort_key TEXT,"
                + "sort_key_alt TEXT,"
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
                + "should_sync INTEGER NOT NULL DEFAULT 1,"
                + "any_synced INTEGER NOT NULL DEFAULT 0,"
                + "ungrouped_count INTEGER NOT NULL DEFAULT 0,"
                + "ungrouped_with_phones INTEGER NOT NULL DEFAULT 0"
                + ");");
        createIndexesAndViews(db);
    }

    private static void onUpgradeDatabase(SQLiteDatabase db, int oldVersion, int newVersion) {
        if (oldVersion < 2) {
            ensureColumns(db, "contacts", new String[][] {
                    { "photo_file_id", "INTEGER" },
                    { "sort_key", "TEXT" },
                    { "sort_key_alt", "TEXT" },
                    { "phonebook_label", "TEXT" },
                    { "phonebook_label_alt", "TEXT" },
                    { "phonebook_bucket", "INTEGER DEFAULT 0" },
                    { "phonebook_bucket_alt", "INTEGER DEFAULT 0" },
                    { "in_default_directory", "INTEGER NOT NULL DEFAULT 1" },
                    { "times_used", "INTEGER NOT NULL DEFAULT 0" },
                    { "last_time_used", "INTEGER" },
                    { "contact_presence", "INTEGER" },
                    { "contact_status", "TEXT" },
                    { "contact_status_ts", "INTEGER" },
                    { "contact_status_res_package", "TEXT" },
                    { "contact_status_label", "INTEGER" },
                    { "contact_status_icon", "INTEGER" }
            });
            ensureColumns(db, "raw_contacts", new String[][] {
                    { "phonetic_name", "TEXT" },
                    { "sort_key", "TEXT" },
                    { "sort_key_alt", "TEXT" },
                    { "phonebook_label", "TEXT" },
                    { "phonebook_label_alt", "TEXT" },
                    { "phonebook_bucket", "INTEGER DEFAULT 0" },
                    { "phonebook_bucket_alt", "INTEGER DEFAULT 0" },
                    { "times_used", "INTEGER NOT NULL DEFAULT 0" },
                    { "last_time_used", "INTEGER" },
                    { "raw_contact_is_user_profile", "INTEGER NOT NULL DEFAULT 0" }
            });
            ensureColumns(db, "data", new String[][] {
                    { "is_read_only", "INTEGER NOT NULL DEFAULT 0" }
            });
            ensureColumns(db, "groups", new String[][] {
                    { "sourceid", "TEXT" },
                    { "version", "INTEGER NOT NULL DEFAULT 1" },
                    { "dirty", "INTEGER NOT NULL DEFAULT 0" },
                    { "title_res", "INTEGER" },
                    { "res_package", "TEXT" }
            });
            db.execSQL("DROP VIEW IF EXISTS view_contacts;");
            db.execSQL("DROP VIEW IF EXISTS view_raw_contacts;");
            db.execSQL("DROP VIEW IF EXISTS view_data;");
            db.execSQL("DROP VIEW IF EXISTS view_phone_lookup;");
            createIndexesAndViews(db);
            backfillV2Metadata(db);
        }
    }

    private static void createIndexesAndViews(SQLiteDatabase db) {
        db.execSQL("CREATE INDEX IF NOT EXISTS contacts_lookup_idx ON contacts(lookup);");
        db.execSQL("CREATE INDEX IF NOT EXISTS contacts_sort_key_idx ON contacts(sort_key);");
        db.execSQL("CREATE INDEX IF NOT EXISTS raw_contacts_contact_idx ON raw_contacts(contact_id);");
        db.execSQL("CREATE INDEX IF NOT EXISTS data_raw_contact_idx ON data(raw_contact_id);");
        db.execSQL("CREATE INDEX IF NOT EXISTS data_mimetype_idx ON data(mimetype_id);");

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
                + "data.data13 AS data13, data.data14 AS data14, data.data15 AS data15,"
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
                + "COALESCE(data.data4, data.data1) AS normalized_number,"
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
                + "WHERE mimetypes.mimetype = '" + MIME_PHONE + "' AND raw_contacts.deleted = 0;");
    }

    private static void ensureColumns(SQLiteDatabase db, String table, String[][] additions) {
        Set<String> cols = existingColumns(db, table);
        for (String[] col : additions) {
            if (!cols.contains(col[0])) {
                db.execSQL("ALTER TABLE " + table + " ADD COLUMN " + col[0] + " " + col[1] + ";");
            }
        }
    }

    private static Set<String> existingColumns(SQLiteDatabase db, String table) {
        Set<String> cols = new HashSet<>();
        try (Cursor c = db.rawQuery("PRAGMA table_info(" + table + ")", null)) {
            int nameIdx = c.getColumnIndex("name");
            while (c.moveToNext()) {
                if (nameIdx >= 0) cols.add(c.getString(nameIdx));
            }
        }
        return cols;
    }

    private static void backfillV2Metadata(SQLiteDatabase db) {
        long now = System.currentTimeMillis();
        Set<Long> rawIds = queryIds(db, "SELECT _id FROM raw_contacts", null);
        // Backfill data4 normalized phone numbers first.
        try (Cursor c = db.rawQuery(
                "SELECT data._id, data.data1 FROM data JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                        + "WHERE mimetypes.mimetype = ? AND data.data1 IS NOT NULL AND data.data4 IS NULL",
                new String[] { MIME_PHONE })) {
            while (c.moveToNext()) {
                String normalized = ContactsProviderSupport.normalizePhoneNumber(c.getString(1));
                if (normalized != null) {
                    ContentValues cv = new ContentValues();
                    cv.put("data4", normalized);
                    db.update("data", cv, "_id = ?", new String[] { Long.toString(c.getLong(0)) });
                }
            }
        }
        refreshRawAndParentContacts(db, rawIds, now, false);
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
            case PROFILE_DATA:
                return ContactsContract.Data.CONTENT_TYPE;
            case DATA_ID:
                return "vnd.android.cursor.item/data";
            case RAW_CONTACTS:
            case PROFILE_RAW_CONTACTS:
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
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION, ContactsProviderSupport.READ_CONTACTS);
        int match = sUriMatcher.match(uri);
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
                effSelection = ContactsProviderSupport.combineSelection(selection, "is_user_profile = 0");
                break;
            case CONTACTS_ID:
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case CONTACTS_FILTER: {
                String pattern = "%" + uri.getLastPathSegment() + "%";
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        "is_user_profile = 0 AND display_name LIKE ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, pattern);
                break;
            }
            case CONTACTS_LOOKUP: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "lookup = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, segments.get(2));
                break;
            }
            case CONTACTS_LOOKUP_ID: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "(_id = ? OR lookup = ?)");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, segments.get(3), segments.get(2));
                break;
            }
            case CONTACTS_DATA: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "contact_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, segments.get(1));
                break;
            }
            case RAW_CONTACTS:
                qb.setTables("view_raw_contacts");
                qb.setProjectionMap(sRawContactsProjectionMap);
                if (!syncAdapter) {
                    effSelection = ContactsProviderSupport.combineSelection(selection, "deleted = 0");
                }
                break;
            case RAW_CONTACTS_ID:
                qb.setTables("view_raw_contacts");
                qb.setProjectionMap(sRawContactsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        syncAdapter ? "_id = ?" : "_id = ? AND deleted = 0");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case RAW_CONTACTS_DATA: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "raw_contact_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, segments.get(1));
                break;
            }
            case DATA:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                break;
            case DATA_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case PHONES:
            case PHONES_FILTER_EMPTY:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_PHONE);
                break;
            case PHONES_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ? AND _id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_PHONE, Long.toString(ContentUris.parseId(uri)));
                break;
            case PHONES_FILTER: {
                String pattern = "%" + uri.getLastPathSegment() + "%";
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        "mimetype = ? AND (display_name LIKE ? OR data1 LIKE ? OR data4 LIKE ?)");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_PHONE, pattern, pattern, pattern);
                break;
            }
            case EMAILS:
            case EMAILS_FILTER_EMPTY:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_EMAIL);
                break;
            case EMAILS_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ? AND _id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_EMAIL, Long.toString(ContentUris.parseId(uri)));
                break;
            case EMAILS_FILTER: {
                String pattern = "%" + uri.getLastPathSegment() + "%";
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        "mimetype = ? AND (display_name LIKE ? OR data1 LIKE ?)");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_EMAIL, pattern, pattern);
                break;
            }
            case POSTALS:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_POSTAL);
                break;
            case POSTALS_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ? AND _id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_POSTAL, Long.toString(ContentUris.parseId(uri)));
                break;
            case PHONE_LOOKUP:
                qb.setTables("view_phone_lookup");
                qb.setProjectionMap(sPhoneLookupProjectionMap);
                phoneLookupQuery = uri.getLastPathSegment();
                break;
            case GROUPS:
            case GROUPS_SUMMARY:
                qb.setTables("groups");
                qb.setProjectionMap(sGroupsProjectionMap);
                if (!syncAdapter) {
                    effSelection = ContactsProviderSupport.combineSelection(selection, "deleted = 0");
                }
                break;
            case GROUPS_ID:
                qb.setTables("groups");
                qb.setProjectionMap(sGroupsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection,
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
                effSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case SETTINGS:
                qb.setTables("settings");
                qb.setProjectionMap(sSettingsProjectionMap);
                break;
            case PROFILE:
                qb.setTables("view_contacts");
                qb.setProjectionMap(sContactsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "is_user_profile = 1");
                break;
            case PROFILE_RAW_CONTACTS:
                qb.setTables("view_raw_contacts");
                qb.setProjectionMap(sRawContactsProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        (syncAdapter ? "" : "deleted = 0 AND ")
                                + "contact_id IN (SELECT _id FROM contacts WHERE is_user_profile = 1)");
                break;
            case PROFILE_DATA:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        "contact_id IN (SELECT _id FROM contacts WHERE is_user_profile = 1)");
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }

        Cursor cursor;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            if (phoneLookupQuery != null) {
                List<Long> matchedDataIds = findMatchingPhoneDataIds(db, phoneLookupQuery);
                String idClause = idInClause("data_id", matchedDataIds);
                effSelection = ContactsProviderSupport.combineSelection(effSelection, idClause);
            }
            cursor = qb.query(db, projection, effSelection, effArgs,
                    null, null, sortOrder, limit);
        }
        ContactsProviderSupport.setNotificationUri(cursor, getContext(), uri);
        return cursor;
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
                if (ContactsProviderSupport.phoneNumbersEqualLoose(number, queryNumber)
                        || ContactsProviderSupport.phoneNumbersEqualLoose(normalized, queryNumber)) {
                    dataIds.add(dataId);
                }
            }
        }
        return dataIds;
    }

    private static String idInClause(String column, List<Long> ids) {
        if (ids.isEmpty()) return "1 = 0";
        StringBuilder sb = new StringBuilder(column).append(" IN (");
        for (int i = 0; i < ids.size(); i++) {
            if (i > 0) sb.append(',');
            sb.append(ids.get(i));
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
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, NOTIFY_URI,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CONTACTS);
        if (operations == null || operations.isEmpty()) {
            return new ContentProviderResult[0];
        }
        ContentProviderResult[] results;
        boolean shouldNotify = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            boolean outermost = (mBatchDepth == 0);
            if (outermost) mBatchDirty = false;
            mBatchDepth++;
            db.beginTransaction();
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
                        mBatchDirty = false;
                    }
                }
            }
        }
        if (shouldNotify) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return results;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CONTACTS);
        long id;
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                id = insertLocked(db, uri, values);
                db.setTransactionSuccessful();
                if (mBatchDepth > 0) {
                    mBatchDirty = true;
                } else {
                    notifyNow = true;
                }
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return ContentUris.withAppendedId(uri, id);
    }

    @Override
    public int bulkInsert(Uri uri, ContentValues[] values) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CONTACTS);
        if (values == null || values.length == 0) return 0;
        int inserted = 0;
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                for (ContentValues item : values) {
                    if (insertLocked(db, uri, item) > 0) inserted++;
                }
                db.setTransactionSuccessful();
                if (inserted > 0) {
                    if (mBatchDepth > 0) {
                        mBatchDirty = true;
                    } else {
                        notifyNow = true;
                    }
                }
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return inserted;
    }

    private long insertLocked(SQLiteDatabase db, Uri uri, ContentValues values) {
        int match = sUriMatcher.match(uri);
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        long now = System.currentTimeMillis();
        boolean syncAdapter = isCallerSyncAdapter(uri);
        switch (match) {
            case RAW_CONTACTS:
            case PROFILE_RAW_CONTACTS: {
                boolean isProfile = (match == PROFILE_RAW_CONTACTS)
                        || asInt(copy, "raw_contact_is_user_profile") != 0;
                long contactId = copy.containsKey("contact_id") && copy.getAsLong("contact_id") != null
                        ? copy.getAsLong("contact_id") : -1L;
                if (contactId < 0) {
                    ContentValues contactValues = new ContentValues();
                    contactValues.put("contact_last_updated_timestamp", now);
                    contactValues.put("is_user_profile", isProfile ? 1 : 0);
                    if (copy.containsKey("display_name")) {
                        String dn = copy.getAsString("display_name");
                        contactValues.put("display_name", dn);
                        applyNameDerivedFields(contactValues, dn, dn, ContactsContract.DisplayNameSources.UNDEFINED);
                    }
                    contactId = db.insertOrThrow("contacts", null, contactValues);
                    copy.put("contact_id", contactId);
                }
                if (isProfile) {
                    copy.put("raw_contact_is_user_profile", 1);
                }
                if (!syncAdapter && !copy.containsKey("dirty")) {
                    copy.put("dirty", 1);
                }
                long rawId = db.insertOrThrow("raw_contacts", null, copy);
                ContentValues contactUpdate = new ContentValues();
                contactUpdate.put("lookup", "0r" + rawId);
                contactUpdate.put("name_raw_contact_id", rawId);
                contactUpdate.put("contact_last_updated_timestamp", now);
                db.update("contacts", contactUpdate, "_id = ? AND lookup IS NULL",
                        new String[] { Long.toString(contactId) });
                return rawId;
            }
            case RAW_CONTACTS_DATA: {
                long rawId = Long.parseLong(uri.getPathSegments().get(1));
                copy.put("raw_contact_id", rawId);
                return insertDataLocked(db, copy, null, now, syncAdapter);
            }
            case DATA:
            case PHONES:
            case EMAILS:
            case POSTALS:
                return insertDataLocked(db, copy, forcedMime(match), now, syncAdapter);
            case GROUPS:
                if (!syncAdapter && !copy.containsKey("dirty")) copy.put("dirty", 1);
                return db.insertOrThrow("groups", null, copy);
            case SETTINGS:
                db.insertWithOnConflict("settings", null, copy, SQLiteDatabase.CONFLICT_REPLACE);
                return 1;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private long insertDataLocked(SQLiteDatabase db, ContentValues copy, String forcedMime,
            long now, boolean syncAdapter) {
        String mimetype = forcedMime != null ? forcedMime : copy.getAsString("mimetype");
        copy.remove("mimetype");
        if (mimetype == null || mimetype.isEmpty()) {
            throw new IllegalArgumentException("Data insert requires mimetype");
        }
        if (MIME_PHONE.equals(mimetype)) {
            maybePopulateNormalizedPhone(copy, null);
        }
        copy.put("mimetype_id", resolveOrCreateMimetypeId(db, mimetype));
        long id = db.insertOrThrow("data", null, copy);
        Long rawContactId = copy.getAsLong("raw_contact_id");
        if (rawContactId != null) {
            refreshRawAndParentContacts(db, Collections.singleton(rawContactId), now, !syncAdapter);
        }
        return id;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CONTACTS);
        int match = sUriMatcher.match(uri);
        int count;
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                count = updateLocked(db, match, uri, values, selection, selectionArgs);
                db.setTransactionSuccessful();
                if (count > 0) {
                    if (mBatchDepth > 0) {
                        mBatchDirty = true;
                    } else {
                        notifyNow = true;
                    }
                }
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    private int updateLocked(SQLiteDatabase db, int match, Uri uri, ContentValues values,
            String selection, String[] selectionArgs) {
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        long now = System.currentTimeMillis();
        boolean syncAdapter = isCallerSyncAdapter(uri);
        switch (match) {
            case CONTACTS:
            case CONTACTS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                if (match == CONTACTS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, Long.toString(ContentUris.parseId(uri)));
                }
                copy.put("contact_last_updated_timestamp", now);
                return db.update("contacts", copy, effSel, effArgs);
            }
            case PROFILE: {
                copy.put("contact_last_updated_timestamp", now);
                return db.update("contacts", copy,
                        ContactsProviderSupport.combineSelection(selection, "is_user_profile = 1"),
                        selectionArgs);
            }
            case RAW_CONTACTS:
            case RAW_CONTACTS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                if (match == RAW_CONTACTS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, Long.toString(ContentUris.parseId(uri)));
                }
                Set<Long> rawIds = queryIds(db, "SELECT _id FROM raw_contacts"
                        + (effSel != null ? " WHERE " + effSel : ""), effArgs);
                int updated = db.update("raw_contacts", copy, effSel, effArgs);
                if (updated > 0) {
                    if (!syncAdapter && !copy.containsKey("dirty") && !copy.containsKey("version")) {
                        bumpRawContactsVersionAndDirty(db, rawIds);
                    }
                    if (copy.containsKey("display_name") || copy.containsKey("deleted")) {
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
            case POSTALS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                String forcedMime = forcedMime(match);
                if (forcedMime != null) {
                    effSel = ContactsProviderSupport.combineSelection(
                            effSel, "mimetype_id = (SELECT _id FROM mimetypes WHERE mimetype = ?)");
                    effArgs = ContactsProviderSupport.prependArgs(effArgs, forcedMime);
                }
                if (match == DATA_ID || match == PHONES_ID || match == EMAILS_ID || match == POSTALS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(effSel, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            effArgs, Long.toString(ContentUris.parseId(uri)));
                }
                if (copy.containsKey("mimetype")) {
                    String newMime = copy.getAsString("mimetype");
                    copy.remove("mimetype");
                    if (newMime != null && !newMime.isEmpty()) {
                        copy.put("mimetype_id", resolveOrCreateMimetypeId(db, newMime));
                    }
                }
                Set<Long> rawIds = queryIds(db, "SELECT DISTINCT raw_contact_id FROM data"
                        + (effSel != null ? " WHERE " + effSel : ""), effArgs);
                int updated;
                if (copy.containsKey("data1") && !copy.containsKey("data4")) {
                    // Populate data4 per-row only for phone rows cleanly.
                    updated = updateDataWithPhoneNormalization(db, copy, effSel, effArgs, forcedMime);
                } else {
                    updated = db.update("data", copy, effSel, effArgs);
                }
                if (updated > 0) {
                    refreshRawAndParentContacts(db, rawIds, now, !syncAdapter);
                }
                return updated;
            }
            case GROUPS:
            case GROUPS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                if (match == GROUPS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, Long.toString(ContentUris.parseId(uri)));
                }
                if (!syncAdapter && !copy.containsKey("dirty")) {
                    copy.put("dirty", 1);
                }
                return db.update("groups", copy, effSel, effArgs);
            }
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private int updateDataWithPhoneNormalization(SQLiteDatabase db, ContentValues copy,
            String selection, String[] args, String forcedMime) {
        if (MIME_PHONE.equals(forcedMime)) {
            ContentValues phoneCopy = new ContentValues(copy);
            maybePopulateNormalizedPhone(phoneCopy, null);
            return db.update("data", phoneCopy, selection, args);
        }
        String sql = "SELECT _id, (SELECT mimetype FROM mimetypes WHERE mimetypes._id = data.mimetype_id) "
                + "FROM data" + (selection != null ? " WHERE " + selection : "");
        List<Long> phoneIds = new ArrayList<>();
        List<Long> otherIds = new ArrayList<>();
        try (Cursor c = db.rawQuery(sql, args)) {
            while (c.moveToNext()) {
                long id = c.getLong(0);
                if (MIME_PHONE.equals(c.getString(1))) phoneIds.add(id);
                else otherIds.add(id);
            }
        }
        int total = 0;
        if (!phoneIds.isEmpty()) {
            ContentValues phoneCopy = new ContentValues(copy);
            maybePopulateNormalizedPhone(phoneCopy, null);
            total += db.update("data", phoneCopy, idInClause("_id", phoneIds), null);
        }
        if (!otherIds.isEmpty()) {
            total += db.update("data", copy, idInClause("_id", otherIds), null);
        }
        return total;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CONTACTS);
        int match = sUriMatcher.match(uri);
        int count;
        boolean notifyNow = false;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                count = deleteLocked(db, match, uri, selection, selectionArgs);
                db.setTransactionSuccessful();
                if (count > 0) {
                    if (mBatchDepth > 0) {
                        mBatchDirty = true;
                    } else {
                        notifyNow = true;
                    }
                }
            } finally {
                db.endTransaction();
            }
        }
        if (notifyNow) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    private int deleteLocked(SQLiteDatabase db, int match, Uri uri, String selection,
            String[] selectionArgs) {
        long now = System.currentTimeMillis();
        boolean syncAdapter = isCallerSyncAdapter(uri);
        switch (match) {
            case CONTACTS:
            case CONTACTS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                if (match == CONTACTS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, Long.toString(ContentUris.parseId(uri)));
                }
                Set<Long> contactIds = queryIds(db, "SELECT _id FROM contacts"
                        + (effSel != null ? " WHERE " + effSel : ""), effArgs);
                if (contactIds.isEmpty()) return 0;
                if (!syncAdapter) {
                    int softCount = 0;
                    for (long cid : contactIds) {
                        boolean hadRaw = hasRemainingRows(db, "raw_contacts", "contact_id = ?", cid);
                        int updated = softDeleteRawContacts(db, "contact_id = ? AND deleted = 0",
                                new String[] { Long.toString(cid) });
                        if (updated > 0 || !hadRaw) {
                            if (!hadRaw) {
                                db.delete("contacts", "_id = ?", new String[] { Long.toString(cid) });
                            }
                            softCount++;
                            recordDeletedContact(db, cid, now);
                        }
                    }
                    return softCount;
                }
                for (long cid : contactIds) {
                    db.delete("raw_contacts", "contact_id = ?", new String[] { Long.toString(cid) });
                }
                int deleted = db.delete("contacts", effSel, effArgs);
                for (long cid : contactIds) {
                    recordDeletedContact(db, cid, now);
                }
                return deleted;
            }
            case RAW_CONTACTS:
            case RAW_CONTACTS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                if (match == RAW_CONTACTS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, Long.toString(ContentUris.parseId(uri)));
                }
                Set<Long> contactIds = queryIds(db, "SELECT DISTINCT contact_id FROM raw_contacts"
                        + (effSel != null ? " WHERE " + effSel : ""), effArgs);
                int count;
                if (!syncAdapter) {
                    count = softDeleteRawContacts(db, effSel, effArgs);
                    for (long cid : contactIds) {
                        if (!hasRemainingRows(db, "raw_contacts", "contact_id = ? AND deleted = 0", cid)) {
                            recordDeletedContact(db, cid, now);
                        }
                    }
                } else {
                    count = db.delete("raw_contacts", effSel, effArgs);
                    for (long cid : contactIds) {
                        if (!hasRemainingRows(db, "raw_contacts", "contact_id = ?", cid)) {
                            db.delete("contacts", "_id = ?", new String[] { Long.toString(cid) });
                            recordDeletedContact(db, cid, now);
                        }
                    }
                }
                return count;
            }
            case DATA:
            case DATA_ID:
            case PHONES:
            case PHONES_ID:
            case EMAILS:
            case EMAILS_ID:
            case POSTALS:
            case POSTALS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                String forcedMime = forcedMime(match);
                if (forcedMime != null) {
                    effSel = ContactsProviderSupport.combineSelection(
                            effSel, "mimetype_id = (SELECT _id FROM mimetypes WHERE mimetype = ?)");
                    effArgs = ContactsProviderSupport.prependArgs(effArgs, forcedMime);
                }
                if (match == DATA_ID || match == PHONES_ID || match == EMAILS_ID || match == POSTALS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(effSel, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            effArgs, Long.toString(ContentUris.parseId(uri)));
                }
                Set<Long> rawIds = queryIds(db, "SELECT DISTINCT raw_contact_id FROM data"
                        + (effSel != null ? " WHERE " + effSel : ""), effArgs);
                int deleted = db.delete("data", effSel, effArgs);
                if (deleted > 0) {
                    refreshRawAndParentContacts(db, rawIds, now, !syncAdapter);
                }
                return deleted;
            }
            case GROUPS:
            case GROUPS_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                if (match == GROUPS_ID) {
                    effSel = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, Long.toString(ContentUris.parseId(uri)));
                }
                if (!syncAdapter) {
                    db.execSQL("UPDATE groups SET deleted = 1, dirty = 1, version = version + 1"
                            + (effSel != null ? " WHERE (" + effSel + ") AND deleted = 0"
                                    : " WHERE deleted = 0"),
                            effArgs == null ? new Object[0] : effArgs);
                    return changesCount(db);
                }
                return db.delete("groups", effSel, effArgs);
            }
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private static int softDeleteRawContacts(SQLiteDatabase db, String selection, String[] args) {
        String where = ContactsProviderSupport.combineSelection(selection, "deleted = 0");
        db.execSQL("UPDATE raw_contacts SET deleted = 1, dirty = 1, version = version + 1"
                + (where != null ? " WHERE " + where : ""),
                args == null ? new Object[0] : args);
        return changesCount(db);
    }

    private static int changesCount(SQLiteDatabase db) {
        try (Cursor c = db.rawQuery("SELECT changes()", null)) {
            return c.moveToFirst() ? c.getInt(0) : 0;
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

    private static void maybePopulateNormalizedPhone(ContentValues copy, String fallbackRaw) {
        if (!copy.containsKey("data4") || copy.getAsString("data4") == null) {
            String raw = copy.containsKey("data1") ? copy.getAsString("data1") : fallbackRaw;
            String normalized = ContactsProviderSupport.normalizePhoneNumber(raw);
            if (normalized != null) copy.put("data4", normalized);
        }
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
            try (Cursor c = db.rawQuery(
                    "SELECT _id FROM raw_contacts WHERE contact_id = ? AND deleted = 0 ORDER BY _id ASC",
                    new String[] { Long.toString(contactId) })) {
                while (c.moveToNext()) {
                    NameResolution candidate = resolveDisplayNameForRawContact(db, c.getLong(0));
                    if (candidate.source > best.source
                            || (candidate.source == best.source && best.displayName == null
                                    && candidate.displayName != null)) {
                        best = candidate;
                    }
                }
            }
            ContentValues update = new ContentValues();
            update.put("has_phone_number", hasPhone ? 1 : 0);
            update.put("contact_last_updated_timestamp", now);
            applyNameDerivedFields(update, best.displayName, best.displayNameAlt, best.source);
            db.update("contacts", update, "_id = ?", new String[] { Long.toString(contactId) });
        }
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
     * {@code Email} -> {@code Phone}.
     */
    private static NameResolution resolveDisplayNameForRawContact(SQLiteDatabase db, long rawContactId) {
        String[] order = new String[] {
                MIME_STRUCTURED_NAME, MIME_NICKNAME, MIME_ORGANIZATION, MIME_EMAIL, MIME_PHONE
        };
        int[] sources = new int[] {
                ContactsContract.DisplayNameSources.STRUCTURED_NAME,
                ContactsContract.DisplayNameSources.NICKNAME,
                ContactsContract.DisplayNameSources.ORGANIZATION,
                ContactsContract.DisplayNameSources.EMAIL,
                ContactsContract.DisplayNameSources.PHONE
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
        target.put("phonebook_label", phonebookLabel(displayName));
        target.put("phonebook_label_alt", phonebookLabel(displayNameAlt));
        target.put("phonebook_bucket", phonebookBucket(displayName));
        target.put("phonebook_bucket_alt", phonebookBucket(displayNameAlt));
    }

    private static String phonebookLabel(String name) {
        if (name == null || name.isEmpty()) return "";
        char c = Character.toUpperCase(name.charAt(0));
        if (c >= 'A' && c <= 'Z') return String.valueOf(c);
        return "#";
    }

    private static int phonebookBucket(String name) {
        if (name == null || name.isEmpty()) return 0;
        char c = Character.toUpperCase(name.charAt(0));
        if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
        return 27;
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

    private static int asInt(ContentValues values, String key) {
        Integer v = values.getAsInteger(key);
        return v == null ? 0 : v;
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
