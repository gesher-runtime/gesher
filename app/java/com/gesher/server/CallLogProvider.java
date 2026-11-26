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
import android.content.Context;
import android.content.PermissionChecker;
import android.content.UriMatcher;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.net.Uri;
import android.os.Binder;
import android.os.Process;
import android.provider.CallLog;

import java.io.File;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * Built-in SQLite-backed {@code content://call_log} provider for
 * {@link CallLog.Calls}. Starts empty on a clean device and enforces
 * {@code READ_CALL_LOG} / {@code WRITE_CALL_LOG} and their AppOps.
 */
final class CallLogProvider extends ContentProvider {
    private static final int CALLS = 1;
    private static final int CALLS_ID = 2;
    private static final int CALLS_FILTER = 3;

    private static final int DB_VERSION = 1;
    private static final Uri NOTIFY_URI = CallLog.CONTENT_URI;

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
                "transcription", "transcription_state", "phone_account_hidden",
                "add_for_all_users", "is_call_log_phone_account_migration_pending"
        }) {
            map.put(col, col);
        }
        sCallsProjectionMap = Collections.unmodifiableMap(map);
    }

    private final AttributionSource mDataSource = new AttributionSource.Builder(Process.SYSTEM_UID)
            .setPackageName(ContactsProviderSupport.PACKAGE).build();
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final UriGrants mGrants;
    private final File mDatabase;
    private final ContentProviders.RegisteredProvider mRegistration;
    private final Object mDbLock = new Object();
    private SQLiteDatabase mDb;
    private boolean mInitialized;
    private boolean mStrictPhoneComparison;
    private int mPhoneMinMatch;
    private Throwable mInitializationFailure;

    CallLogProvider(ContentProviders providers, UriGrants grants) {
        this(providers, grants,
                new File(ContactsProviderSupport.dataDirectory(), "databases/calllog.db"));
    }

    CallLogProvider(ContentProviders providers, UriGrants grants, File databaseFile) {
        mPm = providers == null ? null : providers.packageManager();
        mCallers = providers == null ? null : providers.callers();
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
        attachTo(android.app.GesherSettingsContext.get());
        return ContactsProviderSupport.holder(info(), getIContentProvider());
    }

    synchronized void attachTo(Context context) {
        if (mInitializationFailure != null) {
            throw new IllegalStateException("CallLog provider initialization failed",
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
        return enforceCallLogPermission(source, ContactsProviderSupport.READ_CALL_LOG);
    }

    @Override
    protected int enforceWritePermissionInner(Uri uri, AttributionSource source) {
        return enforceCallLogPermission(source, ContactsProviderSupport.WRITE_CALL_LOG);
    }

    private int enforceCallLogPermission(AttributionSource source, String permission) {
        if (Binder.getCallingPid() == Process.myPid()) return PermissionChecker.PERMISSION_GRANTED;
        ContactsProviderSupport.authenticate(mCallers, source);
        // This in-process call would otherwise see the incoming app's Binder identity
        // while authenticating the system provider attribution at the head of the chain.
        long identity = Binder.clearCallingIdentity();
        try {
            int result = PermissionChecker.checkPermissionForDataDeliveryFromDataSource(
                    getContext(), permission, -1, new AttributionSource(mDataSource, source), null);
            if (result == PermissionChecker.PERMISSION_HARD_DENIED) {
                throw new SecurityException("Call-log access requires " + permission);
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
                + "transcription TEXT,"
                + "transcription_state INTEGER NOT NULL DEFAULT 0,"
                + "phone_account_hidden INTEGER NOT NULL DEFAULT 0,"
                + "add_for_all_users INTEGER NOT NULL DEFAULT 0,"
                + "is_call_log_phone_account_migration_pending INTEGER NOT NULL DEFAULT 0"
                + ");");
        db.execSQL("CREATE INDEX IF NOT EXISTS calls_date_idx ON calls(date DESC);");
    }

    private static void onUpgradeDatabase(SQLiteDatabase db, int oldVersion, int newVersion) {
        throw new IllegalStateException("Unsupported call-log schema upgrade: "
                + oldVersion + " to " + newVersion);
    }

    @Override
    public boolean onCreate() {
        mStrictPhoneComparison = getContext().getResources().getBoolean(
                com.android.internal.R.bool.config_use_strict_phone_number_comparation);
        mPhoneMinMatch = getContext().getResources().getInteger(
                com.android.internal.R.integer.config_phonenumber_compare_min_match);
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
        SQLiteQueryBuilder qb = queryBuilder();
        restrictVoicemails(qb, uri, false);
        qb.appendWhereStandalone("phone_account_hidden = 0");
        String[] args = selectionArgs;
        switch (sUriMatcher.match(uri)) {
            case CALLS:
                break;
            case CALLS_ID:
                qb.appendWhereStandalone("_id = " + ContentUris.parseId(uri));
                break;
            case CALLS_FILTER:
                int strict = mStrictPhoneComparison ? 1 : 0;
                qb.appendWhereStandalone("PHONE_NUMBERS_EQUAL(number, ?, "
                        + strict + ", " + mPhoneMinMatch + ")");
                args = ContactsProviderSupport.prependArgs(selectionArgs, uri.getLastPathSegment());
                break;
            default:
                throw new IllegalArgumentException("Unknown URL " + uri);
        }
        String limit = ContactsProviderSupport.parseLimit(uri);
        String order = sortOrder != null ? sortOrder : "date DESC";
        Cursor cursor;
        synchronized (mDbLock) {
            cursor = qb.query(openDatabase(), projection, selection, args,
                    null, null, order, limit);
        }
        try {
            ContactsProviderSupport.setNotificationUri(cursor, getContext(), NOTIFY_URI);
            return cursor;
        } catch (RuntimeException | Error failure) {
            cursor.close();
            throw failure;
        }
    }

    private static SQLiteQueryBuilder queryBuilder() {
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables("calls");
        qb.setProjectionMap(sCallsProjectionMap);
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        return qb;
    }

    private void restrictVoicemails(SQLiteQueryBuilder qb, Uri uri, boolean write) {
        if (allowsVoicemails(uri)) {
            enforceVoicemailPermission(write);
        } else {
            qb.appendWhereStandalone("type != " + CallLog.Calls.VOICEMAIL_TYPE);
        }
    }

    private static boolean allowsVoicemails(Uri uri) {
        return uri.getBooleanQueryParameter(CallLog.Calls.ALLOW_VOICEMAILS_PARAM_KEY, false);
    }

    private static boolean isVoicemail(ContentValues values) {
        Object value = values == null ? null : values.get(CallLog.Calls.TYPE);
        if (value == null) return false;
        // Validate the integer form before SQLite applies column affinity.
        try {
            return Integer.parseInt(value.toString()) == CallLog.Calls.VOICEMAIL_TYPE;
        } catch (NumberFormatException invalidType) {
            throw new IllegalArgumentException("Call-log type must be an integer", invalidType);
        }
    }

    private static void requireVoicemailUri(Uri uri) {
        if (!allowsVoicemails(uri)) {
            throw new IllegalArgumentException("Use the voicemail-enabled URI for voicemail records");
        }
    }

    private void enforceVoicemailPermission(boolean write) {
        if (Binder.getCallingPid() == Process.myPid()) return;
        String permission = write ? "com.android.voicemail.permission.WRITE_VOICEMAIL"
                : "com.android.voicemail.permission.READ_VOICEMAIL";
        if (mCallers == null || mPm.permissions().checkPermission(mCallers.callerPackage(),
                permission) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Permission Denial: requires " + permission);
        }
    }

    private static void validateColumns(ContentValues values) {
        if (values == null) return;
        for (String column : values.keySet()) {
            if (!sCallsProjectionMap.containsKey(column)) {
                throw new IllegalArgumentException("Unknown call-log column " + column);
            }
        }
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        if (sUriMatcher.match(uri) != CALLS) {
            throw new IllegalArgumentException("Unknown URL " + uri);
        }
        ContentValues prepared = prepareInsertValues(uri, values);
        long rowId;
        synchronized (mDbLock) {
            rowId = openDatabase().insertOrThrow("calls", null, prepared);
        }
        ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        return ContentUris.withAppendedId(uri, rowId);
    }

    @Override
    public int bulkInsert(Uri uri, ContentValues[] values) {
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
                    if (db.insertOrThrow("calls", null, prepareInsertValues(uri, item)) > 0) {
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
        validateColumns(values);
        if (isVoicemail(values)) requireVoicemailUri(uri);
        SQLiteQueryBuilder qb = queryBuilder();
        restrictVoicemails(qb, uri, true);
        switch (sUriMatcher.match(uri)) {
            case CALLS:
                break;
            case CALLS_ID:
                qb.appendWhereStandalone("_id = " + ContentUris.parseId(uri));
                break;
            default:
                throw new UnsupportedOperationException("Cannot update URL " + uri);
        }
        ContentValues prepared = values == null ? new ContentValues() : new ContentValues(values);
        if (!prepared.containsKey("last_modified")) {
            prepared.put("last_modified", System.currentTimeMillis());
        }
        int count;
        synchronized (mDbLock) {
            count = qb.update(openDatabase(), prepared, selection, selectionArgs);
        }
        if (count > 0) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        if (sUriMatcher.match(uri) != CALLS) {
            throw new UnsupportedOperationException("Cannot delete URL " + uri);
        }
        SQLiteQueryBuilder qb = queryBuilder();
        restrictVoicemails(qb, uri, true);
        int count;
        synchronized (mDbLock) {
            count = qb.delete(openDatabase(), selection, selectionArgs);
        }
        if (count > 0) {
            ContactsProviderSupport.notifyChange(getContext(), NOTIFY_URI);
        }
        return count;
    }

    private ContentValues prepareInsertValues(Uri uri, ContentValues values) {
        validateColumns(values);
        if (isVoicemail(values)) {
            requireVoicemailUri(uri);
            enforceVoicemailPermission(true);
        }
        ContentValues copy = values == null ? new ContentValues() : new ContentValues(values);
        if (!copy.containsKey("new")) copy.put("new", 1);
        if (!copy.containsKey("presentation")) copy.put("presentation", 1);
        if (!copy.containsKey("last_modified")) copy.put("last_modified", System.currentTimeMillis());
        return copy;
    }
}
