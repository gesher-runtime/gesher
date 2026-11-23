/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.gesher.server;

import java.util.List;
import java.io.File;
import android.app.ContentProviderHolder;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;

import android.app.backup.BackupManager;
import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.UriMatcher;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.database.SQLException;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.database.sqlite.SQLiteQueryBuilder;
import android.net.Uri;
import android.os.Binder;
import android.os.Process;
import android.os.UserHandle;
import android.provider.UserDictionary;
import android.provider.UserDictionary.Words;
import android.text.TextUtils;
import android.util.ArrayMap;
import android.util.Log;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.TextServicesManager;
import com.android.internal.pm.pkg.component.ParsedProvider;

/**
 * Port of the pinned Android 16 r4 UserDictionaryProvider. SQLite/query semantics
 * remain AOSP's. Gesher adds system-server ownership, real package metadata and
 * authenticated caller checks for its legacy shared-UID runtime mode.
 * Source and local adaptations: docs/user-dictionary.md.
 */
final class UserDictionaryProvider extends ContentProvider {
    static final String PACKAGE = "com.android.providers.userdictionary";
    static final String SHARED_USER = "android.uid.shared";
    static final String PROCESS = "android.process.acore";
    static final String CLASS = "com.gesher.server.UserDictionaryProvider";
    private final CallerIdentity mCallers;
    private final PackageManagerService mPm;
    private final File mDatabase;
    private boolean mInitialized;
    private Throwable mInitializationFailure;

    UserDictionaryProvider(PackageManagerService pm, ActivityTaskManagerService atm) {
        this(pm, atm, new File(dataDirectory(), "databases/user_dict.db"));
    }

    UserDictionaryProvider(PackageManagerService pm, ActivityTaskManagerService atm, File file) {
        mPm = pm;
        mCallers = pm == null ? null : new CallerIdentity(pm, atm);
        mDatabase = file.getAbsoluteFile();
    }

    static File dataDirectory() {
        // Ordinary APK installation/removal owns data/<package>. A runtime-owned
        // provider must not adopt or lose a pre-existing APK's private files.
        return new File(Device.dataDir(), "system/user-dictionary");
    }

    private static final class Identity {
        // This key cannot be an APK package name. A legacy installed dictionary
        // may have left its original identity or shared-group membership on disk.
        private static final int UID = PackageUids.uidFor(PACKAGE + "/hosted-provider");
    }

    static int uid() { return Identity.UID; }

    static ApplicationInfo applicationInfo() {
        ApplicationInfo info = new ApplicationInfo();
        info.packageName = PACKAGE;
        info.className = null;
        info.processName = PROCESS;
        info.uid = uid();
        info.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED
                | ApplicationInfo.FLAG_HAS_CODE;
        info.enabled = true;
        info.targetSdkVersion = android.os.Build.VERSION.SDK_INT;
        info.nonLocalizedLabel = "User Dictionary";
        String root = System.getenv("ANDROID_ROOT");
        info.sourceDir = (root == null ? "/system" : root) + "/framework/gesher-app.jar";
        info.publicSourceDir = info.sourceDir;
        info.dataDir = dataDirectory().getPath();
        return info;
    }

    static ProviderInfo providerInfo() {
        ProviderInfo info = new ProviderInfo();
        info.applicationInfo = applicationInfo();
        info.packageName = PACKAGE;
        info.name = CLASS;
        info.processName = info.applicationInfo.processName;
        info.authority = AUTHORITY;
        info.enabled = true;
        info.exported = true;
        info.multiprocess = false;
        // Android 16's provider has no read/write permissions. Legacy dictionary
        // permissions are marked removed; access is checked against enabled services.
        return info;
    }

    static PackageInfo packageInfo(long flags) {
        PackageInfo info = new PackageInfo();
        info.packageName = PACKAGE;
        info.sharedUserId = SHARED_USER;
        info.applicationInfo = applicationInfo();
        if ((flags & PackageManager.GET_PROVIDERS) != 0 && matchesProviderFlags(flags)) {
            info.providers = new ProviderInfo[]{providerInfo()};
        }
        return info;
    }

    static boolean matchesProviderFlags(long flags) {
        long directBoot = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        // The pinned manifest does not declare a direct-boot-aware provider.
        // Like installed APK components, an unspecified mask includes both kinds.
        return directBoot != PackageManager.MATCH_DIRECT_BOOT_AWARE;
    }

    /** Built-in ownership is admitted before any APK files/registry are published. */
    static void checkPackageAdmission(ApkPackage pkg)
            throws android.content.pm.PackageParser.PackageParserException {
        if (PACKAGE.equals(pkg.packageName)) {
            throw new android.content.pm.PackageParser.PackageParserException(
                    PackageManager.INSTALL_FAILED_UPDATE_INCOMPATIBLE,
                    "The built-in user dictionary is updated with the runtime");
        }
        // Ownership is a manifest-admission rule, including disabled declarations.
        // It must not require generating installed PackageInfo (and resolving its
        // libraries) just to inspect the authoritative parsed provider names.
        for (ParsedProvider provider : pkg.parsed.getProviders()) {
            if (ContentProviders.hasDeclaredAuthority(provider.getAuthority(), AUTHORITY)) {
                throw new android.content.pm.PackageParser.PackageParserException(
                        PackageManager.INSTALL_FAILED_CONFLICTING_PROVIDER,
                        "Provider authority already owned by " + PACKAGE + ": " + AUTHORITY);
            }
        }
    }

    synchronized ContentProviderHolder holder() {
        if (mInitializationFailure != null) {
            throw new IllegalStateException("User dictionary provider initialization failed",
                    mInitializationFailure);
        }
        if (!mInitialized) {
            Context system = android.app.GesherSettingsContext.get();
            // BackupManager obtains its owning package from this context. Database
            // access uses the absolute, device-private path below; the framework's
            // actual resolver still sends notifications through ContentService.
            Context context = new ContextWrapper(system) {
                @Override public String getPackageName() { return PACKAGE; }
                @Override public ApplicationInfo getApplicationInfo() { return applicationInfo(); }
            };
            try {
                attachInfo(context, providerInfo());
                mInitialized = true;
            } catch (RuntimeException | Error failure) {
                // attachInfo installs its context before invoking onCreate. It
                // cannot initialize this instance again after a partial failure.
                // Preserve that failure and never publish its unfinished transport.
                mInitializationFailure = failure;
                if (mOpenHelper != null) {
                    try { mOpenHelper.close(); }
                    catch (RuntimeException | Error cleanup) {
                        if (cleanup != failure) failure.addSuppressed(cleanup);
                    }
                    mOpenHelper = null;
                }
                throw failure;
            }
        }
        ContentProviderHolder holder = new ContentProviderHolder(providerInfo());
        holder.provider = getIContentProvider();
        holder.noReleaseNeeded = true;
        return holder;
    }

    @Override public void shutdown() {
        if (mOpenHelper != null) mOpenHelper.close();
    }


    /**
     * DB versions are as follow:
     *
     * Version 1:
     *   Up to IceCreamSandwich 4.0.3 - API version 15
     *   Contient ID (INTEGER PRIMARY KEY), WORD (TEXT), FREQUENCY (INTEGER),
     *   LOCALE (TEXT), APP_ID (INTEGER).
     *
     * Version 2:
     *   From IceCreamSandwich, 4.1 - API version 16
     *   Adds SHORTCUT (TEXT).
     */

    static final String AUTHORITY = UserDictionary.AUTHORITY;

    private static final String TAG = "UserDictionaryProvider";

    private static final int DATABASE_VERSION = 2;

    private static final String USERDICT_TABLE_NAME = "words";

    private static final int IDLE_CONNECTION_TIMEOUT_MS = 30000;

    private static ArrayMap<String, String> sDictProjectionMap;

    private static final UriMatcher sUriMatcher;

    private static final int WORDS = 1;

    private static final int WORD_ID = 2;

    static {
        sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);
        sUriMatcher.addURI(AUTHORITY, "words", WORDS);
        sUriMatcher.addURI(AUTHORITY, "words/#", WORD_ID);

        sDictProjectionMap = new ArrayMap<>();
        sDictProjectionMap.put(Words._ID, Words._ID);
        sDictProjectionMap.put(Words.WORD, Words.WORD);
        sDictProjectionMap.put(Words.FREQUENCY, Words.FREQUENCY);
        sDictProjectionMap.put(Words.LOCALE, Words.LOCALE);
        sDictProjectionMap.put(Words.APP_ID, Words.APP_ID);
        sDictProjectionMap.put(Words.SHORTCUT, Words.SHORTCUT);
    }

    private BackupManager mBackupManager;
    private InputMethodManager mImeManager;
    private TextServicesManager mTextServiceManager;

    /**
     * This class helps open, create, and upgrade the database file.
     */
    private static class DatabaseHelper extends SQLiteOpenHelper {

        DatabaseHelper(Context context, String path) {
            super(context, path, null, DATABASE_VERSION);
            // Memory optimization - close idle connections after 30s of inactivity
            setIdleConnectionTimeout(IDLE_CONNECTION_TIMEOUT_MS);
        }

        @Override
        public void onCreate(SQLiteDatabase db) {
            db.execSQL("CREATE TABLE " + USERDICT_TABLE_NAME + " ("
                    + Words._ID + " INTEGER PRIMARY KEY,"
                    + Words.WORD + " TEXT,"
                    + Words.FREQUENCY + " INTEGER,"
                    + Words.LOCALE + " TEXT,"
                    + Words.APP_ID + " INTEGER,"
                    + Words.SHORTCUT + " TEXT"
                    + ");");
        }

        @Override
        public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
            if (oldVersion == 1 && newVersion == 2) {
                Log.i(TAG, "Upgrading database from version " + oldVersion
                        + " to version 2: adding " + Words.SHORTCUT + " column");
                db.execSQL("ALTER TABLE " + USERDICT_TABLE_NAME
                        + " ADD " + Words.SHORTCUT + " TEXT;");
            } else {
                Log.w(TAG, "Upgrading database from version " + oldVersion + " to "
                        + newVersion + ", which will destroy all old data");
                db.execSQL("DROP TABLE IF EXISTS " + USERDICT_TABLE_NAME);
                onCreate(db);
            }
        }
    }

    private DatabaseHelper mOpenHelper;

    @Override
    public boolean onCreate() {
        File parent = mDatabase.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()) {
            throw new IllegalStateException("Cannot create user dictionary database directory");
        }
        mOpenHelper = new DatabaseHelper(getContext(), mDatabase.getPath());
        mBackupManager = new BackupManager(getContext());
        mImeManager = getContext().getSystemService(InputMethodManager.class);
        mTextServiceManager = getContext().getSystemService(TextServicesManager.class);
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        // Only the enabled IMEs and spell checkers can access this provider.
        if (!canCallerAccessUserDictionary()) {
            return getEmptyCursorOrThrow(projection);
        }

        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();

        switch (sUriMatcher.match(uri)) {
            case WORDS:
                qb.setTables(USERDICT_TABLE_NAME);
                qb.setProjectionMap(sDictProjectionMap);
                break;

            case WORD_ID:
                qb.setTables(USERDICT_TABLE_NAME);
                qb.setProjectionMap(sDictProjectionMap);
                qb.appendWhere("_id" + "=" + uri.getPathSegments().get(1));
                break;

            default:
                throw new IllegalArgumentException("Unknown URI " + uri);
        }

        // If no sort order is specified use the default
        String orderBy;
        if (TextUtils.isEmpty(sortOrder)) {
            orderBy = Words.DEFAULT_SORT_ORDER;
        } else {
            orderBy = sortOrder;
        }

        // Get the database and run the query
        SQLiteDatabase db = mOpenHelper.getReadableDatabase();
        Cursor c = qb.query(db, projection, selection, selectionArgs, null, null, orderBy);

        // Tell the cursor what uri to watch, so it knows when its source data changes
        long identity = Binder.clearCallingIdentity();
        try { c.setNotificationUri(getContext().getContentResolver(), uri); }
        finally { Binder.restoreCallingIdentity(identity); }
        return c;
    }

    @Override
    public String getType(Uri uri) {
        switch (sUriMatcher.match(uri)) {
            case WORDS:
                return Words.CONTENT_TYPE;

            case WORD_ID:
                return Words.CONTENT_ITEM_TYPE;

            default:
                throw new IllegalArgumentException("Unknown URI " + uri);
        }
    }

    @Override
    public Uri insert(Uri uri, ContentValues initialValues) {
        // Validate the requested uri
        if (sUriMatcher.match(uri) != WORDS) {
            throw new IllegalArgumentException("Unknown URI " + uri);
        }

        // Only the enabled IMEs and spell checkers can access this provider.
        if (!canCallerAccessUserDictionary()) {
            return null;
        }

        ContentValues values;
        if (initialValues != null) {
            values = new ContentValues(initialValues);
        } else {
            values = new ContentValues();
        }

        if (!values.containsKey(Words.WORD)) {
            throw new SQLException("Word must be specified");
        }

        if (!values.containsKey(Words.FREQUENCY)) {
            values.put(Words.FREQUENCY, "1");
        }

        if (!values.containsKey(Words.LOCALE)) {
            values.put(Words.LOCALE, (String) null);
        }

        if (!values.containsKey(Words.SHORTCUT)) {
            values.put(Words.SHORTCUT, (String) null);
        }

        values.put(Words.APP_ID, 0);

        SQLiteDatabase db = mOpenHelper.getWritableDatabase();
        long rowId = db.insert(USERDICT_TABLE_NAME, Words.WORD, values);
        if (rowId > 0) {
            Uri wordUri = ContentUris.withAppendedId(UserDictionary.Words.CONTENT_URI, rowId);
            notifyChanged(wordUri);
            return wordUri;
        }

        throw new SQLException("Failed to insert row into " + uri);
    }

    @Override
    public int delete(Uri uri, String where, String[] whereArgs) {
        // Only the enabled IMEs and spell checkers can access this provider.
        if (!canCallerAccessUserDictionary()) {
            return 0;
        }

        SQLiteDatabase db = mOpenHelper.getWritableDatabase();
        int count;
        switch (sUriMatcher.match(uri)) {
            case WORDS:
                count = db.delete(USERDICT_TABLE_NAME, where, whereArgs);
                break;

            case WORD_ID:
                String wordId = uri.getPathSegments().get(1);
                count = db.delete(USERDICT_TABLE_NAME, Words._ID + "=" + wordId
                        + (!TextUtils.isEmpty(where) ? " AND (" + where + ')' : ""), whereArgs);
                break;

            default:
                throw new IllegalArgumentException("Unknown URI " + uri);
        }

        notifyChanged(uri);
        return count;
    }

    @Override
    public int update(Uri uri, ContentValues values, String where, String[] whereArgs) {
        // Only the enabled IMEs and spell checkers can access this provider.
        if (!canCallerAccessUserDictionary()) {
            return 0;
        }

        SQLiteDatabase db = mOpenHelper.getWritableDatabase();
        int count;
        switch (sUriMatcher.match(uri)) {
            case WORDS:
                count = db.update(USERDICT_TABLE_NAME, values, where, whereArgs);
                break;

            case WORD_ID:
                String wordId = uri.getPathSegments().get(1);
                count = db.update(USERDICT_TABLE_NAME, values, Words._ID + "=" + wordId
                        + (!TextUtils.isEmpty(where) ? " AND (" + where + ')' : ""), whereArgs);
                break;

            default:
                throw new IllegalArgumentException("Unknown URI " + uri);
        }

        notifyChanged(uri);
        return count;
    }

    private void notifyChanged(Uri uri) {
        // Unlike AOSP's separate provider process, this provider shares local
        // ContentService calls with system_server. Publish with provider identity.
        long identity = Binder.clearCallingIdentity();
        try {
            getContext().getContentResolver().notifyChange(uri, null);
            mBackupManager.dataChanged();
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    private boolean canCallerAccessUserDictionary() {
        final int callingUid = Binder.getCallingUid();
        final int callingPid = Binder.getCallingPid();
        final String attached = mCallers == null ? null : mCallers.packageForPid(callingPid);
        final String localPackage = callingPid == Process.myPid()
                ? android.app.ActivityThread.currentPackageName() : null;
        final String actual = attached != null ? attached : localPackage;

        // Legacy Gesher can host an app and services in one process or give them
        // the same guest UID. That must not turn the provider-UID AOSP exemption
        // into dictionary access for every ordinary application.
        if (actual == null) {
            if (callingUid == Process.ROOT_UID
                    || UserHandle.getAppId(callingUid) == Process.SYSTEM_UID) return true;
            if (callingPid == Process.myPid()) return true;
            // Preserve ContentProvider's package/UID validation on real transport
            // calls, even when this runtime has no attached application record.
            getCallingPackage();
            return false;
        }
        ApkPackage caller = mPm == null ? null : mPm.get(actual);
        if (caller == null || mPm.isRemovalPending(actual) || caller.uid() != callingUid) return false;
        final String callingPackage = getCallingPackage();
        if (!actual.equals(callingPackage)) return false;
        if (callingUid == Process.ROOT_UID
                || UserHandle.getAppId(callingUid) == Process.SYSTEM_UID) return true;

        // In AOSP these service calls originate in the provider process. Retain
        // that identity when the same services live in Gesher's server process.
        final long identity = Binder.clearCallingIdentity();
        try {
            List<InputMethodInfo> imeInfos = mImeManager == null ? null : mImeManager.getEnabledInputMethodList();
            if (imeInfos != null) {
                final int imeInfoCount = imeInfos.size();
                for (int i = 0; i < imeInfoCount; i++) {
                    InputMethodInfo imeInfo = imeInfos.get(i);
                    if (imeInfo.getServiceInfo().applicationInfo.uid == callingUid
                            && imeInfo.getPackageName().equals(callingPackage)) {
                        return true;
                    }
                }
            }

            SpellCheckerInfo[] scInfos =
                    mTextServiceManager == null ? null : mTextServiceManager.getEnabledSpellCheckers();
            if (scInfos != null) {
                for (SpellCheckerInfo scInfo : scInfos) {
                    if (scInfo.getServiceInfo().applicationInfo.uid == callingUid
                            && scInfo.getPackageName().equals(callingPackage)) {
                        return true;
                    }
                }
            }
            return false;
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    private static Cursor getEmptyCursorOrThrow(String[] projection) {
        if (projection != null) {
            for (String column : projection) {
                if (sDictProjectionMap.get(column) == null) {
                    throw new IllegalArgumentException("Unknown column: " + column);
                }
            }
        } else {
            final int columnCount = sDictProjectionMap.size();
            projection = new String[columnCount];
            for (int i = 0; i < columnCount; i++) {
                projection[i] = sDictProjectionMap.keyAt(i);
            }
        }

        return new MatrixCursor(projection, 0);
    }
}
