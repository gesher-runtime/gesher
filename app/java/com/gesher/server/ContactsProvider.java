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
import android.content.Context;
import android.content.Intent;
import android.content.OperationApplicationException;
import android.content.PermissionChecker;
import android.content.UriMatcher;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.icu.text.AlphabeticIndex;
import android.net.Uri;
import android.os.Binder;
import android.os.Process;
import android.os.UserHandle;
import android.provider.ContactsContract;
import android.telephony.PhoneNumberUtils;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Built-in SQLite-backed {@code content://com.android.contacts} provider for
 * {@link ContactsContract}. Starts with an empty address book and a local default
 * directory entry, enforces {@code READ_CONTACTS} / {@code WRITE_CONTACTS}, AppOps,
 * and URI grants, and implements transactional {@link #applyBatch}, sync-adapter
 * soft deletes for account-backed rows alongside immediate hard deletes for local rows,
 * strict {@link SQLiteQueryBuilder} view resolution, E.164 {@code normalized_number},
 * ICU {@link AlphabeticIndex} labels and {@code COLLATE LOCALIZED} sort keys, loose
 * {@code phone_lookup} matching via {@code PHONE_NUMBERS_EQUAL}, entity iterators,
 * {@code provider_status}, {@code strequent}, {@code callables}, {@code usagefeedback},
 * and {@code contacts/#/photo}.
 *
 * <p><b>Limitation (1:1 contacts without multi-raw-contact aggregation):</b>
 * Inserting a {@code raw_contact} without an explicit {@code contact_id} creates a
 * 1:1 {@code contacts} row; automatic cross-account raw-contact aggregation and
 * split/join heuristics are not implemented.
 *
 * <p><b>Intentionally omitted optional endpoints:</b> legacy {@code content://contacts}
 * URIs, full-resolution {@code display_photo/#}, {@code search_snippets},
 * {@code stream_items}, {@code status_updates}, and managed-profile {@code *_corp}
 * URIs (single primary user only).
 */
final class ContactsProvider extends ContentProvider {
    private static final int CONTACTS = 1;
    private static final int CONTACTS_ID = 2;
    private static final int CONTACTS_FILTER_EMPTY = 3;
    private static final int CONTACTS_FILTER = 4;
    private static final int CONTACTS_LOOKUP = 5;
    private static final int CONTACTS_LOOKUP_ID = 6;
    private static final int CONTACTS_DATA = 7;
    private static final int CONTACTS_ENTITIES = 8;
    private static final int CONTACTS_STREQUENT = 9;
    private static final int CONTACTS_STREQUENT_FILTER_EMPTY = 13;
    private static final int CONTACTS_STREQUENT_FILTER = 14;
    private static final int CONTACTS_PHOTO = 15;
    private static final int CONTACTS_DELETE_USAGE = 16;

    private static final int RAW_CONTACTS = 10;
    private static final int RAW_CONTACTS_ID = 11;
    private static final int RAW_CONTACTS_DATA = 12;
    private static final int RAW_CONTACTS_ENTITY = 17;
    private static final int RAW_CONTACT_ENTITIES = 18;
    private static final int PROFILE_RAW_CONTACT_ENTITIES = 19;

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
    private static final int CALLABLES = 32;
    private static final int CALLABLES_ID = 33;
    private static final int CALLABLES_FILTER_EMPTY = 34;
    private static final int CALLABLES_FILTER = 35;
    private static final int DATA_USAGEFEEDBACK = 36;
    private static final int DATA_USAGEFEEDBACK_ID = 37;

    private static final int PHONE_LOOKUP = 40;
    private static final int PHONE_LOOKUP_ENTERPRISE = 41;
    private static final int GROUPS = 50;
    private static final int GROUPS_ID = 51;
    private static final int GROUPS_SUMMARY = 52;
    private static final int DELETED_CONTACTS = 60;
    private static final int DIRECTORIES = 70;
    private static final int DIRECTORIES_ID = 71;
    private static final int SETTINGS = 80;
    private static final int PROVIDER_STATUS = 85;
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
    private static final String MIME_SIP =
            ContactsContract.CommonDataKinds.SipAddress.CONTENT_ITEM_TYPE;
    private static final String MIME_PHOTO =
            ContactsContract.CommonDataKinds.Photo.CONTENT_ITEM_TYPE;

    private static final int DB_VERSION = 1;
    private static final int ID_CHUNK_SIZE = 500;
    private static final Uri NOTIFY_URI = ContactsContract.AUTHORITY_URI;

    private static final UriMatcher sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);
    private static final Map<String, String> sContactsProjectionMap;
    private static final Map<String, String> sRawContactsProjectionMap;
    private static final Map<String, String> sDataProjectionMap;
    private static final Map<String, String> sEntitiesProjectionMap;
    private static final Map<String, String> sPhoneLookupProjectionMap;
    private static final Map<String, String> sGroupsProjectionMap;
    private static final Map<String, String> sDeletedContactsProjectionMap;
    private static final Map<String, String> sDirectoriesProjectionMap;
    private static final Map<String, String> sSettingsProjectionMap;
    private static final Map<String, String> sProviderStatusProjectionMap;

    private static final Set<String> sWritableContactsColumns;
    private static final Set<String> sWritableRawContactsColumns;
    private static final Set<String> sWritableDataColumns;
    private static final Set<String> sWritableGroupsColumns;
    private static final Set<String> sWritableSettingsColumns;
    private static final Set<String> sWritableUsageFeedbackColumns;

    private static Locale sCachedIndexLocale;
    private static AlphabeticIndex.ImmutableIndex<Void> sCachedIndex;

    private static Map<String, String> identityMap(String... cols) {
        Map<String, String> map = new HashMap<>();
        for (String col : cols) map.put(col, col);
        return Collections.unmodifiableMap(map);
    }

    private static Set<String> columnSet(String... cols) {
        Set<String> set = new HashSet<>();
        Collections.addAll(set, cols);
        return Collections.unmodifiableSet(set);
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
        sUriMatcher.addURI(auth, "contacts/#/entities", CONTACTS_ENTITIES);
        sUriMatcher.addURI(auth, "contacts/#/photo", CONTACTS_PHOTO);
        sUriMatcher.addURI(auth, "contacts/strequent", CONTACTS_STREQUENT);
        sUriMatcher.addURI(auth, "contacts/strequent/filter", CONTACTS_STREQUENT_FILTER_EMPTY);
        sUriMatcher.addURI(auth, "contacts/strequent/filter/*", CONTACTS_STREQUENT_FILTER);
        sUriMatcher.addURI(auth, "contacts/delete_usage", CONTACTS_DELETE_USAGE);

        sUriMatcher.addURI(auth, "raw_contacts", RAW_CONTACTS);
        sUriMatcher.addURI(auth, "raw_contacts/#", RAW_CONTACTS_ID);
        sUriMatcher.addURI(auth, "raw_contacts/#/data", RAW_CONTACTS_DATA);
        sUriMatcher.addURI(auth, "raw_contacts/#/entity", RAW_CONTACTS_ENTITY);
        sUriMatcher.addURI(auth, "raw_contact_entities", RAW_CONTACT_ENTITIES);
        sUriMatcher.addURI(auth, "profile/raw_contact_entities", PROFILE_RAW_CONTACT_ENTITIES);

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
        sUriMatcher.addURI(auth, "data/callables", CALLABLES);
        sUriMatcher.addURI(auth, "data/callables/#", CALLABLES_ID);
        sUriMatcher.addURI(auth, "data/callables/filter", CALLABLES_FILTER_EMPTY);
        sUriMatcher.addURI(auth, "data/callables/filter/*", CALLABLES_FILTER);
        sUriMatcher.addURI(auth, "data/usagefeedback", DATA_USAGEFEEDBACK);
        sUriMatcher.addURI(auth, "data/usagefeedback/*", DATA_USAGEFEEDBACK_ID);

        sUriMatcher.addURI(auth, "phone_lookup/*", PHONE_LOOKUP);
        sUriMatcher.addURI(auth, "phone_lookup_enterprise/*", PHONE_LOOKUP_ENTERPRISE);
        sUriMatcher.addURI(auth, "groups", GROUPS);
        sUriMatcher.addURI(auth, "groups/#", GROUPS_ID);
        sUriMatcher.addURI(auth, "groups_summary", GROUPS_SUMMARY);
        sUriMatcher.addURI(auth, "deleted_contacts", DELETED_CONTACTS);
        sUriMatcher.addURI(auth, "directories", DIRECTORIES);
        sUriMatcher.addURI(auth, "directories/#", DIRECTORIES_ID);
        sUriMatcher.addURI(auth, "settings", SETTINGS);
        sUriMatcher.addURI(auth, "provider_status", PROVIDER_STATUS);
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

        sEntitiesProjectionMap = identityMap(
                "_id", "raw_contact_id", "contact_id", "data_id", "lookup",
                "name_raw_contact_id", "mimetype", "is_primary", "is_super_primary",
                "is_read_only", "data_version",
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
                "contact_status", "contact_status_ts", "contact_status_res_package",
                "contact_status_label", "contact_status_icon",
                "contact_last_updated_timestamp", "account_name", "account_type",
                "data_set", "sourceid", "version", "dirty", "deleted",
                "aggregation_mode", "sync1", "sync2", "sync3", "sync4",
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

        sProviderStatusProjectionMap = identityMap(
                "status", "database_creation_timestamp");

        sWritableContactsColumns = columnSet(
                "starred", "pinned", "custom_ringtone", "send_to_voicemail",
                "times_contacted", "last_time_contacted", "times_used",
                "last_time_used");

        sWritableRawContactsColumns = columnSet(
                "_id", "contact_id", "account_name", "account_type", "data_set",
                "sourceid", "version", "dirty", "deleted", "aggregation_mode",
                "display_name", "phonetic_name", "starred", "pinned",
                "custom_ringtone", "send_to_voicemail", "times_contacted",
                "times_used", "last_time_contacted", "last_time_used",
                "raw_contact_is_user_profile", "sync1", "sync2", "sync3", "sync4");

        sWritableDataColumns = columnSet(
                "_id", "raw_contact_id", "mimetype", "mimetype_id", "is_primary",
                "is_super_primary", "is_read_only", "data_version",
                "data1", "data2", "data3", "data4", "data5", "data6", "data7",
                "data8", "data9", "data10", "data11", "data12", "data13",
                "data14", "data15", "data_sync1", "data_sync2", "data_sync3",
                "data_sync4", "carrier_presence");

        sWritableGroupsColumns = columnSet(
                "_id", "account_name", "account_type", "data_set", "sourceid",
                "version", "dirty", "title", "title_res", "res_package", "notes",
                "system_id", "deleted", "group_visible", "should_sync",
                "auto_add", "favorites", "group_is_read_only",
                "sync1", "sync2", "sync3", "sync4");

        sWritableSettingsColumns = columnSet(
                "account_name", "account_type", "data_set", "ungrouped_visible",
                "should_sync", "any_synced", "ungrouped_count",
                "ungrouped_with_phones");

        sWritableUsageFeedbackColumns = columnSet("times_used", "last_time_used");
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
    private boolean mStrictPhoneComparison;
    private int mPhoneMinMatch = 7;
    private Throwable mInitializationFailure;
    private int mBatchDepth;
    private boolean mBatchDirty;

    ContactsProvider(ContentProviders providers, UriGrants grants) {
        this(providers, grants,
                new File(ContactsProviderSupport.dataDirectory(), "databases/contacts2.db"));
    }

    ContactsProvider(ContentProviders providers, UriGrants grants, File databaseFile) {
        mCallers = providers == null ? null : providers.callers();
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
        attachTo(android.app.GesherSettingsContext.get());
        return ContactsProviderSupport.holder(info(), getIContentProvider());
    }

    synchronized void attachTo(Context context) {
        if (mInitializationFailure != null) {
            throw new IllegalStateException("Contacts provider initialization failed",
                    mInitializationFailure);
        }
        if (mInitialized) return;
        try {
            attachInfo(ContactsProviderSupport.contextForProvider(context, mGrants, mRegistration),
                    info());
            mInitialized = true;
        } catch (RuntimeException | Error failure) {
            mInitializationFailure = failure;
            shutdown();
            throw failure;
        }
    }

    @Override
    protected int enforceReadPermissionInner(Uri uri, AttributionSource source) {
        return enforceContactsPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION,
                source, ContactsProviderSupport.READ_CONTACTS);
    }

    @Override
    protected int enforceWritePermissionInner(Uri uri, AttributionSource source) {
        return enforceContactsPermission(uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
                source, ContactsProviderSupport.WRITE_CONTACTS);
    }

    private int enforceContactsPermission(Uri uri, int uriModeFlag,
            AttributionSource source, String permission) {
        if (Binder.getCallingPid() == Process.myPid()) return PermissionChecker.PERMISSION_GRANTED;
        if (UserHandle.getUserId(Binder.getCallingUid()) != 0) {
            throw new SecurityException("Contacts access restricted to primary user");
        }
        ContactsProviderSupport.authenticate(mCallers, source);
        if (uri != null && mGrants != null && getContext().checkUriPermission(
                uri, Binder.getCallingPid(), Binder.getCallingUid(), uriModeFlag)
                == PackageManager.PERMISSION_GRANTED) {
            return PermissionChecker.PERMISSION_GRANTED;
        }
        // Clear incoming Binder identity so PermissionChecker evaluates mDataSource as SYSTEM_UID
        // at the head of the attribution chain while checking AppOps & renouncements on source.
        long identity = Binder.clearCallingIdentity();
        try {
            int result = PermissionChecker.checkPermissionForDataDeliveryFromDataSource(
                    getContext(), permission, -1, new AttributionSource(mDataSource, source), null);
            if (result == PermissionChecker.PERMISSION_HARD_DENIED) {
                throw new SecurityException("Contacts access requires " + permission);
            }
            return result;
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
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
                Locale dbLocale = Locale.getDefault();
                if (dbLocale == null || dbLocale.getLanguage().isEmpty()) {
                    dbLocale = Locale.US;
                }
                db.setLocale(dbLocale);
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
                + "display_name TEXT COLLATE LOCALIZED,"
                + "display_name_alt TEXT COLLATE LOCALIZED,"
                + "display_name_source INTEGER DEFAULT 0,"
                + "phonetic_name TEXT COLLATE LOCALIZED,"
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
                + "display_name TEXT COLLATE LOCALIZED,"
                + "display_name_alt TEXT COLLATE LOCALIZED,"
                + "display_name_source INTEGER DEFAULT 0,"
                + "phonetic_name TEXT COLLATE LOCALIZED,"
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
                + "should_sync INTEGER NOT NULL DEFAULT 1,"
                + "any_synced INTEGER NOT NULL DEFAULT 0,"
                + "ungrouped_count INTEGER NOT NULL DEFAULT 0,"
                + "ungrouped_with_phones INTEGER NOT NULL DEFAULT 0"
                + ");");
        db.execSQL("CREATE TABLE IF NOT EXISTS provider_status ("
                + "status INTEGER NOT NULL DEFAULT 0,"
                + "database_creation_timestamp INTEGER NOT NULL DEFAULT 0"
                + ");");
        db.execSQL("INSERT INTO provider_status (status, database_creation_timestamp) VALUES (0, ?);",
                new Object[] { System.currentTimeMillis() });
        createIndexesAndViews(db);
    }

    private static void onUpgradeDatabase(SQLiteDatabase db, int oldVersion, int newVersion) {
        throw new IllegalStateException("Unsupported contacts schema upgrade: "
                + oldVersion + " to " + newVersion);
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

        db.execSQL("CREATE VIEW IF NOT EXISTS view_entities AS SELECT "
                + "raw_contacts._id AS _id,"
                + "raw_contacts._id AS raw_contact_id,"
                + "raw_contacts.contact_id AS contact_id,"
                + "data._id AS data_id,"
                + "contacts.lookup AS lookup,"
                + "contacts.name_raw_contact_id AS name_raw_contact_id,"
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
                + "contacts.contact_status_res_package AS contact_status_res_package,"
                + "contacts.contact_status_label AS contact_status_label,"
                + "contacts.contact_status_icon AS contact_status_icon,"
                + "contacts.contact_last_updated_timestamp AS contact_last_updated_timestamp,"
                + "raw_contacts.account_name AS account_name,"
                + "raw_contacts.account_type AS account_type,"
                + "raw_contacts.data_set AS data_set,"
                + "raw_contacts.sourceid AS sourceid,"
                + "raw_contacts.version AS version,"
                + "raw_contacts.dirty AS dirty,"
                + "raw_contacts.deleted AS deleted,"
                + "raw_contacts.aggregation_mode AS aggregation_mode,"
                + "raw_contacts.sync1 AS sync1, raw_contacts.sync2 AS sync2,"
                + "raw_contacts.sync3 AS sync3, raw_contacts.sync4 AS sync4,"
                + "raw_contacts.raw_contact_is_user_profile AS raw_contact_is_user_profile,"
                + "contacts.is_user_profile AS is_user_profile "
                + "FROM raw_contacts "
                + "LEFT JOIN contacts ON raw_contacts.contact_id = contacts._id "
                + "LEFT JOIN data ON data.raw_contact_id = raw_contacts._id "
                + "LEFT JOIN mimetypes ON data.mimetype_id = mimetypes._id;");

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
                + "WHERE mimetypes.mimetype = '" + MIME_PHONE + "' "
                + "AND raw_contacts.deleted = 0 AND contacts.is_user_profile = 0;");
    }

    @Override
    public boolean onCreate() {
        Context context = getContext();
        if (context != null && context.getResources() != null) {
            mStrictPhoneComparison = context.getResources().getBoolean(
                    com.android.internal.R.bool.config_use_strict_phone_number_comparation);
            int minMatch = context.getResources().getInteger(
                    com.android.internal.R.integer.config_phonenumber_compare_min_match);
            if (minMatch > 0) mPhoneMinMatch = minMatch;
        }
        return true;
    }

    @Override
    public String getType(Uri uri) {
        switch (sUriMatcher.match(uri)) {
            case CONTACTS:
            case CONTACTS_FILTER_EMPTY:
            case CONTACTS_FILTER:
            case CONTACTS_STREQUENT:
            case CONTACTS_STREQUENT_FILTER_EMPTY:
            case CONTACTS_STREQUENT_FILTER:
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
            case CONTACTS_ENTITIES:
                return "vnd.android.cursor.dir/contact_entity";
            case RAW_CONTACTS_ENTITY:
            case RAW_CONTACT_ENTITIES:
            case PROFILE_RAW_CONTACT_ENTITIES:
                return ContactsContract.RawContactsEntity.CONTENT_TYPE;
            case CONTACTS_PHOTO:
                return ContactsContract.CommonDataKinds.Photo.CONTENT_ITEM_TYPE;
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
            case CALLABLES:
            case CALLABLES_FILTER_EMPTY:
            case CALLABLES_FILTER:
                return "vnd.android.cursor.dir/callable";
            case CALLABLES_ID:
                return "vnd.android.cursor.item/callable";
            case PHONE_LOOKUP:
            case PHONE_LOOKUP_ENTERPRISE:
                return ContactsContract.PhoneLookup.CONTENT_TYPE;
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
            case PROVIDER_STATUS:
                return ContactsContract.ProviderStatus.CONTENT_TYPE;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private static SQLiteQueryBuilder strictBuilder(String tables, Map<String, String> projectionMap) {
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables(tables);
        qb.setProjectionMap(projectionMap);
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        return qb;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        int match = sUriMatcher.match(uri);
        String limit = ContactsProviderSupport.parseLimit(uri);
        boolean syncAdapter = isCallerSyncAdapter(uri);
        SQLiteQueryBuilder qb;
        List<String> whereArgs = new ArrayList<>();
        String defaultSortOrder = null;

        switch (match) {
            case CONTACTS:
            case CONTACTS_FILTER_EMPTY:
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0");
                break;
            case CONTACTS_ID:
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND _id = " + ContentUris.parseId(uri));
                break;
            case CONTACTS_FILTER:
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND display_name LIKE ?");
                whereArgs.add("%" + uri.getLastPathSegment() + "%");
                break;
            case CONTACTS_LOOKUP:
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND lookup = ?");
                whereArgs.add(uri.getPathSegments().get(2));
                break;
            case CONTACTS_LOOKUP_ID: {
                List<String> segments = uri.getPathSegments();
                long id = parsePositivePathLong(segments.get(3), uri);
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND (_id = " + id + " OR lookup = ?)");
                whereArgs.add(segments.get(2));
                break;
            }
            case CONTACTS_STREQUENT:
            case CONTACTS_STREQUENT_FILTER_EMPTY:
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND (starred = 1 OR times_contacted > 0)");
                defaultSortOrder = "starred DESC, times_contacted DESC, sort_key ASC";
                break;
            case CONTACTS_STREQUENT_FILTER:
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone(
                        "is_user_profile = 0 AND (starred = 1 OR times_contacted > 0) AND display_name LIKE ?");
                whereArgs.add("%" + uri.getLastPathSegment() + "%");
                defaultSortOrder = "starred DESC, times_contacted DESC, sort_key ASC";
                break;
            case CONTACTS_DATA: {
                long contactId = parsePositivePathLong(uri.getPathSegments().get(1), uri);
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND contact_id = " + contactId);
                break;
            }
            case CONTACTS_ENTITIES: {
                long contactId = parsePositivePathLong(uri.getPathSegments().get(1), uri);
                qb = strictBuilder("view_entities", sEntitiesProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND contact_id = " + contactId);
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                defaultSortOrder = "_id ASC, data_id ASC";
                break;
            }
            case CONTACTS_PHOTO: {
                long contactId = parsePositivePathLong(uri.getPathSegments().get(1), uri);
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND contact_id = " + contactId
                        + " AND mimetype = ?");
                whereArgs.add(MIME_PHOTO);
                break;
            }
            case RAW_CONTACTS:
                qb = strictBuilder("view_raw_contacts", sRawContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0");
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                break;
            case RAW_CONTACTS_ID:
                qb = strictBuilder("view_raw_contacts", sRawContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND _id = " + ContentUris.parseId(uri));
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                break;
            case RAW_CONTACTS_DATA: {
                long rawId = parsePositivePathLong(uri.getPathSegments().get(1), uri);
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND raw_contact_id = " + rawId);
                break;
            }
            case RAW_CONTACTS_ENTITY: {
                long rawId = parsePositivePathLong(uri.getPathSegments().get(1), uri);
                qb = strictBuilder("view_entities", sEntitiesProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND _id = " + rawId);
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                defaultSortOrder = "_id ASC, data_id ASC";
                break;
            }
            case RAW_CONTACT_ENTITIES:
                qb = strictBuilder("view_entities", sEntitiesProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0");
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                defaultSortOrder = "_id ASC, data_id ASC";
                break;
            case PROFILE_RAW_CONTACT_ENTITIES:
                qb = strictBuilder("view_entities", sEntitiesProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 1");
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                defaultSortOrder = "_id ASC, data_id ASC";
                break;
            case DATA:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0");
                break;
            case DATA_ID:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND _id = " + ContentUris.parseId(uri));
                break;
            case PHONES:
            case PHONES_FILTER_EMPTY:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND mimetype = ?");
                whereArgs.add(MIME_PHONE);
                break;
            case PHONES_ID:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND mimetype = ? AND _id = "
                        + ContentUris.parseId(uri));
                whereArgs.add(MIME_PHONE);
                break;
            case PHONES_FILTER:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone(
                        "is_user_profile = 0 AND mimetype = ? AND (display_name LIKE ? OR data1 LIKE ? OR data4 LIKE ?)");
                whereArgs.add(MIME_PHONE);
                String phonePattern = "%" + uri.getLastPathSegment() + "%";
                whereArgs.add(phonePattern);
                whereArgs.add(phonePattern);
                whereArgs.add(phonePattern);
                break;
            case EMAILS:
            case EMAILS_FILTER_EMPTY:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND mimetype = ?");
                whereArgs.add(MIME_EMAIL);
                break;
            case EMAILS_ID:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND mimetype = ? AND _id = "
                        + ContentUris.parseId(uri));
                whereArgs.add(MIME_EMAIL);
                break;
            case EMAILS_FILTER:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone(
                        "is_user_profile = 0 AND mimetype = ? AND (display_name LIKE ? OR data1 LIKE ?)");
                whereArgs.add(MIME_EMAIL);
                String emailPattern = "%" + uri.getLastPathSegment() + "%";
                whereArgs.add(emailPattern);
                whereArgs.add(emailPattern);
                break;
            case POSTALS:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND mimetype = ?");
                whereArgs.add(MIME_POSTAL);
                break;
            case POSTALS_ID:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND mimetype = ? AND _id = "
                        + ContentUris.parseId(uri));
                whereArgs.add(MIME_POSTAL);
                break;
            case CALLABLES:
            case CALLABLES_FILTER_EMPTY:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 0 AND (mimetype = ? OR mimetype = ?)");
                whereArgs.add(MIME_PHONE);
                whereArgs.add(MIME_SIP);
                break;
            case CALLABLES_ID:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone(
                        "is_user_profile = 0 AND (mimetype = ? OR mimetype = ?) AND _id = "
                                + ContentUris.parseId(uri));
                whereArgs.add(MIME_PHONE);
                whereArgs.add(MIME_SIP);
                break;
            case CALLABLES_FILTER:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone(
                        "is_user_profile = 0 AND (mimetype = ? OR mimetype = ?) "
                                + "AND (display_name LIKE ? OR data1 LIKE ? OR data4 LIKE ?)");
                whereArgs.add(MIME_PHONE);
                whereArgs.add(MIME_SIP);
                String callablePattern = "%" + uri.getLastPathSegment() + "%";
                whereArgs.add(callablePattern);
                whereArgs.add(callablePattern);
                whereArgs.add(callablePattern);
                break;
            case PHONE_LOOKUP:
            case PHONE_LOOKUP_ENTERPRISE: {
                String phoneQuery = uri.getLastPathSegment();
                qb = strictBuilder("view_phone_lookup", sPhoneLookupProjectionMap);
                if (phoneQuery == null || phoneQuery.isEmpty()) {
                    qb.appendWhereStandalone("1 = 0");
                } else {
                    int strict = mStrictPhoneComparison ? 1 : 0;
                    qb.appendWhereStandalone("PHONE_NUMBERS_EQUAL(number, ?, "
                            + strict + ", " + mPhoneMinMatch + ") OR "
                            + "(normalized_number IS NOT NULL AND PHONE_NUMBERS_EQUAL(normalized_number, ?, "
                            + strict + ", " + mPhoneMinMatch + "))");
                    whereArgs.add(phoneQuery);
                    whereArgs.add(phoneQuery);
                }
                break;
            }
            case GROUPS:
            case GROUPS_SUMMARY:
                qb = strictBuilder("groups", sGroupsProjectionMap);
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                break;
            case GROUPS_ID:
                qb = strictBuilder("groups", sGroupsProjectionMap);
                qb.appendWhereStandalone("_id = " + ContentUris.parseId(uri));
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                break;
            case DELETED_CONTACTS:
                qb = strictBuilder("deleted_contacts", sDeletedContactsProjectionMap);
                break;
            case DIRECTORIES:
                qb = strictBuilder("directories", sDirectoriesProjectionMap);
                break;
            case DIRECTORIES_ID:
                qb = strictBuilder("directories", sDirectoriesProjectionMap);
                qb.appendWhereStandalone("_id = " + ContentUris.parseId(uri));
                break;
            case SETTINGS:
                qb = strictBuilder("settings", sSettingsProjectionMap);
                break;
            case PROVIDER_STATUS:
                qb = strictBuilder("provider_status", sProviderStatusProjectionMap);
                break;
            case PROFILE:
                qb = strictBuilder("view_contacts", sContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 1");
                break;
            case PROFILE_RAW_CONTACTS:
                qb = strictBuilder("view_raw_contacts", sRawContactsProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 1");
                if (!syncAdapter) qb.appendWhereStandalone("deleted = 0");
                break;
            case PROFILE_DATA:
                qb = strictBuilder("view_data", sDataProjectionMap);
                qb.appendWhereStandalone("is_user_profile = 1");
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }

        String[] effArgs = combineArgs(whereArgs, selectionArgs);
        String effSort = (sortOrder != null && !sortOrder.trim().isEmpty()) ? sortOrder : defaultSortOrder;
        Cursor cursor;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            validateSelectionSyntax(db, qb.getTables(), selection);
            cursor = qb.query(db, projection, selection, effArgs,
                    null, null, effSort, limit);
        }
        try {
            ContactsProviderSupport.setNotificationUri(cursor, getContext(), uri);
            return cursor;
        } catch (RuntimeException | Error failure) {
            cursor.close();
            throw failure;
        }
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
        // ContentProvider.Transport.applyBatch already enforces per-operation read/write
        // permissions and URI grants before dispatching here; do not require WRITE_CONTACTS
        // on the authority root (which breaks assert-only and URI-grant batches) or
        // double-note AppOps.
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
                validateColumns(copy, sWritableRawContactsColumns, "raw_contacts");
                boolean isProfile = (match == PROFILE_RAW_CONTACTS)
                        || asInt(copy, "raw_contact_is_user_profile") != 0;
                long contactId = copy.containsKey("contact_id") && copy.getAsLong("contact_id") != null
                        ? copy.getAsLong("contact_id") : -1L;
                if (contactId < 0 && isProfile) {
                    contactId = findExistingProfileContactId(db);
                }
                if (contactId < 0) {
                    ContentValues contactValues = new ContentValues();
                    contactValues.put("contact_last_updated_timestamp", now);
                    contactValues.put("is_user_profile", isProfile ? 1 : 0);
                    if (copy.containsKey("display_name")) {
                        String dn = copy.getAsString("display_name");
                        applyNameDerivedFields(contactValues, dn, dn, ContactsContract.DisplayNameSources.UNDEFINED);
                    }
                    contactId = db.insertOrThrow("contacts", null, contactValues);
                    copy.put("contact_id", contactId);
                } else if (!copy.containsKey("contact_id")) {
                    copy.put("contact_id", contactId);
                }
                if (copy.containsKey("display_name")) {
                    String dn = copy.getAsString("display_name");
                    applyNameDerivedFields(copy, dn, dn, ContactsContract.DisplayNameSources.UNDEFINED);
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
                validateColumns(copy, sWritableDataColumns, "data");
                long rawId = parsePositivePathLong(uri.getPathSegments().get(1), uri);
                copy.put("raw_contact_id", rawId);
                return insertDataLocked(db, copy, null, now, syncAdapter);
            }
            case DATA:
            case PHONES:
            case EMAILS:
            case POSTALS:
                validateColumns(copy, sWritableDataColumns, "data");
                return insertDataLocked(db, copy, forcedMime(match), now, syncAdapter);
            case GROUPS:
                validateColumns(copy, sWritableGroupsColumns, "groups");
                if (!syncAdapter && !copy.containsKey("dirty")) copy.put("dirty", 1);
                return db.insertOrThrow("groups", null, copy);
            case SETTINGS:
                validateColumns(copy, sWritableSettingsColumns, "settings");
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
            maybePopulateNormalizedPhone(copy, false);
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
            case CONTACTS_ID:
            case CONTACTS_LOOKUP:
            case CONTACTS_LOOKUP_ID:
            case PROFILE: {
                validateColumns(copy, sWritableContactsColumns, "contacts");
                List<Long> targetIds = resolveContactTargetIds(
                        db, match, uri, selection, selectionArgs);
                if (targetIds.isEmpty()) return 0;
                ContentValues rawMirror = new ContentValues(copy);
                copy.put("contact_last_updated_timestamp", now);
                int updated = updateByIds(db, "contacts", "_id", copy, targetIds);
                if (updated > 0 && rawMirror.size() > 0) {
                    updateByIds(db, "raw_contacts", "contact_id", rawMirror, targetIds);
                    if (!syncAdapter) {
                        Set<Long> rawIds = queryIdsForColumn(db, "raw_contacts", "_id",
                                "contact_id", targetIds, null);
                        bumpRawContactsVersionAndDirty(db, rawIds);
                    }
                }
                return updated;
            }
            case RAW_CONTACTS:
            case RAW_CONTACTS_ID:
            case PROFILE_RAW_CONTACTS: {
                validateColumns(copy, sWritableRawContactsColumns, "raw_contacts");
                List<String> whereArgs = new ArrayList<>();
                StringBuilder extra = new StringBuilder();
                extra.append(match == PROFILE_RAW_CONTACTS ? "is_user_profile = 1" : "is_user_profile = 0");
                if (match == RAW_CONTACTS_ID) {
                    extra.append(" AND _id = ").append(ContentUris.parseId(uri));
                }
                if (!syncAdapter) {
                    extra.append(" AND deleted = 0");
                }
                List<Long> rawIdList = resolveTargetIds(db, "view_raw_contacts",
                        sRawContactsProjectionMap, extra.toString(), whereArgs,
                        selection, selectionArgs);
                if (rawIdList.isEmpty()) return 0;
                if (copy.containsKey("display_name")) {
                    String dn = copy.getAsString("display_name");
                    applyNameDerivedFields(copy, dn, dn, ContactsContract.DisplayNameSources.UNDEFINED);
                }
                int updated = updateByIds(db, "raw_contacts", "_id", copy, rawIdList);
                if (updated > 0) {
                    Set<Long> rawIds = new LinkedHashSet<>(rawIdList);
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
            case RAW_CONTACTS_DATA:
            case PHONES:
            case PHONES_ID:
            case EMAILS:
            case EMAILS_ID:
            case POSTALS:
            case POSTALS_ID: {
                validateColumns(copy, sWritableDataColumns, "data");
                List<Long> targetDataIds = resolveDataTargetIds(
                        db, match, uri, selection, selectionArgs);
                if (targetDataIds.isEmpty()) return 0;
                String newMime = null;
                if (copy.containsKey("mimetype")) {
                    newMime = copy.getAsString("mimetype");
                    copy.remove("mimetype");
                    if (newMime == null || newMime.isEmpty()) {
                        throw new IllegalArgumentException("Data mimetype cannot be empty");
                    }
                    copy.put("mimetype_id", resolveOrCreateMimetypeId(db, newMime));
                }
                Set<Long> rawIds = queryIdsForColumn(db, "data", "raw_contact_id",
                        "_id", targetDataIds, null);
                int updated = updateDataRowsByIds(db, copy, targetDataIds, newMime);
                if (updated > 0) {
                    refreshRawAndParentContacts(db, rawIds, now, !syncAdapter);
                }
                return updated;
            }
            case DATA_USAGEFEEDBACK:
            case DATA_USAGEFEEDBACK_ID: {
                validateColumns(copy, sWritableUsageFeedbackColumns, "usagefeedback");
                List<String> whereArgs = new ArrayList<>();
                StringBuilder extra = new StringBuilder();
                if (match == DATA_USAGEFEEDBACK_ID) {
                    List<Long> requestedIds = parseCommaSeparatedIds(uri.getLastPathSegment(), uri);
                    if (requestedIds.isEmpty()) return 0;
                    extra.append(idInClause("_id", requestedIds));
                }
                List<Long> targetDataIds = resolveTargetIds(db, "view_data", sDataProjectionMap,
                        extra.toString(), whereArgs, selection, selectionArgs);
                if (targetDataIds.isEmpty()) return 0;
                Set<Long> rawIds = queryIdsForColumn(db, "data", "raw_contact_id",
                        "_id", targetDataIds, null);
                Set<Long> contactIds = queryIdsForColumn(db, "raw_contacts", "contact_id",
                        "_id", new ArrayList<>(rawIds), null);
                long usedAt = copy.containsKey("last_time_used") && copy.getAsLong("last_time_used") != null
                        ? copy.getAsLong("last_time_used") : now;
                incrementUsageStats(db, "raw_contacts", new ArrayList<>(rawIds), usedAt);
                incrementUsageStats(db, "contacts", new ArrayList<>(contactIds), usedAt);
                return targetDataIds.size();
            }
            case GROUPS:
            case GROUPS_ID: {
                validateColumns(copy, sWritableGroupsColumns, "groups");
                StringBuilder extra = new StringBuilder();
                if (match == GROUPS_ID) {
                    extra.append("_id = ").append(ContentUris.parseId(uri));
                }
                if (!syncAdapter) {
                    if (extra.length() > 0) extra.append(" AND ");
                    extra.append("deleted = 0");
                }
                List<Long> groupIds = resolveTargetIds(db, "groups", sGroupsProjectionMap,
                        extra.toString(), null, selection, selectionArgs);
                if (groupIds.isEmpty()) return 0;
                if (!syncAdapter && !copy.containsKey("dirty")) {
                    copy.put("dirty", 1);
                }
                return updateByIds(db, "groups", "_id", copy, groupIds);
            }
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private int updateDataRowsByIds(SQLiteDatabase db, ContentValues copy, List<Long> targetIds,
            String explicitNewMime) {
        boolean needsPhoneNorm = copy.containsKey("data1") && !copy.containsKey("data4");
        if (!needsPhoneNorm || explicitNewMime != null) {
            ContentValues effective = new ContentValues(copy);
            if (needsPhoneNorm && MIME_PHONE.equals(explicitNewMime)) {
                maybePopulateNormalizedPhone(effective, true);
            }
            return updateByIds(db, "data", "_id", effective, targetIds);
        }
        List<Long> phoneIds = new ArrayList<>();
        List<Long> otherIds = new ArrayList<>();
        forEachIdChunk(targetIds, chunk -> {
            String sql = "SELECT data._id, mimetypes.mimetype FROM data "
                    + "JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                    + "WHERE " + idInClause("data._id", chunk);
            try (Cursor c = db.rawQuery(sql, null)) {
                while (c.moveToNext()) {
                    long id = c.getLong(0);
                    if (MIME_PHONE.equals(c.getString(1))) phoneIds.add(id);
                    else otherIds.add(id);
                }
            }
        });
        int total = 0;
        if (!phoneIds.isEmpty()) {
            ContentValues phoneCopy = new ContentValues(copy);
            maybePopulateNormalizedPhone(phoneCopy, true);
            total += updateByIds(db, "data", "_id", phoneCopy, phoneIds);
        }
        if (!otherIds.isEmpty()) {
            total += updateByIds(db, "data", "_id", copy, otherIds);
        }
        return total;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
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
            case CONTACTS_ID:
            case CONTACTS_LOOKUP:
            case CONTACTS_LOOKUP_ID: {
                List<Long> contactIds = resolveContactTargetIds(
                        db, match, uri, selection, selectionArgs);
                if (contactIds.isEmpty()) return 0;
                List<Long> rawIdList = new ArrayList<>(queryIdsForColumn(
                        db, "raw_contacts", "_id", "contact_id", contactIds, null));
                deleteRawContactsPartitioned(db, rawIdList, syncAdapter, now);
                for (long cid : contactIds) {
                    if (!hasRemainingRows(db, "raw_contacts", "contact_id = ?", cid)) {
                        db.delete("contacts", "_id = ?", new String[] { Long.toString(cid) });
                        recordDeletedContact(db, cid, now);
                    } else if (!hasRemainingRows(db, "raw_contacts", "contact_id = ? AND deleted = 0", cid)) {
                        recordDeletedContact(db, cid, now);
                    }
                }
                return contactIds.size();
            }
            case CONTACTS_DELETE_USAGE: {
                List<Long> contactIds = resolveTargetIds(db, "view_contacts",
                        sContactsProjectionMap, "is_user_profile = 0", null,
                        selection, selectionArgs);
                if (contactIds.isEmpty()) return 0;
                ContentValues reset = new ContentValues();
                reset.put("times_used", 0);
                reset.putNull("last_time_used");
                int updated = updateByIds(db, "contacts", "_id", reset, contactIds);
                updateByIds(db, "raw_contacts", "contact_id", reset, contactIds);
                return updated;
            }
            case RAW_CONTACTS:
            case RAW_CONTACTS_ID:
            case PROFILE_RAW_CONTACTS: {
                StringBuilder extra = new StringBuilder();
                extra.append(match == PROFILE_RAW_CONTACTS ? "is_user_profile = 1" : "is_user_profile = 0");
                if (match == RAW_CONTACTS_ID) {
                    extra.append(" AND _id = ").append(ContentUris.parseId(uri));
                }
                if (!syncAdapter) {
                    extra.append(" AND deleted = 0");
                }
                List<Long> rawIdList = resolveTargetIds(db, "view_raw_contacts",
                        sRawContactsProjectionMap, extra.toString(), null,
                        selection, selectionArgs);
                if (rawIdList.isEmpty()) return 0;
                Set<Long> contactIds = queryIdsForColumn(db, "raw_contacts", "contact_id",
                        "_id", rawIdList, null);
                int count = deleteRawContactsPartitioned(db, rawIdList, syncAdapter, now);
                Set<Long> survivingContacts = new HashSet<>();
                for (long cid : contactIds) {
                    if (!hasRemainingRows(db, "raw_contacts", "contact_id = ?", cid)) {
                        db.delete("contacts", "_id = ?", new String[] { Long.toString(cid) });
                        recordDeletedContact(db, cid, now);
                    } else if (!hasRemainingRows(db, "raw_contacts", "contact_id = ? AND deleted = 0", cid)) {
                        recordDeletedContact(db, cid, now);
                    } else {
                        survivingContacts.add(cid);
                    }
                }
                if (!survivingContacts.isEmpty()) {
                    Set<Long> survivingRawIds = queryIdsForColumn(db, "raw_contacts", "_id",
                            "contact_id", new ArrayList<>(survivingContacts), "deleted = 0");
                    refreshRawAndParentContacts(db, survivingRawIds, now, false);
                }
                return count;
            }
            case DATA:
            case DATA_ID:
            case RAW_CONTACTS_DATA:
            case PHONES:
            case PHONES_ID:
            case EMAILS:
            case EMAILS_ID:
            case POSTALS:
            case POSTALS_ID: {
                List<Long> targetDataIds = resolveDataTargetIds(
                        db, match, uri, selection, selectionArgs);
                if (targetDataIds.isEmpty()) return 0;
                Set<Long> rawIds = queryIdsForColumn(db, "data", "raw_contact_id",
                        "_id", targetDataIds, null);
                int deleted = deleteByIds(db, "data", "_id", targetDataIds);
                if (deleted > 0) {
                    refreshRawAndParentContacts(db, rawIds, now, !syncAdapter);
                }
                return deleted;
            }
            case GROUPS:
            case GROUPS_ID: {
                StringBuilder extra = new StringBuilder();
                if (match == GROUPS_ID) {
                    extra.append("_id = ").append(ContentUris.parseId(uri));
                }
                if (!syncAdapter) {
                    if (extra.length() > 0) extra.append(" AND ");
                    extra.append("deleted = 0");
                }
                List<Long> groupIds = resolveTargetIds(db, "groups", sGroupsProjectionMap,
                        extra.toString(), null, selection, selectionArgs);
                if (groupIds.isEmpty()) return 0;
                return deleteGroupsPartitioned(db, groupIds, syncAdapter);
            }
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private int deleteRawContactsPartitioned(SQLiteDatabase db, List<Long> rawIds,
            boolean syncAdapter, long now) {
        if (rawIds.isEmpty()) return 0;
        if (syncAdapter) {
            return deleteByIds(db, "raw_contacts", "_id", rawIds);
        }
        List<Long> hardDeleteIds = new ArrayList<>();
        List<Long> softDeleteIds = new ArrayList<>();
        forEachIdChunk(rawIds, chunk -> {
            String sql = "SELECT _id, account_type FROM raw_contacts WHERE " + idInClause("_id", chunk);
            try (Cursor c = db.rawQuery(sql, null)) {
                while (c.moveToNext()) {
                    long id = c.getLong(0);
                    String accountType = c.getString(1);
                    if (accountType == null || accountType.isEmpty()) {
                        hardDeleteIds.add(id);
                    } else {
                        softDeleteIds.add(id);
                    }
                }
            }
        });
        int total = 0;
        if (!hardDeleteIds.isEmpty()) {
            total += deleteByIds(db, "raw_contacts", "_id", hardDeleteIds);
        }
        if (!softDeleteIds.isEmpty()) {
            total += softDeleteByIds(db, "raw_contacts", softDeleteIds);
        }
        return total;
    }

    private static int deleteGroupsPartitioned(SQLiteDatabase db, List<Long> groupIds,
            boolean syncAdapter) {
        if (groupIds.isEmpty()) return 0;
        if (syncAdapter) {
            return deleteByIds(db, "groups", "_id", groupIds);
        }
        List<Long> hardDeleteIds = new ArrayList<>();
        List<Long> softDeleteIds = new ArrayList<>();
        forEachIdChunk(groupIds, chunk -> {
            String sql = "SELECT _id, account_type FROM groups WHERE " + idInClause("_id", chunk);
            try (Cursor c = db.rawQuery(sql, null)) {
                while (c.moveToNext()) {
                    long id = c.getLong(0);
                    String accountType = c.getString(1);
                    if (accountType == null || accountType.isEmpty()) {
                        hardDeleteIds.add(id);
                    } else {
                        softDeleteIds.add(id);
                    }
                }
            }
        });
        int total = 0;
        if (!hardDeleteIds.isEmpty()) {
            total += deleteByIds(db, "groups", "_id", hardDeleteIds);
        }
        if (!softDeleteIds.isEmpty()) {
            total += softDeleteByIds(db, "groups", softDeleteIds);
        }
        return total;
    }

    private static int softDeleteByIds(SQLiteDatabase db, String table, List<Long> ids) {
        final int[] count = { 0 };
        forEachIdChunk(ids, chunk -> {
            db.execSQL("UPDATE " + table + " SET deleted = 1, dirty = 1, version = version + 1 "
                    + "WHERE deleted = 0 AND " + idInClause("_id", chunk));
            count[0] += changesCount(db);
        });
        return count[0];
    }

    private static List<Long> resolveContactTargetIds(SQLiteDatabase db, int match, Uri uri,
            String selection, String[] selectionArgs) {
        List<String> whereArgs = new ArrayList<>();
        String extra;
        switch (match) {
            case CONTACTS:
                extra = "is_user_profile = 0";
                break;
            case CONTACTS_ID:
                extra = "is_user_profile = 0 AND _id = " + ContentUris.parseId(uri);
                break;
            case CONTACTS_LOOKUP:
                extra = "is_user_profile = 0 AND lookup = ?";
                whereArgs.add(uri.getPathSegments().get(2));
                break;
            case CONTACTS_LOOKUP_ID: {
                List<String> segments = uri.getPathSegments();
                long id = parsePositivePathLong(segments.get(3), uri);
                extra = "is_user_profile = 0 AND (_id = " + id + " OR lookup = ?)";
                whereArgs.add(segments.get(2));
                break;
            }
            case PROFILE:
                extra = "is_user_profile = 1";
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
        return resolveTargetIds(db, "view_contacts", sContactsProjectionMap, extra, whereArgs,
                selection, selectionArgs);
    }

    private static List<Long> resolveDataTargetIds(SQLiteDatabase db, int match, Uri uri,
            String selection, String[] selectionArgs) {
        List<String> whereArgs = new ArrayList<>();
        StringBuilder extra = new StringBuilder();
        String forcedMime = forcedMime(match);
        if (forcedMime != null) {
            extra.append("mimetype = ?");
            whereArgs.add(forcedMime);
        }
        if (match == RAW_CONTACTS_DATA) {
            if (extra.length() > 0) extra.append(" AND ");
            extra.append("raw_contact_id = ").append(parsePositivePathLong(uri.getPathSegments().get(1), uri));
        }
        if (match == DATA_ID || match == PHONES_ID || match == EMAILS_ID || match == POSTALS_ID) {
            if (extra.length() > 0) extra.append(" AND ");
            extra.append("_id = ").append(ContentUris.parseId(uri));
        }
        return resolveTargetIds(db, "view_data", sDataProjectionMap,
                extra.toString(), whereArgs, selection, selectionArgs);
    }

    private static void validateSelectionSyntax(SQLiteDatabase db, String viewOrTable,
            String selection) {
        if (selection == null || selection.trim().isEmpty()) return;
        try {
            // Validate caller selection standalone before computeWhere wraps it in (...),
            // so unbalanced parens like "1) OR (1" are rejected cleanly.
            db.validateSql("SELECT 1 FROM " + viewOrTable + " WHERE " + selection, null);
        } catch (android.database.sqlite.SQLiteException badSql) {
            throw new IllegalArgumentException("Invalid selection syntax", badSql);
        }
    }

    private static List<Long> resolveTargetIds(SQLiteDatabase db, String viewOrTable,
            Map<String, String> projectionMap, String extraWhere, List<String> extraArgs,
            String selection, String[] selectionArgs) {
        validateSelectionSyntax(db, viewOrTable, selection);
        SQLiteQueryBuilder qb = strictBuilder(viewOrTable, projectionMap);
        if (extraWhere != null && !extraWhere.isEmpty()) {
            qb.appendWhereStandalone(extraWhere);
        }
        String[] effArgs = combineArgs(extraArgs, selectionArgs);
        List<Long> ids = new ArrayList<>();
        try (Cursor c = qb.query(db, new String[] { "_id" }, selection, effArgs,
                null, null, "_id ASC", null)) {
            while (c.moveToNext()) {
                if (!c.isNull(0)) ids.add(c.getLong(0));
            }
        }
        return ids;
    }

    private static int updateByIds(SQLiteDatabase db, String table, String keyColumn,
            ContentValues values, List<Long> ids) {
        final int[] count = { 0 };
        forEachIdChunk(ids, chunk ->
                count[0] += db.update(table, values, idInClause(keyColumn, chunk), null));
        return count[0];
    }

    private static int deleteByIds(SQLiteDatabase db, String table, String keyColumn,
            List<Long> ids) {
        final int[] count = { 0 };
        forEachIdChunk(ids, chunk ->
                count[0] += db.delete(table, idInClause(keyColumn, chunk), null));
        return count[0];
    }

    private static Set<Long> queryIdsForColumn(SQLiteDatabase db, String table,
            String selectColumn, String whereColumn, List<Long> whereIds, String extraAndClause) {
        Set<Long> out = new LinkedHashSet<>();
        forEachIdChunk(whereIds, chunk -> {
            String sql = "SELECT DISTINCT " + selectColumn + " FROM " + table
                    + " WHERE " + idInClause(whereColumn, chunk)
                    + (extraAndClause != null ? " AND (" + extraAndClause + ")" : "");
            try (Cursor c = db.rawQuery(sql, null)) {
                while (c.moveToNext()) {
                    if (!c.isNull(0)) out.add(c.getLong(0));
                }
            }
        });
        return out;
    }

    private static void incrementUsageStats(SQLiteDatabase db, String table,
            List<Long> ids, long usedAt) {
        if (ids.isEmpty()) return;
        forEachIdChunk(ids, chunk ->
                db.execSQL("UPDATE " + table + " SET times_used = times_used + 1, "
                        + "last_time_used = ? WHERE " + idInClause("_id", chunk),
                        new Object[] { usedAt }));
    }

    private interface IdChunkConsumer {
        void accept(List<Long> chunk);
    }

    private static void forEachIdChunk(List<Long> ids, IdChunkConsumer consumer) {
        if (ids == null || ids.isEmpty()) return;
        for (int start = 0; start < ids.size(); start += ID_CHUNK_SIZE) {
            int end = Math.min(start + ID_CHUNK_SIZE, ids.size());
            consumer.accept(ids.subList(start, end));
        }
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

    private static String[] combineArgs(List<String> whereArgs, String[] selectionArgs) {
        int extraCount = whereArgs == null ? 0 : whereArgs.size();
        int selCount = selectionArgs == null ? 0 : selectionArgs.length;
        if (extraCount == 0 && selCount == 0) return null;
        if (extraCount == 0) return selectionArgs;
        String[] combined = new String[extraCount + selCount];
        for (int i = 0; i < extraCount; i++) combined[i] = whereArgs.get(i);
        if (selCount > 0) System.arraycopy(selectionArgs, 0, combined, extraCount, selCount);
        return combined;
    }

    private static void validateColumns(ContentValues values, Set<String> allowedColumns,
            String target) {
        if (values == null) return;
        for (String col : values.keySet()) {
            if (!allowedColumns.contains(col)) {
                throw new IllegalArgumentException("Unknown or read-only " + target + " column: " + col);
            }
        }
    }

    private static long parsePositivePathLong(String segment, Uri uri) {
        try {
            return Long.parseLong(segment);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("Invalid numeric ID in URL " + uri, e);
        }
    }

    private static List<Long> parseCommaSeparatedIds(String segment, Uri uri) {
        if (segment == null || segment.trim().isEmpty()) {
            throw new IllegalArgumentException("Usage feedback requires data IDs: " + uri);
        }
        String[] parts = segment.split(",");
        List<Long> ids = new ArrayList<>(parts.length);
        for (String part : parts) {
            ids.add(parsePositivePathLong(part.trim(), uri));
        }
        return ids;
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

    private static long findExistingProfileContactId(SQLiteDatabase db) {
        try (Cursor c = db.rawQuery(
                "SELECT _id FROM contacts WHERE is_user_profile = 1 ORDER BY _id ASC LIMIT 1", null)) {
            return c.moveToFirst() ? c.getLong(0) : -1L;
        }
    }

    private static void bumpRawContactsVersionAndDirty(SQLiteDatabase db, Set<Long> rawIds) {
        if (rawIds == null || rawIds.isEmpty()) return;
        forEachIdChunk(new ArrayList<>(rawIds), chunk ->
                db.execSQL("UPDATE raw_contacts SET dirty = 1, version = version + 1 WHERE "
                        + idInClause("_id", chunk)));
    }

    private static void maybePopulateNormalizedPhone(ContentValues copy, boolean clearWhenUnknown) {
        if (!copy.containsKey("data4") || copy.getAsString("data4") == null) {
            String raw = copy.containsKey("data1") ? copy.getAsString("data1") : null;
            String e164 = formatPhoneE164(raw);
            if (e164 != null) {
                copy.put("data4", e164);
            } else if (clearWhenUnknown) {
                copy.putNull("data4");
            }
        }
    }

    private static String formatPhoneE164(String raw) {
        if (raw == null || raw.trim().isEmpty()) return null;
        Locale locale = Locale.getDefault();
        String countryIso = (locale == null || locale.getCountry().isEmpty())
                ? null : locale.getCountry();
        return PhoneNumberUtils.formatNumberToE164(raw, countryIso);
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
            long photoId = 0L;
            Long photoFileId = null;
            try (Cursor c = db.rawQuery(
                    "SELECT data._id, data.data14 FROM data "
                            + "JOIN mimetypes ON data.mimetype_id = mimetypes._id "
                            + "JOIN raw_contacts ON data.raw_contact_id = raw_contacts._id "
                            + "WHERE raw_contacts.contact_id = ? AND raw_contacts.deleted = 0 "
                            + "AND mimetypes.mimetype = ? "
                            + "ORDER BY data.is_super_primary DESC, data.is_primary DESC, data._id DESC LIMIT 1",
                    new String[] { Long.toString(contactId), MIME_PHOTO })) {
                if (c.moveToFirst()) {
                    photoId = c.getLong(0);
                    if (!c.isNull(1)) photoFileId = c.getLong(1);
                }
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
            update.put("photo_id", photoId);
            if (photoFileId != null) {
                update.put("photo_file_id", photoFileId);
            } else {
                update.putNull("photo_file_id");
            }
            if (photoId > 0) {
                String photoUri = Uri.withAppendedPath(
                        ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, contactId),
                        ContactsContract.Contacts.Photo.CONTENT_DIRECTORY).toString();
                update.put("photo_uri", photoUri);
                update.put("photo_thumb_uri", photoUri);
            } else {
                update.putNull("photo_uri");
                update.putNull("photo_thumb_uri");
            }
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
     * Resolves display name following AOSP precedence ({@code DisplayNameSources}):
     * {@code StructuredName} (40) -&gt; {@code Nickname} (35) -&gt; {@code Organization} (30)
     * -&gt; {@code Phone} (20) -&gt; {@code Email} (10).
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

    private static synchronized AlphabeticIndex.ImmutableIndex<Void> phonebookIndex() {
        Locale primary = Locale.getDefault();
        if (primary == null || primary.getLanguage().isEmpty()) primary = Locale.US;
        if (sCachedIndex != null && primary.equals(sCachedIndexLocale)) {
            return sCachedIndex;
        }
        AlphabeticIndex<Void> index = new AlphabeticIndex<Void>(primary)
                .setMaxLabelCount(300)
                .addLabels(Locale.ENGLISH)
                .addLabels(Locale.forLanguageTag("he"));
        sCachedIndex = index.buildImmutableIndex();
        sCachedIndexLocale = primary;
        return sCachedIndex;
    }

    private static void applyNameDerivedFields(ContentValues target, String displayName,
            String displayNameAlt, int source) {
        target.put("display_name", displayName);
        target.put("display_name_alt", displayNameAlt);
        target.put("display_name_source", source);
        target.put("sort_key", displayName);
        target.put("sort_key_alt", displayNameAlt);
        applyPhonebookLabelAndBucket(target, "phonebook_label", "phonebook_bucket", displayName);
        applyPhonebookLabelAndBucket(target, "phonebook_label_alt", "phonebook_bucket_alt",
                displayNameAlt);
    }

    private static void applyPhonebookLabelAndBucket(ContentValues target,
            String labelCol, String bucketCol, String name) {
        if (name == null || name.isEmpty()) {
            target.put(labelCol, "");
            target.put(bucketCol, 0);
            return;
        }
        AlphabeticIndex.ImmutableIndex<Void> index = phonebookIndex();
        int bucketIdx = index.getBucketIndex(name);
        if (bucketIdx < 0 || bucketIdx >= index.getBucketCount()) {
            target.put(labelCol, "#");
            target.put(bucketCol, index.getBucketCount());
            return;
        }
        AlphabeticIndex.Bucket<Void> bucket = index.getBucket(bucketIdx);
        if (bucket != null && bucket.getLabelType() == AlphabeticIndex.Bucket.LabelType.NORMAL
                && bucket.getLabel() != null && !bucket.getLabel().isEmpty()) {
            target.put(labelCol, bucket.getLabel());
            target.put(bucketCol, bucketIdx);
        } else {
            target.put(labelCol, "#");
            target.put(bucketCol, bucketIdx);
        }
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
