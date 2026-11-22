/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.AttributionSource;
import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Intent;
import android.content.UriMatcher;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.net.Uri;
import android.provider.CallLog;

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
 * Built-in SQLite-backed {@code content://call_log} provider for
 * {@link CallLog.Calls}. Starts empty on a clean device and enforces
 * {@code READ_CALL_LOG} / {@code WRITE_CALL_LOG}, AppOps, and URI grants.
 */
final class CallLogProvider extends ContentProvider {
    private static final int CALLS = 1;
    private static final int CALLS_ID = 2;
    private static final int CALLS_FILTER = 3;

    private static final int DB_VERSION = 2;
    private static final Uri NOTIFY_URI = Uri.parse("content://call_log/calls");

    private static final UriMatcher sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);
    private static final Map<String, String> sCallsProjectionMap;

    static {
        sUriMatcher.addURI(ContactsProviderSupport.CALL_LOG_AUTHORITY, "calls", CALLS);
        sUriMatcher.addURI(ContactsProviderSupport.CALL_LOG_AUTHORITY, "calls/#", CALLS_ID);
        sUriMatcher.addURI(ContactsProviderSupport.CALL_LOG_AUTHORITY, "calls/filter/*", CALLS_FILTER);

        Map<String, String> map = new HashMap<>();
        for (String col : new String[] {
                "_id", "number", "presentation", "date", "duration", "data_usage",
                "type", "features", "new", "name", "numbertype", "numberlabel",
                "countryiso", "voicemail_uri", "is_read", "geocoded_location",
                "lookup_uri", "matched_number", "normalized_number", "photo_id",
                "photo_uri", "formatted_number", "subscription_component_name",
                "subscription_id", "phone_account_address", "post_dial_digits",
                "via_number", "call_screening_component_name",
                "call_screening_app_name", "block_reason", "last_modified",
                "missed_reason", "subject", "location", "priority",
                "composer_photo_uri", "is_business_call", "asserted_display_name",
                "transcription"
        }) {
            map.put(col, col);
        }
        sCallsProjectionMap = Collections.unmodifiableMap(map);
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

    CallLogProvider(ContentProviders providers, PackageManagerService pm,
            ActivityTaskManagerService atm, UriGrants grants) {
        this(providers, pm, atm, grants,
                new File(ContactsProviderSupport.dataDirectory(), "databases/calllog.db"));
    }

    CallLogProvider(PackageManagerService pm, ActivityTaskManagerService atm, File databaseFile) {
        this(null, pm, atm, null, databaseFile);
    }

    CallLogProvider(ContentProviders providers, PackageManagerService pm,
            ActivityTaskManagerService atm, UriGrants grants, File databaseFile) {
        mPm = pm;
        mCallers = pm == null ? null : new CallerIdentity(pm, atm);
        mGrants = grants;
        mDatabase = databaseFile.getAbsoluteFile();
        if (providers != null) {
            mRegistration = new ContentProviders.RegisteredProvider(
                    ContactsProviderSupport.callLogProviderInfo(), getIContentProvider());
            providers.registerProvider(mRegistration);
        } else {
            mRegistration = null;
        }
    }

    ProviderInfo info() {
        return ContactsProviderSupport.callLogProviderInfo();
    }

    synchronized ContentProviderHolder holder() {
        if (mInitializationFailure != null) {
            throw new IllegalStateException("CallLog provider initialization failed",
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
                ContactsProviderSupport.READ_CALL_LOG, source);
    }

    @Override
    protected int enforceWritePermissionInner(Uri uri, AttributionSource source) {
        return ContactsProviderSupport.enforcePermissionInner(
                mPm, mCallers, mGrants, uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
                ContactsProviderSupport.WRITE_CALL_LOG, source);
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
                throw new IllegalStateException("Failed to create CallLog database directory: " + parent);
            }
            SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(mDatabase, null);
            try {
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
        db.execSQL("CREATE TABLE IF NOT EXISTS calls ("
                + "_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "number TEXT,"
                + "presentation INTEGER NOT NULL DEFAULT 1,"
                + "date INTEGER,"
                + "duration INTEGER,"
                + "data_usage INTEGER,"
                + "type INTEGER,"
                + "features INTEGER NOT NULL DEFAULT 0,"
                + "new INTEGER DEFAULT 1,"
                + "name TEXT,"
                + "numbertype INTEGER,"
                + "numberlabel TEXT,"
                + "countryiso TEXT,"
                + "voicemail_uri TEXT,"
                + "is_read INTEGER DEFAULT 0,"
                + "geocoded_location TEXT,"
                + "lookup_uri TEXT,"
                + "matched_number TEXT,"
                + "normalized_number TEXT,"
                + "photo_id INTEGER NOT NULL DEFAULT 0,"
                + "photo_uri TEXT,"
                + "formatted_number TEXT,"
                + "subscription_component_name TEXT,"
                + "subscription_id TEXT,"
                + "phone_account_address TEXT,"
                + "post_dial_digits TEXT NOT NULL DEFAULT '',"
                + "via_number TEXT NOT NULL DEFAULT '',"
                + "call_screening_component_name TEXT,"
                + "call_screening_app_name TEXT,"
                + "block_reason INTEGER NOT NULL DEFAULT 0,"
                + "last_modified INTEGER DEFAULT 0,"
                + "missed_reason INTEGER NOT NULL DEFAULT 0,"
                + "subject TEXT,"
                + "location TEXT,"
                + "priority INTEGER NOT NULL DEFAULT 0,"
                + "composer_photo_uri TEXT,"
                + "is_business_call INTEGER NOT NULL DEFAULT 0,"
                + "asserted_display_name TEXT,"
                + "transcription TEXT"
                + ");");
        db.execSQL("CREATE INDEX IF NOT EXISTS calls_date_idx ON calls(date DESC);");
    }

    private static void onUpgradeDatabase(SQLiteDatabase db, int oldVersion, int newVersion) {
        if (oldVersion < 2) {
            Set<String> cols = existingColumns(db, "calls");
            if (!cols.contains("transcription")) {
                db.execSQL("ALTER TABLE calls ADD COLUMN transcription TEXT;");
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

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        switch (sUriMatcher.match(uri)) {
            case CALLS:
            case CALLS_FILTER:
                return "vnd.android.cursor.dir/calls";
            case CALLS_ID:
                return "vnd.android.cursor.item/calls";
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION, ContactsProviderSupport.READ_CALL_LOG);
        int match = sUriMatcher.match(uri);
        String effectiveSelection = selection;
        String[] effectiveArgs = selectionArgs;
        String filterQuery = null;
        switch (match) {
            case CALLS:
                break;
            case CALLS_ID:
                effectiveSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effectiveArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            case CALLS_FILTER:
                filterQuery = uri.getPathSegments().get(2);
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
        String limit = ContactsProviderSupport.parseLimit(uri);
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables("calls");
        qb.setProjectionMap(sCallsProjectionMap);
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        String order = sortOrder != null ? sortOrder : "date DESC";
        Cursor cursor;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            if (filterQuery != null) {
                List<Long> matchedIds = findMatchingCallIds(db, filterQuery);
                String idClause = idInClause(matchedIds);
                effectiveSelection = ContactsProviderSupport.combineSelection(selection, idClause);
            }
            cursor = qb.query(db, projection, effectiveSelection, effectiveArgs,
                    null, null, order, limit);
        }
        ContactsProviderSupport.setNotificationUri(cursor, getContext(), uri);
        return cursor;
    }

    private static List<Long> findMatchingCallIds(SQLiteDatabase db, String query) {
        List<Long> ids = new ArrayList<>();
        if (query == null || query.isEmpty()) return ids;
        boolean hasLetters = containsLetters(query);
        String normalizedQuery = ContactsProviderSupport.normalizePhoneNumber(query);
        String lowerQuery = query.toLowerCase(Locale.ROOT);
        try (Cursor c = db.rawQuery(
                "SELECT _id, number, normalized_number, name FROM calls", null)) {
            while (c.moveToNext()) {
                long id = c.getLong(0);
                String number = c.getString(1);
                String normalized = c.getString(2);
                String name = c.getString(3);
                boolean matched = false;
                if (!hasLetters && normalizedQuery != null) {
                    matched = ContactsProviderSupport.phoneNumbersEqualLoose(number, query)
                            || ContactsProviderSupport.phoneNumbersEqualLoose(normalized, query);
                } else if (name != null && name.toLowerCase(Locale.ROOT).contains(lowerQuery)) {
                    matched = true;
                }
                if (matched) ids.add(id);
            }
        }
        return ids;
    }

    private static boolean containsLetters(String s) {
        for (int i = 0; i < s.length(); i++) {
            if (Character.isLetter(s.charAt(i))) return true;
        }
        return false;
    }

    private static String idInClause(List<Long> ids) {
        if (ids.isEmpty()) return "1 = 0";
        StringBuilder sb = new StringBuilder("_id IN (");
        for (int i = 0; i < ids.size(); i++) {
            if (i > 0) sb.append(',');
            sb.append(ids.get(i));
        }
        sb.append(')');
        return sb.toString();
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CALL_LOG);
        if (sUriMatcher.match(uri) != CALLS) {
            throw new IllegalArgumentException("Unknown URL " + uri);
        }
        ContentValues prepared = prepareInsertValues(values);
        long rowId;
        synchronized (mDbLock) {
            rowId = openDatabase().insertOrThrow("calls", null, prepared);
        }
        ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        return ContentUris.withAppendedId(uri, rowId);
    }

    @Override
    public int bulkInsert(Uri uri, ContentValues[] values) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CALL_LOG);
        if (sUriMatcher.match(uri) != CALLS) {
            throw new IllegalArgumentException("Unknown URL " + uri);
        }
        if (values == null || values.length == 0) return 0;
        int inserted = 0;
        synchronized (mDbLock) {
            SQLiteDatabase db = openDatabase();
            db.beginTransaction();
            try {
                for (ContentValues item : values) {
                    if (db.insertOrThrow("calls", null, prepareInsertValues(item)) > 0) {
                        inserted++;
                    }
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

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CALL_LOG);
        int match = sUriMatcher.match(uri);
        String effectiveSelection = selection;
        String[] effectiveArgs = selectionArgs;
        switch (match) {
            case CALLS:
                break;
            case CALLS_ID:
                effectiveSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effectiveArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
        ContentValues prepared = values == null ? new ContentValues() : new ContentValues(values);
        if (prepared.containsKey("number") && !prepared.containsKey("normalized_number")) {
            String normalized = ContactsProviderSupport.normalizePhoneNumber(prepared.getAsString("number"));
            if (normalized != null) prepared.put("normalized_number", normalized);
        }
        if (!prepared.containsKey("last_modified")) {
            prepared.put("last_modified", System.currentTimeMillis());
        }
        int count;
        synchronized (mDbLock) {
            count = openDatabase().update("calls", prepared, effectiveSelection, effectiveArgs);
        }
        if (count > 0) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        ContactsProviderSupport.enforce(mPm, mCallers, mGrants, uri,
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION, ContactsProviderSupport.WRITE_CALL_LOG);
        int match = sUriMatcher.match(uri);
        String effectiveSelection = selection;
        String[] effectiveArgs = selectionArgs;
        switch (match) {
            case CALLS:
                break;
            case CALLS_ID:
                effectiveSelection = ContactsProviderSupport.combineSelection(selection, "_id = ?");
                effectiveArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, Long.toString(ContentUris.parseId(uri)));
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
        int count;
        synchronized (mDbLock) {
            count = openDatabase().delete("calls", effectiveSelection, effectiveArgs);
        }
        if (count > 0) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    private static ContentValues prepareInsertValues(ContentValues values) {
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        if (!copy.containsKey("new")) copy.put("new", 1);
        if (!copy.containsKey("presentation")) copy.put("presentation", 1);
        if (!copy.containsKey("last_modified")) copy.put("last_modified", System.currentTimeMillis());
        if (copy.containsKey("number") && !copy.containsKey("normalized_number")) {
            String normalized = ContactsProviderSupport.normalizePhoneNumber(copy.getAsString("number"));
            if (normalized != null) copy.put("normalized_number", normalized);
        }
        return copy;
    }
}
