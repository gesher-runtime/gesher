/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.UriMatcher;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.net.Uri;
import android.provider.ContactsContract;

import java.io.File;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Built-in SQLite-backed {@code content://com.android.contacts} and legacy
 * {@code content://contacts} provider. Starts with an empty address book and
 * local directory entry, and enforces {@code READ_CONTACTS} /
 * {@code WRITE_CONTACTS} via {@link PackageManagerService}.
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

    private static final int LEGACY_PEOPLE = 100;
    private static final int LEGACY_PEOPLE_ID = 101;
    private static final int LEGACY_PHONES = 102;
    private static final int LEGACY_PHONES_ID = 103;
    private static final int LEGACY_PHONES_FILTER = 104;
    private static final int LEGACY_CONTACT_METHODS = 105;
    private static final int LEGACY_GROUPS = 106;

    private static final String MIME_STRUCTURED_NAME = "vnd.android.cursor.item/name";
    private static final String MIME_PHONE = "vnd.android.cursor.item/phone_v2";
    private static final String MIME_EMAIL = "vnd.android.cursor.item/email_v2";
    private static final String MIME_POSTAL = "vnd.android.cursor.item/postal-address_v2";

    private static final int DB_VERSION = 1;
    private static final Uri NOTIFY_URI = Uri.parse("content://com.android.contacts");

    private static final UriMatcher sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);
    private static final Map<String, String> sDataProjectionMap = new HashMap<>();

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

        String legacy = ContactsProviderSupport.LEGACY_CONTACTS_AUTHORITY;
        sUriMatcher.addURI(legacy, "people", LEGACY_PEOPLE);
        sUriMatcher.addURI(legacy, "people/#", LEGACY_PEOPLE_ID);
        sUriMatcher.addURI(legacy, "phones", LEGACY_PHONES);
        sUriMatcher.addURI(legacy, "phones/#", LEGACY_PHONES_ID);
        sUriMatcher.addURI(legacy, "phones/filter/*", LEGACY_PHONES_FILTER);
        sUriMatcher.addURI(legacy, "contact_methods", LEGACY_CONTACT_METHODS);
        sUriMatcher.addURI(legacy, "groups", LEGACY_GROUPS);

        for (String col : new String[] {
                "_id", "raw_contact_id", "contact_id", "lookup", "mimetype",
                "is_primary", "is_super_primary", "data_version",
                "data1", "data2", "data3", "data4", "data5", "data6", "data7", "data8",
                "data9", "data10", "data11", "data12", "data13", "data14", "data15",
                "data_sync1", "data_sync2", "data_sync3", "data_sync4", "carrier_presence",
                "display_name", "display_name_alt", "display_name_source", "phonetic_name",
                "photo_id", "photo_uri", "photo_thumb_uri", "in_visible_group",
                "has_phone_number", "starred", "pinned", "custom_ringtone", "send_to_voicemail",
                "times_contacted", "last_time_contacted", "contact_last_updated_timestamp",
                "account_name", "account_type", "data_set", "sourceid", "version",
                "dirty", "deleted", "name_raw_contact_id"
        }) {
            sDataProjectionMap.put(col, col);
        }
    }

    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final File mDatabase;
    private final Object mDbLock = new Object();
    private SQLiteDatabase mDb;
    private boolean mInitialized;
    private Throwable mInitializationFailure;

    ContactsProvider(PackageManagerService pm, ActivityTaskManagerService atm) {
        this(pm, atm, new File(ContactsProviderSupport.dataDirectory(), "databases/contacts2.db"));
    }

    ContactsProvider(PackageManagerService pm, ActivityTaskManagerService atm, File databaseFile) {
        mPm = pm;
        mCallers = pm == null ? null : new CallerIdentity(pm, atm);
        mDatabase = databaseFile.getAbsoluteFile();
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
                attachInfo(ContactsProviderSupport.contextForProvider(),
                        ContactsProviderSupport.withoutPermissions(declared));
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
                if (db.getVersion() < DB_VERSION) {
                    db.beginTransaction();
                    try {
                        createSchema(db);
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

    private static void createSchema(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE IF NOT EXISTS contacts ("
                + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "lookup TEXT,"
                + "name_raw_contact_id INTEGER,"
                + "photo_id INTEGER DEFAULT 0,"
                + "photo_uri TEXT,"
                + "photo_thumb_uri TEXT,"
                + "display_name TEXT,"
                + "display_name_alt TEXT,"
                + "display_name_source INTEGER DEFAULT 0,"
                + "phonetic_name TEXT,"
                + "in_visible_group INTEGER NOT NULL DEFAULT 1,"
                + "has_phone_number INTEGER NOT NULL DEFAULT 0,"
                + "times_contacted INTEGER NOT NULL DEFAULT 0,"
                + "last_time_contacted INTEGER,"
                + "starred INTEGER NOT NULL DEFAULT 0,"
                + "pinned INTEGER NOT NULL DEFAULT 0,"
                + "custom_ringtone TEXT,"
                + "send_to_voicemail INTEGER DEFAULT 0,"
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
                + "starred INTEGER NOT NULL DEFAULT 0,"
                + "pinned INTEGER NOT NULL DEFAULT 0,"
                + "custom_ringtone TEXT,"
                + "send_to_voicemail INTEGER DEFAULT 0,"
                + "times_contacted INTEGER NOT NULL DEFAULT 0,"
                + "last_time_contacted INTEGER,"
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
                + "title TEXT,"
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
        db.execSQL("CREATE VIEW IF NOT EXISTS view_data AS SELECT "
                + "data._id AS _id,"
                + "data.raw_contact_id AS raw_contact_id,"
                + "raw_contacts.contact_id AS contact_id,"
                + "contacts.lookup AS lookup,"
                + "mimetypes.mimetype AS mimetype,"
                + "data.is_primary AS is_primary,"
                + "data.is_super_primary AS is_super_primary,"
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
                + "contacts.photo_id AS photo_id,"
                + "contacts.photo_uri AS photo_uri,"
                + "contacts.photo_thumb_uri AS photo_thumb_uri,"
                + "contacts.in_visible_group AS in_visible_group,"
                + "contacts.has_phone_number AS has_phone_number,"
                + "contacts.starred AS starred,"
                + "contacts.pinned AS pinned,"
                + "contacts.custom_ringtone AS custom_ringtone,"
                + "contacts.send_to_voicemail AS send_to_voicemail,"
                + "contacts.times_contacted AS times_contacted,"
                + "contacts.last_time_contacted AS last_time_contacted,"
                + "contacts.contact_last_updated_timestamp AS contact_last_updated_timestamp,"
                + "raw_contacts.account_name AS account_name,"
                + "raw_contacts.account_type AS account_type,"
                + "raw_contacts.data_set AS data_set,"
                + "raw_contacts.sourceid AS sourceid,"
                + "raw_contacts.version AS version,"
                + "raw_contacts.dirty AS dirty,"
                + "raw_contacts.deleted AS deleted,"
                + "contacts.name_raw_contact_id AS name_raw_contact_id "
                + "FROM data "
                + "JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                + "JOIN raw_contacts ON data.raw_contact_id = raw_contacts._id "
                + "LEFT JOIN contacts ON raw_contacts.contact_id = contacts._id;");
        db.execSQL("CREATE VIEW IF NOT EXISTS view_phone_lookup AS SELECT "
                + "contacts._id AS _id,"
                + "contacts._id AS contact_id,"
                + "contacts.lookup AS lookup,"
                + "contacts.display_name AS display_name,"
                + "data.data1 AS number,"
                + "data.data2 AS type,"
                + "data.data3 AS label,"
                + "COALESCE(data.data4, data.data1) AS normalized_number,"
                + "contacts.photo_id AS photo_id,"
                + "contacts.photo_uri AS photo_uri,"
                + "contacts.photo_thumb_uri AS photo_thumb_uri,"
                + "contacts.starred AS starred,"
                + "contacts.custom_ringtone AS custom_ringtone,"
                + "contacts.send_to_voicemail AS send_to_voicemail,"
                + "contacts.in_visible_group AS in_visible_group,"
                + "contacts.has_phone_number AS has_phone_number,"
                + "contacts.times_contacted AS times_contacted,"
                + "contacts.last_time_contacted AS last_time_contacted "
                + "FROM data "
                + "JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                + "JOIN raw_contacts ON data.raw_contact_id = raw_contacts._id "
                + "JOIN contacts ON raw_contacts.contact_id = contacts._id "
                + "WHERE mimetypes.mimetype = '" + MIME_PHONE + "' AND raw_contacts.deleted = 0;");
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
            case LEGACY_PEOPLE:
                return ContactsContract.Contacts.CONTENT_TYPE;
            case CONTACTS_ID:
            case CONTACTS_LOOKUP:
            case CONTACTS_LOOKUP_ID:
            case PROFILE:
            case LEGACY_PEOPLE_ID:
                return ContactsContract.Contacts.CONTENT_ITEM_TYPE;
            case RAW_CONTACTS:
            case PROFILE_RAW_CONTACTS:
                return ContactsContract.RawContacts.CONTENT_TYPE;
            case RAW_CONTACTS_ID:
                return ContactsContract.RawContacts.CONTENT_ITEM_TYPE;
            case CONTACTS_DATA:
            case RAW_CONTACTS_DATA:
            case DATA:
            case PROFILE_DATA:
            case LEGACY_CONTACT_METHODS:
                return ContactsContract.Data.CONTENT_TYPE;
            case DATA_ID:
                return "vnd.android.cursor.item/data";
            case PHONES:
            case PHONES_FILTER_EMPTY:
            case PHONES_FILTER:
            case LEGACY_PHONES:
            case LEGACY_PHONES_FILTER:
                return ContactsContract.CommonDataKinds.Phone.CONTENT_TYPE;
            case PHONES_ID:
            case LEGACY_PHONES_ID:
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
            case LEGACY_GROUPS:
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
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.READ_CONTACTS);
        int match = sUriMatcher.match(uri);
        String limit = ContactsProviderSupport.parseLimit(uri);
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        String effSelection = selection;
        String[] effArgs = selectionArgs;
        switch (match) {
            case CONTACTS:
            case LEGACY_PEOPLE:
                qb.setTables("contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection, "is_user_profile = 0");
                break;
            case CONTACTS_ID:
            case LEGACY_PEOPLE_ID:
                qb.setTables("contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case CONTACTS_FILTER_EMPTY:
                qb.setTables("contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection, "is_user_profile = 0");
                break;
            case CONTACTS_FILTER: {
                String pattern = "%" + uri.getLastPathSegment() + "%";
                qb.setTables("contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        "is_user_profile = 0 AND display_name LIKE ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, pattern);
                break;
            }
            case CONTACTS_LOOKUP: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection, "lookup = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, segments.get(2));
                break;
            }
            case CONTACTS_LOOKUP_ID: {
                List<String> segments = uri.getPathSegments();
                qb.setTables("contacts");
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
                qb.setTables("raw_contacts");
                break;
            case RAW_CONTACTS_ID:
                qb.setTables("raw_contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
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
            case LEGACY_CONTACT_METHODS:
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
            case LEGACY_PHONES:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ?");
                effArgs = ContactsProviderSupport.prependArgs(selectionArgs, MIME_PHONE);
                break;
            case PHONES_ID:
            case LEGACY_PHONES_ID:
                qb.setTables("view_data");
                qb.setProjectionMap(sDataProjectionMap);
                effSelection = ContactsProviderSupport.combineSelection(selection, "mimetype = ? AND _id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, MIME_PHONE, Long.toString(ContentUris.parseId(uri)));
                break;
            case PHONES_FILTER:
            case LEGACY_PHONES_FILTER: {
                String query = uri.getLastPathSegment();
                String pattern = "%" + query + "%";
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
            case PHONE_LOOKUP: {
                String rawNumber = uri.getLastPathSegment();
                String digits = normalizeDigits(rawNumber);
                qb.setTables("view_phone_lookup");
                if (digits.isEmpty()) {
                    effSelection = ContactsProviderSupport.combineSelection(
                            selection, "(number = ? OR normalized_number = ?)");
                    effArgs = ContactsProviderSupport.prependArgs(selectionArgs, rawNumber, rawNumber);
                } else {
                    String strippedExpr = "REPLACE(REPLACE(REPLACE(REPLACE(REPLACE(number,'-',''),' ',''),'(',''),')',''),'+','')";
                    effSelection = ContactsProviderSupport.combineSelection(selection,
                            "(number = ? OR normalized_number = ? OR " + strippedExpr + " = ? OR " + strippedExpr + " LIKE ?)");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, rawNumber, digits, digits, "%" + digits);
                }
                break;
            }
            case GROUPS:
            case GROUPS_SUMMARY:
            case LEGACY_GROUPS:
                qb.setTables("groups");
                effSelection = ContactsProviderSupport.combineSelection(selection, "deleted = 0");
                break;
            case GROUPS_ID:
                qb.setTables("groups");
                effSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case DELETED_CONTACTS:
                qb.setTables("deleted_contacts");
                break;
            case DIRECTORIES:
                qb.setTables("directories");
                break;
            case DIRECTORIES_ID:
                qb.setTables("directories");
                effSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case SETTINGS:
                qb.setTables("settings");
                break;
            case PROFILE:
                qb.setTables("contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection, "is_user_profile = 1");
                break;
            case PROFILE_RAW_CONTACTS:
                qb.setTables("raw_contacts");
                effSelection = ContactsProviderSupport.combineSelection(selection,
                        "contact_id IN (SELECT _id FROM contacts WHERE is_user_profile = 1)");
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
            cursor = qb.query(openDatabase(), projection, effSelection, effArgs,
                    null, null, sortOrder, limit);
        }
        ContactsProviderSupport.setNotificationUri(cursor, getContext(), uri);
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CONTACTS);
        long id;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                id = insertLocked(db, uri, values);
                db.setTransactionSuccessful();
            } finally {
                db.endTransaction();
            }
        }
        ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        return ContentUris.withAppendedId(uri, id);
    }

    @Override
    public int bulkInsert(Uri uri, ContentValues[] values) {
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CONTACTS);
        if (values == null || values.length == 0) return 0;
        int inserted = 0;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                for (ContentValues item : values) {
                    if (insertLocked(db, uri, item) > 0) inserted++;
                }
                db.setTransactionSuccessful();
            } finally {
                db.endTransaction();
            }
        }
        if (inserted > 0) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return inserted;
    }

    private long insertLocked(SQLiteDatabase db, Uri uri, ContentValues values) {
        int match = sUriMatcher.match(uri);
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        long now = System.currentTimeMillis();
        switch (match) {
            case RAW_CONTACTS: {
                long contactId = copy.containsKey("contact_id") && copy.getAsLong("contact_id") != null
                        ? copy.getAsLong("contact_id") : -1L;
                if (contactId < 0) {
                    ContentValues contactValues = new ContentValues();
                    contactValues.put("contact_last_updated_timestamp", now);
                    if (copy.containsKey("display_name")) {
                        contactValues.put("display_name", copy.getAsString("display_name"));
                    }
                    contactId = db.insertOrThrow("contacts", null, contactValues);
                    copy.put("contact_id", contactId);
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
                return insertDataRowLocked(db, copy, null, now);
            }
            case DATA:
                return insertDataRowLocked(db, copy, null, now);
            case PHONES:
                return insertDataRowLocked(db, copy, MIME_PHONE, now);
            case EMAILS:
                return insertDataRowLocked(db, copy, MIME_EMAIL, now);
            case POSTALS:
                return insertDataRowLocked(db, copy, MIME_POSTAL, now);
            case GROUPS:
            case LEGACY_GROUPS:
                return db.insertOrThrow("groups", null, copy);
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private long insertDataRowLocked(SQLiteDatabase db, ContentValues copy, String inferredMime,
            long now) {
        String mimetype = copy.getAsString("mimetype");
        copy.remove("mimetype");
        if (mimetype == null || mimetype.isEmpty()) mimetype = inferredMime;
        if (mimetype == null || mimetype.isEmpty()) {
            throw new IllegalArgumentException("mimetype is required for data insert");
        }
        Long rawId = copy.getAsLong("raw_contact_id");
        if (rawId == null || rawId <= 0) {
            throw new IllegalArgumentException("raw_contact_id is required for data insert");
        }
        long mimetypeId = resolveOrCreateMimetypeId(db, mimetype);
        copy.put("mimetype_id", mimetypeId);
        long dataId = db.insertOrThrow("data", null, copy);
        refreshParentContactForRawContact(db, rawId, mimetype, copy.getAsString("data1"), now);
        return dataId;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CONTACTS);
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        int match = sUriMatcher.match(uri);
        int count;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                count = updateLocked(db, match, uri, copy, selection, selectionArgs);
                db.setTransactionSuccessful();
            } finally {
                db.endTransaction();
            }
        }
        if (count > 0) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    private int updateLocked(SQLiteDatabase db, int match, Uri uri, ContentValues copy,
            String selection, String[] selectionArgs) {
        long now = System.currentTimeMillis();
        switch (match) {
            case CONTACTS:
            case LEGACY_PEOPLE:
                copy.put("contact_last_updated_timestamp", now);
                return db.update("contacts", copy, selection, selectionArgs);
            case CONTACTS_ID:
            case LEGACY_PEOPLE_ID:
                copy.put("contact_last_updated_timestamp", now);
                return db.update("contacts", copy,
                        ContactsProviderSupport.combineSelection(selection, "_id = ?"),
                        ContactsProviderSupport.prependArgs(
                                selectionArgs, Long.toString(ContentUris.parseId(uri))));
            case RAW_CONTACTS:
                return db.update("raw_contacts", copy, selection, selectionArgs);
            case RAW_CONTACTS_ID:
                return db.update("raw_contacts", copy,
                        ContactsProviderSupport.combineSelection(selection, "_id = ?"),
                        ContactsProviderSupport.prependArgs(
                                selectionArgs, Long.toString(ContentUris.parseId(uri))));
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
                int updated = db.update("data", copy, effSel, effArgs);
                if (updated > 0) {
                    refreshParentContacts(db, rawIds, now);
                }
                return updated;
            }
            case GROUPS:
            case LEGACY_GROUPS:
                return db.update("groups", copy, selection, selectionArgs);
            case GROUPS_ID:
                return db.update("groups", copy,
                        ContactsProviderSupport.combineSelection(selection, "_id = ?"),
                        ContactsProviderSupport.prependArgs(
                                selectionArgs, Long.toString(ContentUris.parseId(uri))));
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CONTACTS);
        int match = sUriMatcher.match(uri);
        int count;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                count = deleteLocked(db, match, uri, selection, selectionArgs);
                db.setTransactionSuccessful();
            } finally {
                db.endTransaction();
            }
        }
        if (count > 0) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    private int deleteLocked(SQLiteDatabase db, int match, Uri uri, String selection,
            String[] selectionArgs) {
        long now = System.currentTimeMillis();
        switch (match) {
            case CONTACTS:
            case CONTACTS_ID:
            case LEGACY_PEOPLE:
            case LEGACY_PEOPLE_ID: {
                String effSel = selection;
                String[] effArgs = selectionArgs;
                if (match == CONTACTS_ID || match == LEGACY_PEOPLE_ID) {
                    effSel = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                    effArgs = ContactsProviderSupport.prependArgs(
                            selectionArgs, Long.toString(ContentUris.parseId(uri)));
                }
                Set<Long> contactIds = queryIds(db, "SELECT _id FROM contacts"
                        + (effSel != null ? " WHERE " + effSel : ""), effArgs);
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
                        + (effSel != null ? " WHERE contact_id IS NOT NULL AND (" + effSel + ")"
                                : " WHERE contact_id IS NOT NULL"), effArgs);
                int deleted = db.delete("raw_contacts", effSel, effArgs);
                for (long cid : contactIds) {
                    if (!hasRemainingRows(db, "raw_contacts", "contact_id = ? AND deleted = 0", cid)) {
                        if (db.delete("contacts", "_id = ?", new String[] { Long.toString(cid) }) > 0) {
                            recordDeletedContact(db, cid, now);
                        }
                    }
                }
                return deleted;
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
                    refreshParentContacts(db, rawIds, now);
                }
                return deleted;
            }
            case GROUPS:
            case LEGACY_GROUPS:
                return db.delete("groups", selection, selectionArgs);
            case GROUPS_ID:
                return db.delete("groups",
                        ContactsProviderSupport.combineSelection(selection, "_id = ?"),
                        ContactsProviderSupport.prependArgs(
                                selectionArgs, Long.toString(ContentUris.parseId(uri))));
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private static String forcedMime(int match) {
        switch (match) {
            case PHONES:
            case PHONES_ID:
                return MIME_PHONE;
            case EMAILS:
            case EMAILS_ID:
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

    private static void refreshParentContactForRawContact(SQLiteDatabase db, long rawContactId,
            String mimetype, String data1, long now) {
        long contactId = -1L;
        try (Cursor c = db.rawQuery("SELECT contact_id FROM raw_contacts WHERE _id = ?",
                new String[] { Long.toString(rawContactId) })) {
            if (c.moveToFirst() && !c.isNull(0)) contactId = c.getLong(0);
        }
        if (contactId <= 0) return;
        ContentValues update = new ContentValues();
        update.put("contact_last_updated_timestamp", now);
        if (MIME_STRUCTURED_NAME.equals(mimetype) && data1 != null && !data1.isEmpty()) {
            update.put("display_name", data1);
        }
        if (MIME_PHONE.equals(mimetype)) {
            update.put("has_phone_number", 1);
        }
        db.update("contacts", update, "_id = ?", new String[] { Long.toString(contactId) });
    }

    private static void refreshParentContacts(SQLiteDatabase db, Set<Long> rawContactIds, long now) {
        Set<Long> contactIds = new HashSet<>();
        for (long rawId : rawContactIds) {
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
                            + "WHERE raw_contacts.contact_id = ? AND mimetypes.mimetype = ? LIMIT 1",
                    new String[] { Long.toString(contactId), MIME_PHONE })) {
                hasPhone = c.moveToFirst();
            }
            String displayName = null;
            try (Cursor c = db.rawQuery(
                    "SELECT data.data1 FROM data JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                            + "JOIN raw_contacts ON data.raw_contact_id = raw_contacts._id "
                            + "WHERE raw_contacts.contact_id = ? AND mimetypes.mimetype = ? "
                            + "ORDER BY data.is_super_primary DESC, data.is_primary DESC, data._id ASC LIMIT 1",
                    new String[] { Long.toString(contactId), MIME_STRUCTURED_NAME })) {
                if (c.moveToFirst()) displayName = c.getString(0);
            }
            ContentValues update = new ContentValues();
            update.put("has_phone_number", hasPhone ? 1 : 0);
            update.put("display_name", displayName);
            update.put("contact_last_updated_timestamp", now);
            db.update("contacts", update, "_id = ?", new String[] { Long.toString(contactId) });
        }
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

    private static String normalizeDigits(String input) {
        if (input == null) return "";
        StringBuilder sb = new StringBuilder(input.length());
        for (int i = 0; i < input.length(); i++) {
            char ch = input.charAt(i);
            if (ch >= '0' && ch <= '9') sb.append(ch);
        }
        return sb.toString();
    }
}
