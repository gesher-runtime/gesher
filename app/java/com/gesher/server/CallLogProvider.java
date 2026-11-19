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
import android.provider.CallLog;

import java.io.File;

/**
 * Built-in SQLite-backed {@code content://call_log} provider for
 * {@link CallLog.Calls}. Starts empty on a clean device and enforces
 * {@code READ_CALL_LOG} / {@code WRITE_CALL_LOG} via {@link PackageManagerService}.
 */
final class CallLogProvider extends ContentProvider {
    private static final int CALLS = 1;
    private static final int CALLS_ID = 2;
    private static final int CALLS_FILTER = 3;

    private static final int DB_VERSION = 1;
    private static final Uri NOTIFY_URI = Uri.parse("content://call_log/calls");

    private static final UriMatcher sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);

    static {
        sUriMatcher.addURI(ContactsProviderSupport.CALL_LOG_AUTHORITY, "calls", CALLS);
        sUriMatcher.addURI(ContactsProviderSupport.CALL_LOG_AUTHORITY, "calls/#", CALLS_ID);
        sUriMatcher.addURI(ContactsProviderSupport.CALL_LOG_AUTHORITY, "calls/filter/*", CALLS_FILTER);
    }

    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final File mDatabase;
    private final Object mDbLock = new Object();
    private SQLiteDatabase mDb;
    private boolean mInitialized;
    private Throwable mInitializationFailure;

    CallLogProvider(PackageManagerService pm, ActivityTaskManagerService atm) {
        this(pm, atm, new File(ContactsProviderSupport.dataDirectory(), "databases/calllog.db"));
    }

    CallLogProvider(PackageManagerService pm, ActivityTaskManagerService atm, File databaseFile) {
        mPm = pm;
        mCallers = pm == null ? null : new CallerIdentity(pm, atm);
        mDatabase = databaseFile.getAbsoluteFile();
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
                throw new IllegalStateException("Failed to create CallLog database directory: " + parent);
            }
            SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(mDatabase, null);
            try {
                if (db.getVersion() < DB_VERSION) {
                    db.beginTransaction();
                    try {
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
                                + "asserted_display_name TEXT"
                                + ");");
                        db.execSQL("CREATE INDEX IF NOT EXISTS calls_date_idx ON calls(date DESC);");
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
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.READ_CALL_LOG);
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
            case CALLS_FILTER: {
                String query = uri.getPathSegments().get(2);
                String pattern = "%" + query + "%";
                effectiveSelection = ContactsProviderSupport.combineSelection(
                        selection,
                        "(number LIKE ? OR normalized_number LIKE ? OR name LIKE ?)");
                effectiveArgs = ContactsProviderSupport.prependArgs(
                        selectionArgs, pattern, pattern, pattern);
                break;
            }
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
        String limit = ContactsProviderSupport.parseLimit(uri);
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables("calls");
        String order = sortOrder != null ? sortOrder : "date DESC";
        Cursor cursor;
        synchronized (mDbLock) {
            cursor = qb.query(openDatabase(), projection, effectiveSelection, effectiveArgs,
                    null, null, order, limit);
        }
        ContactsProviderSupport.setNotificationUri(cursor, getContext(), uri);
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CALL_LOG);
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
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CALL_LOG);
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
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CALL_LOG);
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
        ContactsProviderSupport.enforce(mPm, mCallers, ContactsProviderSupport.WRITE_CALL_LOG);
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
        return copy;
    }
}
