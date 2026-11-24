/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.DownloadManager;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.net.Uri;
import android.provider.Downloads;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.Random;

/**
 * The {@code downloads} provider's data and its access rules: AOSP DownloadProvider
 * (android-16.0.0_r4 packages/providers/DownloadProvider) without the framework
 * transport, which {@link DownloadProvider} supplies. Rows live in a SQLite database with
 * AOSP's schema; every app-visible read, update and delete goes through
 * {@link SQLiteQueryBuilder} in strict mode with AOSP's projection map and per-UID
 * where clause, so selections, projections and ordering behave as on Android.
 *
 * <p>Destinations are Gesher's shared storage (the emulated volume at
 * {@code $ANDROID_DATA/media/0}) and this provider's private cache partition. AOSP's
 * {@code /storage/emulated/<user>} patterns are evaluated against that volume root.
 */
final class DownloadRequests {
    static final String DB_TABLE = "downloads";
    static final String HEADERS_TABLE = Downloads.Impl.RequestHeaders.HEADERS_DB_TABLE;
    static final int DB_VERSION = 114;
    /** Constants.UID, ETAG, RETRY_AFTER_X_REDIRECT_COUNT, OTA_UPDATE, NO_SYSTEM_FILES. */
    static final String UID = "uid", ETAG = "etag", RETRY_AFTER = "method";
    static final String DEFAULT_DL_FILENAME = "downloadfile";
    private static final String RECOVERY_DIRECTORY = "recovery";
    private static final String DEFAULT_DOWNLOAD_FILE_NAME_PREFIX = "Download";

    /** The URI kinds DownloadProvider's matcher accepts. */
    static final int MY_DOWNLOADS = 1, MY_DOWNLOADS_ID = 2, MY_DOWNLOADS_ID_HEADERS = 3,
            ALL_DOWNLOADS = 4, ALL_DOWNLOADS_ID = 5, ALL_DOWNLOADS_ID_HEADERS = 6;

    /** The Binder caller of one provider operation. */
    interface Caller {
        int uid();
        /** The caller's authenticated package (ContentProvider.getCallingPackage). */
        String packageName();
        boolean holds(String permission);
        /** Binder.getCallingPid() == Process.myPid(). */
        boolean local();
        /** AppOpsManager OP_LEGACY_STORAGE for the caller. */
        boolean legacyStorage();
        /** WRITE_EXTERNAL_STORAGE and its app op note, as checkDestinationFilePathRestrictions. */
        boolean writeExternalStorage();
        /** REQUEST_INSTALL_PACKAGES permission or app op (isCallingAppInstaller). */
        boolean installer();
    }

    /** What the provider needs from the system around it. */
    interface Host {
        /** The primary shared storage volume (Environment.getExternalStorageDirectory()). */
        File externalRoot();
        /** The provider's own files and cache directories (the cache partition). */
        File filesDir();
        File cacheDir();
        boolean uidOwnsPackage(int uid, String packageName);
        void grantAllDownloads(String packageName, long id);
        void revokeAllDownloads(long id);
        void notifyChange(Uri uri);
        /** DownloadInfo.sendIntentIfRequested. */
        void sendCompleted(String packageName, String className, boolean publicApi, long id,
                String extras);
        void schedule(long id);
        void cancel(long id);
        long currentTimeMillis();
    }

    private static final Map<String, String> sDownloadsMap = new HashMap<>();
    private static final Map<String, String> sHeadersMap = new HashMap<>();

    private static void addMapping(Map<String, String> map, String column) {
        map.putIfAbsent(column, column);
    }

    private static void addMapping(Map<String, String> map, String column, String rawColumn) {
        map.putIfAbsent(column, rawColumn + " AS " + column);
    }

    static {
        Map<String, String> map = sDownloadsMap;
        addMapping(map, DownloadManager.COLUMN_ID, Downloads.Impl._ID);
        addMapping(map, DownloadManager.COLUMN_LOCAL_FILENAME, Downloads.Impl._DATA);
        addMapping(map, DownloadManager.COLUMN_MEDIAPROVIDER_URI);
        addMapping(map, DownloadManager.COLUMN_DESTINATION);
        addMapping(map, DownloadManager.COLUMN_TITLE);
        addMapping(map, DownloadManager.COLUMN_DESCRIPTION);
        addMapping(map, DownloadManager.COLUMN_URI);
        addMapping(map, DownloadManager.COLUMN_STATUS);
        addMapping(map, DownloadManager.COLUMN_FILE_NAME_HINT);
        addMapping(map, DownloadManager.COLUMN_MEDIA_TYPE, Downloads.Impl.COLUMN_MIME_TYPE);
        addMapping(map, DownloadManager.COLUMN_TOTAL_SIZE_BYTES, Downloads.Impl.COLUMN_TOTAL_BYTES);
        addMapping(map, DownloadManager.COLUMN_LAST_MODIFIED_TIMESTAMP,
                Downloads.Impl.COLUMN_LAST_MODIFICATION);
        addMapping(map, DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR,
                Downloads.Impl.COLUMN_CURRENT_BYTES);
        addMapping(map, DownloadManager.COLUMN_ALLOW_WRITE);
        addMapping(map, DownloadManager.COLUMN_LOCAL_URI, "'placeholder'");
        addMapping(map, DownloadManager.COLUMN_REASON, "'placeholder'");
        addMapping(map, OpenableColumns.DISPLAY_NAME, Downloads.Impl.COLUMN_TITLE);
        addMapping(map, OpenableColumns.SIZE, Downloads.Impl.COLUMN_TOTAL_BYTES);
        for (String column : new String[] {Downloads.Impl._ID, Downloads.Impl._DATA,
                Downloads.Impl.COLUMN_ALLOWED_NETWORK_TYPES, Downloads.Impl.COLUMN_ALLOW_METERED,
                Downloads.Impl.COLUMN_ALLOW_ROAMING, Downloads.Impl.COLUMN_ALLOW_WRITE,
                Downloads.Impl.COLUMN_APP_DATA, Downloads.Impl.COLUMN_BYPASS_RECOMMENDED_SIZE_LIMIT,
                Downloads.Impl.COLUMN_CONTROL, Downloads.Impl.COLUMN_COOKIE_DATA,
                Downloads.Impl.COLUMN_CURRENT_BYTES, Downloads.Impl.COLUMN_DELETED,
                Downloads.Impl.COLUMN_DESCRIPTION, Downloads.Impl.COLUMN_DESTINATION,
                Downloads.Impl.COLUMN_ERROR_MSG, Downloads.Impl.COLUMN_FAILED_CONNECTIONS,
                Downloads.Impl.COLUMN_FILE_NAME_HINT, Downloads.Impl.COLUMN_FLAGS,
                Downloads.Impl.COLUMN_IS_PUBLIC_API, Downloads.Impl.COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI,
                Downloads.Impl.COLUMN_LAST_MODIFICATION, Downloads.Impl.COLUMN_MEDIAPROVIDER_URI,
                Downloads.Impl.COLUMN_MEDIA_SCANNED, Downloads.Impl.COLUMN_MEDIASTORE_URI,
                Downloads.Impl.COLUMN_MIME_TYPE, Downloads.Impl.COLUMN_NO_INTEGRITY,
                Downloads.Impl.COLUMN_NOTIFICATION_CLASS, Downloads.Impl.COLUMN_NOTIFICATION_EXTRAS,
                Downloads.Impl.COLUMN_NOTIFICATION_PACKAGE, Downloads.Impl.COLUMN_OTHER_UID,
                Downloads.Impl.COLUMN_REFERER, Downloads.Impl.COLUMN_STATUS,
                Downloads.Impl.COLUMN_TITLE, Downloads.Impl.COLUMN_TOTAL_BYTES,
                Downloads.Impl.COLUMN_URI, Downloads.Impl.COLUMN_USER_AGENT,
                Downloads.Impl.COLUMN_VISIBILITY, ETAG, RETRY_AFTER, UID}) {
            addMapping(map, column);
        }
        addMapping(sHeadersMap, "id");
        addMapping(sHeadersMap, Downloads.Impl.RequestHeaders.COLUMN_DOWNLOAD_ID);
        addMapping(sHeadersMap, Downloads.Impl.RequestHeaders.COLUMN_HEADER);
        addMapping(sHeadersMap, Downloads.Impl.RequestHeaders.COLUMN_VALUE);
    }

    private final SQLiteDatabase mDb;
    private final Host mHost;
    private final Object mUniqueLock = new Object();
    private final Random mRandom = new Random();

    DownloadRequests(SQLiteDatabase db, Host host) {
        mDb = Objects.requireNonNull(db);
        mHost = Objects.requireNonNull(host);
        createOrUpgrade();
    }

    SQLiteDatabase database() { return mDb; }

    /** The schema DownloadProvider's DatabaseHelper reaches at version 114. */
    private void createOrUpgrade() {
        int version = mDb.getVersion();
        if (version == DB_VERSION) return;
        if (version > DB_VERSION) {
            throw new IllegalStateException("Downloads database is newer than this runtime");
        }
        mDb.beginTransaction();
        try {
            mDb.execSQL("DROP TABLE IF EXISTS " + DB_TABLE);
            mDb.execSQL("CREATE TABLE " + DB_TABLE + "("
                    + Downloads.Impl._ID + " INTEGER PRIMARY KEY AUTOINCREMENT,"
                    + Downloads.Impl.COLUMN_URI + " TEXT, "
                    + RETRY_AFTER + " INTEGER, "
                    + Downloads.Impl.COLUMN_APP_DATA + " TEXT, "
                    + Downloads.Impl.COLUMN_NO_INTEGRITY + " BOOLEAN, "
                    + Downloads.Impl.COLUMN_FILE_NAME_HINT + " TEXT, "
                    + "otaupdate BOOLEAN, "
                    + Downloads.Impl._DATA + " TEXT, "
                    + Downloads.Impl.COLUMN_MIME_TYPE + " TEXT, "
                    + Downloads.Impl.COLUMN_DESTINATION + " INTEGER, "
                    + "no_system BOOLEAN, "
                    + Downloads.Impl.COLUMN_VISIBILITY + " INTEGER, "
                    + Downloads.Impl.COLUMN_CONTROL + " INTEGER, "
                    + Downloads.Impl.COLUMN_STATUS + " INTEGER, "
                    + Downloads.Impl.COLUMN_FAILED_CONNECTIONS + " INTEGER, "
                    + Downloads.Impl.COLUMN_LAST_MODIFICATION + " BIGINT, "
                    + Downloads.Impl.COLUMN_NOTIFICATION_PACKAGE + " TEXT, "
                    + Downloads.Impl.COLUMN_NOTIFICATION_CLASS + " TEXT, "
                    + Downloads.Impl.COLUMN_NOTIFICATION_EXTRAS + " TEXT, "
                    + Downloads.Impl.COLUMN_COOKIE_DATA + " TEXT, "
                    + Downloads.Impl.COLUMN_USER_AGENT + " TEXT, "
                    + Downloads.Impl.COLUMN_REFERER + " TEXT, "
                    + Downloads.Impl.COLUMN_TOTAL_BYTES + " INTEGER, "
                    + Downloads.Impl.COLUMN_CURRENT_BYTES + " INTEGER, "
                    + ETAG + " TEXT, "
                    + UID + " INTEGER, "
                    + Downloads.Impl.COLUMN_OTHER_UID + " INTEGER, "
                    + Downloads.Impl.COLUMN_TITLE + " TEXT, "
                    + Downloads.Impl.COLUMN_DESCRIPTION + " TEXT, "
                    + Downloads.Impl.COLUMN_MEDIA_SCANNED + " BOOLEAN, "
                    + Downloads.Impl.COLUMN_IS_PUBLIC_API + " INTEGER NOT NULL DEFAULT 0, "
                    + Downloads.Impl.COLUMN_ALLOW_ROAMING + " INTEGER NOT NULL DEFAULT 0, "
                    + Downloads.Impl.COLUMN_ALLOWED_NETWORK_TYPES + " INTEGER NOT NULL DEFAULT 0, "
                    + Downloads.Impl.COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI + " INTEGER NOT NULL DEFAULT 1, "
                    + Downloads.Impl.COLUMN_BYPASS_RECOMMENDED_SIZE_LIMIT + " INTEGER NOT NULL DEFAULT 0, "
                    + Downloads.Impl.COLUMN_MEDIAPROVIDER_URI + " TEXT, "
                    + Downloads.Impl.COLUMN_DELETED + " BOOLEAN NOT NULL DEFAULT 0, "
                    + Downloads.Impl.COLUMN_ERROR_MSG + " TEXT, "
                    + Downloads.Impl.COLUMN_ALLOW_METERED + " INTEGER NOT NULL DEFAULT 1, "
                    + Downloads.Impl.COLUMN_ALLOW_WRITE + " BOOLEAN NOT NULL DEFAULT 0, "
                    + Downloads.Impl.COLUMN_FLAGS + " INTEGER NOT NULL DEFAULT 0, "
                    + Downloads.Impl.COLUMN_MEDIASTORE_URI + " TEXT DEFAULT NULL);");
            mDb.execSQL("DROP TABLE IF EXISTS " + HEADERS_TABLE);
            mDb.execSQL("CREATE TABLE " + HEADERS_TABLE + "("
                    + "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    + Downloads.Impl.RequestHeaders.COLUMN_DOWNLOAD_ID + " INTEGER NOT NULL,"
                    + Downloads.Impl.RequestHeaders.COLUMN_HEADER + " TEXT NOT NULL,"
                    + Downloads.Impl.RequestHeaders.COLUMN_VALUE + " TEXT NOT NULL);");
            mDb.setVersion(DB_VERSION);
            mDb.setTransactionSuccessful();
        } finally {
            mDb.endTransaction();
        }
    }

    // ---- URIs ------------------------------------------------------------------------

    /** DownloadProvider's sURIMatcher, or -1. "download" is the legacy alias of my_downloads. */
    static int match(Uri uri) {
        if (uri == null || !"content".equals(uri.getScheme())
                || !Downloads.Impl.AUTHORITY.equals(uri.getAuthority())) return -1;
        List<String> segments = uri.getPathSegments();
        if (segments.isEmpty() || segments.size() > 3) return -1;
        boolean mine;
        switch (segments.get(0)) {
            case "my_downloads": case "download": mine = true; break;
            case "all_downloads": mine = false; break;
            default: return -1;
        }
        if (segments.size() == 1) return mine ? MY_DOWNLOADS : ALL_DOWNLOADS;
        if (!isNumber(segments.get(1))) return -1;
        if (segments.size() == 2) return mine ? MY_DOWNLOADS_ID : ALL_DOWNLOADS_ID;
        if (!Downloads.Impl.RequestHeaders.URI_SEGMENT.equals(segments.get(2))) return -1;
        return mine ? MY_DOWNLOADS_ID_HEADERS : ALL_DOWNLOADS_ID_HEADERS;
    }

    /** UriMatcher's "#": one or more ASCII digits. */
    private static boolean isNumber(String segment) {
        if (segment.isEmpty()) return false;
        for (int i = 0; i < segment.length(); i++) {
            char c = segment.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    private static String downloadId(Uri uri) {
        return uri.getPathSegments().get(1);
    }

    // ---- insert ----------------------------------------------------------------------

    Uri insert(Caller caller, Uri uri, ContentValues values) {
        checkInsertPermissions(caller, values);
        int match = match(uri);
        if (match != MY_DOWNLOADS) {
            throw new IllegalArgumentException("Unknown/Invalid URI " + uri);
        }
        ContentValues filtered = new ContentValues();
        boolean isPublicApi = Objects.equals(
                values.getAsBoolean(Downloads.Impl.COLUMN_IS_PUBLIC_API), Boolean.TRUE);
        Integer dest = values.getAsInteger(Downloads.Impl.COLUMN_DESTINATION);
        if (dest != null) {
            if (!caller.holds(Downloads.Impl.PERMISSION_ACCESS_ADVANCED)
                    && (dest == Downloads.Impl.DESTINATION_CACHE_PARTITION
                            || dest == Downloads.Impl.DESTINATION_CACHE_PARTITION_NOROAMING)) {
                throw new SecurityException("setting destination to : " + dest
                        + " not allowed, unless PERMISSION_ACCESS_ADVANCED is granted");
            }
            if (isPublicApi && dest == Downloads.Impl.DESTINATION_CACHE_PARTITION_PURGEABLE
                    && caller.holds(Downloads.Impl.PERMISSION_CACHE_NON_PURGEABLE)) {
                dest = Downloads.Impl.DESTINATION_CACHE_PARTITION;
            }
            if (dest == Downloads.Impl.DESTINATION_FILE_URI) {
                checkFileUriDestination(caller, values);
            } else if (dest == Downloads.Impl.DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD) {
                checkDownloadedFilePath(caller, values);
            } else if (dest == Downloads.Impl.DESTINATION_EXTERNAL) {
                if (!caller.writeExternalStorage()) throw new SecurityException("No permission to write");
            } else if (dest != Downloads.Impl.DESTINATION_CACHE_PARTITION
                    && dest != Downloads.Impl.DESTINATION_CACHE_PARTITION_PURGEABLE
                    && dest != Downloads.Impl.DESTINATION_CACHE_PARTITION_NOROAMING) {
                // DESTINATION_SYSTEMCACHE_PARTITION and unknown values have no directory.
                throw new IllegalArgumentException("Unsupported destination " + dest);
            }
            filtered.put(Downloads.Impl.COLUMN_DESTINATION, dest);
        }

        ensureDefaultColumns(values);
        copyString(Downloads.Impl.COLUMN_URI, values, filtered);
        copyString(Downloads.Impl.COLUMN_APP_DATA, values, filtered);
        copyBoolean(Downloads.Impl.COLUMN_NO_INTEGRITY, values, filtered);
        copyString(Downloads.Impl.COLUMN_FILE_NAME_HINT, values, filtered);
        copyString(Downloads.Impl.COLUMN_MIME_TYPE, values, filtered);
        copyBoolean(Downloads.Impl.COLUMN_IS_PUBLIC_API, values, filtered);

        Integer vis = values.getAsInteger(Downloads.Impl.COLUMN_VISIBILITY);
        if (vis == null) {
            filtered.put(Downloads.Impl.COLUMN_VISIBILITY,
                    Objects.equals(dest, Downloads.Impl.DESTINATION_EXTERNAL)
                            ? Downloads.Impl.VISIBILITY_VISIBLE_NOTIFY_COMPLETED
                            : Downloads.Impl.VISIBILITY_HIDDEN);
        } else {
            filtered.put(Downloads.Impl.COLUMN_VISIBILITY, vis);
        }
        copyInteger(Downloads.Impl.COLUMN_CONTROL, values, filtered);

        if (Objects.equals(values.getAsInteger(Downloads.Impl.COLUMN_DESTINATION),
                Downloads.Impl.DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD)) {
            filtered.put(Downloads.Impl.COLUMN_STATUS, Downloads.Impl.STATUS_SUCCESS);
            filtered.put(Downloads.Impl.COLUMN_TOTAL_BYTES,
                    values.getAsLong(Downloads.Impl.COLUMN_TOTAL_BYTES));
            filtered.put(Downloads.Impl.COLUMN_CURRENT_BYTES, 0);
            copyString(Downloads.Impl._DATA, values, filtered);
            copyBoolean(Downloads.Impl.COLUMN_ALLOW_WRITE, values, filtered);
        } else {
            filtered.put(Downloads.Impl.COLUMN_STATUS, Downloads.Impl.STATUS_PENDING);
            filtered.put(Downloads.Impl.COLUMN_TOTAL_BYTES, -1);
            filtered.put(Downloads.Impl.COLUMN_CURRENT_BYTES, 0);
        }
        filtered.put(Downloads.Impl.COLUMN_LAST_MODIFICATION, mHost.currentTimeMillis());

        String pckg = values.getAsString(Downloads.Impl.COLUMN_NOTIFICATION_PACKAGE);
        String clazz = values.getAsString(Downloads.Impl.COLUMN_NOTIFICATION_CLASS);
        if (pckg != null && (clazz != null || isPublicApi)) {
            int uid = caller.uid();
            if (uid == 0 || mHost.uidOwnsPackage(uid, pckg)) {
                filtered.put(Downloads.Impl.COLUMN_NOTIFICATION_PACKAGE, pckg);
                if (clazz != null) filtered.put(Downloads.Impl.COLUMN_NOTIFICATION_CLASS, clazz);
            }
        }
        copyString(Downloads.Impl.COLUMN_NOTIFICATION_EXTRAS, values, filtered);
        copyString(Downloads.Impl.COLUMN_COOKIE_DATA, values, filtered);
        copyString(Downloads.Impl.COLUMN_USER_AGENT, values, filtered);
        copyString(Downloads.Impl.COLUMN_REFERER, values, filtered);
        if (caller.holds(Downloads.Impl.PERMISSION_ACCESS_ADVANCED)) {
            copyInteger(Downloads.Impl.COLUMN_OTHER_UID, values, filtered);
        }
        filtered.put(UID, caller.uid());
        if (caller.uid() == 0) copyInteger(UID, values, filtered);
        copyStringWithDefault(Downloads.Impl.COLUMN_TITLE, values, filtered, "");
        copyStringWithDefault(Downloads.Impl.COLUMN_DESCRIPTION, values, filtered, "");
        copyBoolean(Downloads.Impl.COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI, values, filtered);
        if (isPublicApi) {
            copyInteger(Downloads.Impl.COLUMN_ALLOWED_NETWORK_TYPES, values, filtered);
            copyBoolean(Downloads.Impl.COLUMN_ALLOW_ROAMING, values, filtered);
            copyBoolean(Downloads.Impl.COLUMN_ALLOW_METERED, values, filtered);
            copyInteger(Downloads.Impl.COLUMN_FLAGS, values, filtered);
        }
        Integer mediaScanned = values.getAsInteger(Downloads.Impl.COLUMN_MEDIA_SCANNED);
        filtered.put(Downloads.Impl.COLUMN_MEDIA_SCANNED,
                mediaScanned == null ? Downloads.Impl.MEDIA_NOT_SCANNED : mediaScanned);
        // Scanning completed downloads into MediaStore is not connected yet, so
        // mediaprovider_uri/mediastore_uri stay null, as when Android's scan has no URI.

        long rowId;
        mDb.beginTransaction();
        try {
            rowId = mDb.insert(DB_TABLE, null, filtered);
            if (rowId == -1) return null;
            insertRequestHeaders(rowId, values);
            mDb.setTransactionSuccessful();
        } finally {
            mDb.endTransaction();
        }
        String callingPackage = caller.packageName();
        if (callingPackage == null) return null;
        mHost.grantAllDownloads(callingPackage, rowId);
        notifyContentChanged(uri, match);
        mHost.schedule(rowId);
        return ContentUris.withAppendedId(Downloads.Impl.CONTENT_URI, rowId);
    }

    private void insertRequestHeaders(long downloadId, ContentValues values) {
        ContentValues row = new ContentValues();
        row.put(Downloads.Impl.RequestHeaders.COLUMN_DOWNLOAD_ID, downloadId);
        for (Map.Entry<String, Object> entry : values.valueSet()) {
            if (!entry.getKey().startsWith(Downloads.Impl.RequestHeaders.INSERT_KEY_PREFIX)) continue;
            String line = String.valueOf(entry.getValue());
            if (!line.contains(":")) throw new IllegalArgumentException("Invalid HTTP header line: " + line);
            String[] parts = line.split(":", 2);
            row.put(Downloads.Impl.RequestHeaders.COLUMN_HEADER, parts[0].trim());
            row.put(Downloads.Impl.RequestHeaders.COLUMN_VALUE, parts[1].trim());
            mDb.insert(HEADERS_TABLE, null, row);
        }
    }

    private void ensureDefaultColumns(ContentValues values) {
        Integer dest = values.getAsInteger(Downloads.Impl.COLUMN_DESTINATION);
        if (dest == null) {
            values.put(Downloads.Impl.COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI, true);
            return;
        }
        int mediaScannable;
        boolean visible;
        if (dest == Downloads.Impl.DESTINATION_EXTERNAL) {
            mediaScannable = Downloads.Impl.MEDIA_NOT_SCANNED;
            visible = true;
        } else if (dest != Downloads.Impl.DESTINATION_FILE_URI
                && dest != Downloads.Impl.DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD) {
            mediaScannable = Downloads.Impl.MEDIA_NOT_SCANNABLE;
            visible = false;
        } else {
            File file = dest == Downloads.Impl.DESTINATION_FILE_URI
                    ? new File(Uri.parse(values.getAsString(Downloads.Impl.COLUMN_FILE_NAME_HINT)).getPath())
                    : new File(values.getAsString(Downloads.Impl._DATA));
            if (isFileInExternalAndroidDirs(file)) {
                mediaScannable = Downloads.Impl.MEDIA_NOT_SCANNABLE;
                visible = false;
            } else if (isFilenameValidInPublicDownloadsDir(file)) {
                mediaScannable = Downloads.Impl.MEDIA_NOT_SCANNED;
                visible = true;
            } else {
                mediaScannable = Downloads.Impl.MEDIA_NOT_SCANNED;
                visible = false;
            }
        }
        values.put(Downloads.Impl.COLUMN_MEDIA_SCANNED, mediaScannable);
        values.put(Downloads.Impl.COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI, visible);
    }

    private void checkFileUriDestination(Caller caller, ContentValues values) {
        String fileUri = values.getAsString(Downloads.Impl.COLUMN_FILE_NAME_HINT);
        if (fileUri == null) {
            throw new IllegalArgumentException(
                    "DESTINATION_FILE_URI must include a file URI under COLUMN_FILE_NAME_HINT");
        }
        Uri uri = Uri.parse(fileUri);
        if (!"file".equals(uri.getScheme())) throw new IllegalArgumentException("Not a file URI: " + null);
        String path = uri.getPath();
        if (path == null || hasParentReference(path)) {
            throw new IllegalArgumentException("Invalid file URI: " + uri);
        }
        File file;
        try {
            file = new File(path).getCanonicalFile();
            values.put(Downloads.Impl.COLUMN_FILE_NAME_HINT, Uri.fromFile(file).toString());
        } catch (IOException e) {
            throw new SecurityException(e);
        }
        checkDestinationFilePathRestrictions(caller, file, false);
    }

    private void checkDownloadedFilePath(Caller caller, ContentValues values) {
        String path = values.getAsString(Downloads.Impl._DATA);
        if (path == null || hasParentReference(path)) {
            throw new IllegalArgumentException("Invalid file path: " + (path == null ? "null" : path));
        }
        File file;
        try {
            file = new File(path).getCanonicalFile();
            values.put(Downloads.Impl._DATA, file.getPath());
        } catch (IOException e) {
            throw new SecurityException(e);
        }
        if (!file.exists()) throw new IllegalArgumentException("File doesn't exist: " + file);
        if (caller.local()) return;
        checkDestinationFilePathRestrictions(caller, file, true);
        SharedStorageOwnership.enforceOwner(file,
                owner -> mHost.uidOwnsPackage(caller.uid(), owner));
    }

    /** Helpers.checkDestinationFilePathRestrictions. */
    private void checkDestinationFilePathRestrictions(Caller caller, File file,
            boolean allowDownloadsDirOnly) {
        boolean validPublic = allowDownloadsDirOnly ? isFilenameValidInPublicDownloadsDir(file)
                : isFilenameValidInKnownPublicDir(file);
        if (isFilenameValidInExternalPackage(file, caller.packageName()) || validPublic) return;
        if (isFilenameValidInExternalObbDir(file) && caller.installer()) return;
        if (isFileInPrivateExternalAndroidDirs(file)) throw new SecurityException("Unsupported path " + file);
        if (caller.legacyStorage() && within(externalRoot(), file)) {
            if (!caller.writeExternalStorage()) {
                throw new SecurityException("No permission to write to " + file);
            }
            return;
        }
        throw new SecurityException("Unsupported path " + file);
    }

    /** DownloadProvider.checkInsertPermissions. */
    private void checkInsertPermissions(Caller caller, ContentValues values) {
        if (caller.holds(Downloads.Impl.PERMISSION_ACCESS)) return;
        if (!caller.holds(android.Manifest.permission.INTERNET)) {
            throw new SecurityException("INTERNET permission is required to use the download manager");
        }
        values = new ContentValues(values);
        enforceAllowedValues(values, Downloads.Impl.COLUMN_IS_PUBLIC_API, Boolean.TRUE);
        if (Objects.equals(values.getAsInteger(Downloads.Impl.COLUMN_DESTINATION),
                Downloads.Impl.DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD)) {
            values.remove(Downloads.Impl.COLUMN_TOTAL_BYTES);
            values.remove(Downloads.Impl._DATA);
            values.remove(Downloads.Impl.COLUMN_STATUS);
        }
        enforceAllowedValues(values, Downloads.Impl.COLUMN_DESTINATION,
                Downloads.Impl.DESTINATION_CACHE_PARTITION_PURGEABLE,
                Downloads.Impl.DESTINATION_FILE_URI,
                Downloads.Impl.DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD);
        if (caller.holds(Downloads.Impl.PERMISSION_NO_NOTIFICATION)) {
            enforceAllowedValues(values, Downloads.Impl.COLUMN_VISIBILITY,
                    DownloadManager.Request.VISIBILITY_HIDDEN,
                    DownloadManager.Request.VISIBILITY_VISIBLE,
                    DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED,
                    DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_ONLY_COMPLETION);
        } else {
            enforceAllowedValues(values, Downloads.Impl.COLUMN_VISIBILITY,
                    DownloadManager.Request.VISIBILITY_VISIBLE,
                    DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED,
                    DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_ONLY_COMPLETION);
        }
        for (String allowed : new String[] {Downloads.Impl.COLUMN_URI, Downloads.Impl.COLUMN_TITLE,
                Downloads.Impl.COLUMN_DESCRIPTION, Downloads.Impl.COLUMN_MIME_TYPE,
                Downloads.Impl.COLUMN_FILE_NAME_HINT, Downloads.Impl.COLUMN_NOTIFICATION_PACKAGE,
                Downloads.Impl.COLUMN_ALLOWED_NETWORK_TYPES, Downloads.Impl.COLUMN_ALLOW_ROAMING,
                Downloads.Impl.COLUMN_ALLOW_METERED, Downloads.Impl.COLUMN_FLAGS,
                Downloads.Impl.COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI, Downloads.Impl.COLUMN_MEDIA_SCANNED,
                Downloads.Impl.COLUMN_ALLOW_WRITE}) {
            values.remove(allowed);
        }
        Iterator<Map.Entry<String, Object>> iterator = values.valueSet().iterator();
        List<String> headers = new ArrayList<>();
        while (iterator.hasNext()) {
            String key = iterator.next().getKey();
            if (key.startsWith(Downloads.Impl.RequestHeaders.INSERT_KEY_PREFIX)) headers.add(key);
        }
        for (String header : headers) values.remove(header);
        if (values.size() > 0) {
            StringBuilder error = new StringBuilder("Invalid columns in request: ");
            boolean first = true;
            for (Map.Entry<String, Object> entry : values.valueSet()) {
                if (!first) error.append(", ");
                error.append(entry.getKey());
                first = false;
            }
            throw new SecurityException(error.toString());
        }
    }

    private static void enforceAllowedValues(ContentValues values, String column, Object... allowed) {
        Object value = values.get(column);
        values.remove(column);
        for (Object candidate : allowed) {
            if (value == null && candidate == null) return;
            if (value != null && value.equals(candidate)) return;
        }
        throw new SecurityException("Invalid value for " + column + ": " + value);
    }

    /** DownloadProvider.getType. */
    String getType(Uri uri) {
        switch (match(uri)) {
            case MY_DOWNLOADS: case ALL_DOWNLOADS:
                return "vnd.android.cursor.dir/download";
            case MY_DOWNLOADS_ID: case ALL_DOWNLOADS_ID: {
                try (Cursor c = mDb.rawQuery("SELECT " + Downloads.Impl.COLUMN_MIME_TYPE + " FROM "
                        + DB_TABLE + " WHERE " + Downloads.Impl._ID + " = ?",
                        new String[] {downloadId(uri)})) {
                    String mimeType = c.moveToFirst() && !c.isNull(0) ? c.getString(0) : null;
                    return mimeType == null || mimeType.isEmpty()
                            ? "vnd.android.cursor.item/download" : mimeType;
                }
            }
            default:
                throw new IllegalArgumentException("Unknown URI: " + uri);
        }
    }

    // ---- query / update / delete -----------------------------------------------------

    Cursor query(Caller caller, Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sort) {
        int match = match(uri);
        if (match == -1) throw new IllegalArgumentException("Unknown URI: " + uri);
        if (match == MY_DOWNLOADS_ID_HEADERS || match == ALL_DOWNLOADS_ID_HEADERS) {
            if (projection != null || selection != null || sort != null) {
                throw new UnsupportedOperationException("Request header queries do not support "
                        + "projections, selections or sorting");
            }
            if (!caller.holds(Downloads.Impl.PERMISSION_ACCESS_ALL)) {
                throw new SecurityException("DownloadManager: requires "
                        + Downloads.Impl.PERMISSION_ACCESS_ALL);
            }
            projection = new String[] {Downloads.Impl.RequestHeaders.COLUMN_HEADER,
                    Downloads.Impl.RequestHeaders.COLUMN_VALUE};
            return queryBuilder(caller, uri, match).query(mDb, projection, null, null, null, null, null);
        }
        return queryBuilder(caller, uri, match)
                .query(mDb, projection, selection, selectionArgs, null, null, sort);
    }

    int update(Caller caller, Uri uri, ContentValues values, String where, String[] whereArgs) {
        // The queue writes transfer state through writeState; every provider caller
        // gets DownloadProvider's external column filter.
        ContentValues filtered = new ContentValues();
        boolean updateSchedule = false;
        copyString(Downloads.Impl.COLUMN_APP_DATA, values, filtered);
        copyInteger(Downloads.Impl.COLUMN_VISIBILITY, values, filtered);
        Integer control = values.getAsInteger(Downloads.Impl.COLUMN_CONTROL);
        if (control != null) {
            filtered.put(Downloads.Impl.COLUMN_CONTROL, control);
            updateSchedule = true;
        }
        copyString(Downloads.Impl.COLUMN_TITLE, values, filtered);
        copyString(Downloads.Impl.COLUMN_MEDIAPROVIDER_URI, values, filtered);
        copyString(Downloads.Impl.COLUMN_DESCRIPTION, values, filtered);
        copyInteger(Downloads.Impl.COLUMN_DELETED, values, filtered);

        int match = match(uri);
        switch (match) {
            case MY_DOWNLOADS: case MY_DOWNLOADS_ID: case ALL_DOWNLOADS: case ALL_DOWNLOADS_ID:
                break;
            default:
                throw new UnsupportedOperationException("Cannot update URI: " + uri);
        }
        if (filtered.size() == 0) {
            notifyContentChanged(uri, match);
            return 0;
        }
        SQLiteQueryBuilder qb = queryBuilder(caller, uri, match);
        List<Long> ids = new ArrayList<>();
        int count;
        mDb.beginTransaction();
        try {
            count = qb.update(mDb, filtered, where, whereArgs);
            if (updateSchedule || filtered.containsKey(Downloads.Impl.COLUMN_DELETED)) {
                try (Cursor cursor = qb.query(mDb, new String[] {Downloads.Impl._ID},
                        where, whereArgs, null, null, null)) {
                    while (cursor.moveToNext()) ids.add(cursor.getLong(0));
                }
            }
            mDb.setTransactionSuccessful();
        } finally {
            mDb.endTransaction();
        }
        for (long id : ids) mHost.schedule(id);
        notifyContentChanged(uri, match);
        return count;
    }

    int delete(Caller caller, Uri uri, String where, String[] whereArgs) {
        int match = match(uri);
        switch (match) {
            case MY_DOWNLOADS: case MY_DOWNLOADS_ID: case ALL_DOWNLOADS: case ALL_DOWNLOADS_ID:
                break;
            default:
                throw new UnsupportedOperationException("Cannot delete URI: " + uri);
        }
        SQLiteQueryBuilder qb = queryBuilder(caller, uri, match);
        List<Info> removed = new ArrayList<>();
        int count;
        mDb.beginTransaction();
        try {
            try (Cursor cursor = qb.query(mDb, null, where, whereArgs, null, null, null)) {
                while (cursor.moveToNext()) removed.add(Info.read(cursor));
            }
            for (Info info : removed) {
                mDb.delete(HEADERS_TABLE, Downloads.Impl.RequestHeaders.COLUMN_DOWNLOAD_ID + "=?",
                        new String[] {Long.toString(info.id)});
            }
            count = qb.delete(mDb, where, whereArgs);
            mDb.setTransactionSuccessful();
        } finally {
            mDb.endTransaction();
        }
        for (Info info : removed) {
            mHost.cancel(info.id);
            mHost.revokeAllDownloads(info.id);
            if (info.fileName != null && !info.fileName.isEmpty()) {
                try {
                    File file = new File(info.fileName).getCanonicalFile();
                    if (isFilenameValid(file, true)) file.delete();
                } catch (IOException ignored) {
                    // AOSP logs and keeps the row's deletion.
                }
            }
            if (!Downloads.Impl.isStatusCompleted(info.status)) sendIntentIfRequested(info);
        }
        notifyContentChanged(uri, match);
        return count;
    }

    /** The checked file behind openFile, with the caller's access enforced first. */
    static final class OpenTarget {
        final File file;
        final boolean shouldScan;
        OpenTarget(File file, boolean shouldScan) { this.file = file; this.shouldScan = shouldScan; }
    }

    OpenTarget openFileTarget(Caller caller, Uri uri) throws FileNotFoundException {
        try (Cursor probe = query(caller, uri, new String[] {Downloads.Impl._DATA}, null, null, null)) {
            if (probe == null || probe.getCount() == 0) {
                throw new FileNotFoundException("No file found for " + uri + " as UID " + caller.uid());
            }
        }
        String path;
        boolean shouldScan;
        Caller cleared = system();
        try (Cursor cursor = query(cleared, uri, new String[] {Downloads.Impl._DATA,
                Downloads.Impl.COLUMN_STATUS, Downloads.Impl.COLUMN_DESTINATION,
                Downloads.Impl.COLUMN_MEDIA_SCANNED}, null, null, null)) {
            int count = cursor == null ? 0 : cursor.getCount();
            if (count != 1) {
                if (count == 0) throw new FileNotFoundException("No entry for " + uri);
                throw new FileNotFoundException("Multiple items at " + uri);
            }
            if (!cursor.moveToFirst()) throw new FileNotFoundException("Failed moveToFirst");
            int status = cursor.getInt(1);
            int destination = cursor.getInt(2);
            int mediaScanned = cursor.getInt(3);
            path = cursor.getString(0);
            shouldScan = Downloads.Impl.isStatusSuccess(status)
                    && (destination == Downloads.Impl.DESTINATION_EXTERNAL
                            || destination == Downloads.Impl.DESTINATION_FILE_URI
                            || destination == Downloads.Impl.DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD)
                    && mediaScanned != Downloads.Impl.MEDIA_NOT_SCANNABLE;
        }
        if (path == null) throw new FileNotFoundException("No filename found.");
        File file;
        try {
            file = new File(path).getCanonicalFile();
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
        if (!isFilenameValid(file, true)) throw new FileNotFoundException("Invalid file: " + file);
        return new OpenTarget(file, shouldScan);
    }

    /** The provider's own identity, as Binder.clearCallingIdentity gives AOSP. */
    static Caller system() {
        return new Caller() {
            @Override public int uid() { return 1000; }
            @Override public String packageName() { return "android"; }
            @Override public boolean holds(String permission) { return true; }
            @Override public boolean local() { return true; }
            @Override public boolean legacyStorage() { return true; }
            @Override public boolean writeExternalStorage() { return true; }
            @Override public boolean installer() { return true; }
        };
    }

    /** DownloadProvider.getQueryBuilder. */
    private SQLiteQueryBuilder queryBuilder(Caller caller, Uri uri, int match) {
        String table;
        Map<String, String> projectionMap;
        StringBuilder where = new StringBuilder();
        switch (match) {
            case MY_DOWNLOADS_ID:
                appendWhereExpression(where, Downloads.Impl._ID + "=" + downloadId(uri));
                // fall through
            case MY_DOWNLOADS:
                table = DB_TABLE;
                projectionMap = sDownloadsMap;
                if (!caller.holds(Downloads.Impl.PERMISSION_ACCESS_ALL)) {
                    appendWhereExpression(where, UID + "=" + caller.uid() + " OR "
                            + Downloads.Impl.COLUMN_OTHER_UID + "=" + caller.uid());
                }
                break;
            case ALL_DOWNLOADS_ID:
                appendWhereExpression(where, Downloads.Impl._ID + "=" + downloadId(uri));
                // fall through
            case ALL_DOWNLOADS:
                // Limited by the provider's path-permission and URI grants (DownloadProvider).
                table = DB_TABLE;
                projectionMap = sDownloadsMap;
                break;
            case MY_DOWNLOADS_ID_HEADERS:
            case ALL_DOWNLOADS_ID_HEADERS:
                table = HEADERS_TABLE;
                projectionMap = sHeadersMap;
                appendWhereExpression(where, Downloads.Impl.RequestHeaders.COLUMN_DOWNLOAD_ID + "="
                        + downloadId(uri));
                break;
            default:
                throw new UnsupportedOperationException("Unknown URI: " + uri);
        }
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables(table);
        qb.setProjectionMap(projectionMap);
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        qb.appendWhere(where);
        return qb;
    }

    private static void appendWhereExpression(StringBuilder sb, String expression) {
        if (sb.length() > 0) sb.append(" AND ");
        sb.append('(').append(expression).append(')');
    }

    private void notifyContentChanged(Uri uri, int match) {
        Long id = match == MY_DOWNLOADS_ID || match == ALL_DOWNLOADS_ID
                ? Long.parseLong(downloadId(uri)) : null;
        for (Uri base : new Uri[] {Downloads.Impl.CONTENT_URI, Downloads.Impl.ALL_DOWNLOADS_CONTENT_URI}) {
            mHost.notifyChange(id == null ? base : ContentUris.withAppendedId(base, id));
        }
    }

    // ---- the queue's view of rows ----------------------------------------------------

    /** DownloadInfo: the columns the queue and transfer read. */
    static final class Info {
        long id;
        String uri;
        String hint;
        String fileName;
        String mimeType;
        int destination;
        int visibility;
        int control;
        int status;
        int numFailed;
        int retryAfter;
        long lastMod;
        String packageName;
        String className;
        String extras;
        String userAgent;
        String cookies;
        String referer;
        long totalBytes;
        long currentBytes;
        String etag;
        int uid;
        boolean deleted;
        boolean isPublicApi;
        int allowedNetworkTypes;
        boolean allowRoaming;
        boolean allowMetered;
        int flags;
        int bypassRecommendedSizeLimit;

        static Info read(Cursor c) {
            Info info = new Info();
            info.id = c.getLong(c.getColumnIndexOrThrow(Downloads.Impl._ID));
            info.uri = string(c, Downloads.Impl.COLUMN_URI);
            info.hint = string(c, Downloads.Impl.COLUMN_FILE_NAME_HINT);
            info.fileName = string(c, Downloads.Impl._DATA);
            info.mimeType = string(c, Downloads.Impl.COLUMN_MIME_TYPE);
            info.destination = integer(c, Downloads.Impl.COLUMN_DESTINATION);
            info.visibility = integer(c, Downloads.Impl.COLUMN_VISIBILITY);
            info.control = integer(c, Downloads.Impl.COLUMN_CONTROL);
            info.status = integer(c, Downloads.Impl.COLUMN_STATUS);
            info.numFailed = integer(c, Downloads.Impl.COLUMN_FAILED_CONNECTIONS);
            info.retryAfter = integer(c, RETRY_AFTER) & 0xfffffff;
            info.lastMod = c.getLong(c.getColumnIndexOrThrow(Downloads.Impl.COLUMN_LAST_MODIFICATION));
            info.packageName = string(c, Downloads.Impl.COLUMN_NOTIFICATION_PACKAGE);
            info.className = string(c, Downloads.Impl.COLUMN_NOTIFICATION_CLASS);
            info.extras = string(c, Downloads.Impl.COLUMN_NOTIFICATION_EXTRAS);
            info.cookies = string(c, Downloads.Impl.COLUMN_COOKIE_DATA);
            info.userAgent = string(c, Downloads.Impl.COLUMN_USER_AGENT);
            info.referer = string(c, Downloads.Impl.COLUMN_REFERER);
            info.totalBytes = c.getLong(c.getColumnIndexOrThrow(Downloads.Impl.COLUMN_TOTAL_BYTES));
            info.currentBytes = c.getLong(c.getColumnIndexOrThrow(Downloads.Impl.COLUMN_CURRENT_BYTES));
            info.etag = string(c, ETAG);
            info.uid = integer(c, UID);
            info.deleted = integer(c, Downloads.Impl.COLUMN_DELETED) == 1;
            info.isPublicApi = integer(c, Downloads.Impl.COLUMN_IS_PUBLIC_API) != 0;
            info.allowedNetworkTypes = integer(c, Downloads.Impl.COLUMN_ALLOWED_NETWORK_TYPES);
            info.allowRoaming = integer(c, Downloads.Impl.COLUMN_ALLOW_ROAMING) != 0;
            info.allowMetered = integer(c, Downloads.Impl.COLUMN_ALLOW_METERED) != 0;
            info.flags = integer(c, Downloads.Impl.COLUMN_FLAGS);
            info.bypassRecommendedSizeLimit = integer(c,
                    Downloads.Impl.COLUMN_BYPASS_RECOMMENDED_SIZE_LIMIT);
            return info;
        }

        private static String string(Cursor c, String column) {
            int index = c.getColumnIndexOrThrow(column);
            return c.isNull(index) ? null : c.getString(index);
        }

        private static int integer(Cursor c, String column) {
            return c.getInt(c.getColumnIndexOrThrow(column));
        }

        /** DownloadInfo.isReadyToSchedule, without the removable-media case Gesher lacks. */
        boolean readyToSchedule() {
            if (deleted || control == Downloads.Impl.CONTROL_PAUSED) return false;
            switch (status) {
                case 0:
                case Downloads.Impl.STATUS_PENDING:
                case Downloads.Impl.STATUS_RUNNING:
                case Downloads.Impl.STATUS_WAITING_FOR_NETWORK:
                case Downloads.Impl.STATUS_WAITING_TO_RETRY:
                case Downloads.Impl.STATUS_QUEUED_FOR_WIFI:
                    return true;
                default:
                    return false;
            }
        }

        /** DownloadInfo.isRoamingAllowed. */
        boolean roamingAllowed() {
            return isPublicApi ? allowRoaming
                    : destination != Downloads.Impl.DESTINATION_CACHE_PARTITION_NOROAMING;
        }
    }

    Info info(long id) {
        try (Cursor c = mDb.query(DB_TABLE, null, Downloads.Impl._ID + "=?",
                new String[] {Long.toString(id)}, null, null, null)) {
            return c.moveToFirst() ? Info.read(c) : null;
        }
    }

    List<Long> ids() {
        List<Long> ids = new ArrayList<>();
        try (Cursor c = mDb.query(DB_TABLE, new String[] {Downloads.Impl._ID}, null, null,
                null, null, Downloads.Impl._ID + " ASC")) {
            while (c.moveToNext()) ids.add(c.getLong(0));
        }
        return ids;
    }

    List<String[]> headers(long id) {
        List<String[]> headers = new ArrayList<>();
        try (Cursor c = mDb.query(HEADERS_TABLE, new String[] {
                Downloads.Impl.RequestHeaders.COLUMN_HEADER, Downloads.Impl.RequestHeaders.COLUMN_VALUE},
                Downloads.Impl.RequestHeaders.COLUMN_DOWNLOAD_ID + "=?",
                new String[] {Long.toString(id)}, null, null, "id ASC")) {
            while (c.moveToNext()) headers.add(new String[] {c.getString(0), c.getString(1)});
        }
        return headers;
    }

    /** DownloadInfo.getHeaders: the cookie and referer columns become request headers too. */
    List<String[]> requestHeaders(Info info) {
        List<String[]> headers = headers(info.id);
        if (info.cookies != null) headers.add(new String[] {"Cookie", info.cookies});
        if (info.referer != null) headers.add(new String[] {"Referer", info.referer});
        return headers;
    }

    /**
     * DownloadInfoDelta.writeToDatabase(OrThrow): the queue's write of the transfer's
     * fields. With {@code onlyIfValid}, nothing is written to a paused, canceled or
     * deleted row, and false is returned.
     */
    boolean writeState(long id, DownloadTransfer.State state, boolean onlyIfValid) {
        ContentValues values = new ContentValues();
        values.put(Downloads.Impl.COLUMN_URI, state.uri);
        values.put(Downloads.Impl._DATA, state.fileName);
        values.put(Downloads.Impl.COLUMN_MIME_TYPE, state.mimeType);
        values.put(Downloads.Impl.COLUMN_STATUS, state.status);
        values.put(Downloads.Impl.COLUMN_FAILED_CONNECTIONS, state.numFailed);
        values.put(RETRY_AFTER, state.retryAfter);
        values.put(Downloads.Impl.COLUMN_TOTAL_BYTES, state.totalBytes);
        values.put(Downloads.Impl.COLUMN_CURRENT_BYTES, state.currentBytes);
        values.put(ETAG, state.etag);
        values.put(Downloads.Impl.COLUMN_LAST_MODIFICATION, mHost.currentTimeMillis());
        values.put(Downloads.Impl.COLUMN_ERROR_MSG, state.errorMsg);
        String where = Downloads.Impl._ID + "=?";
        if (onlyIfValid) {
            where += " AND " + Downloads.Impl.COLUMN_STATUS + " != '" + Downloads.Impl.STATUS_CANCELED
                    + "' AND " + Downloads.Impl.COLUMN_DELETED + " == '0' AND ("
                    + Downloads.Impl.COLUMN_CONTROL + " IS NULL OR " + Downloads.Impl.COLUMN_CONTROL
                    + " != '" + Downloads.Impl.CONTROL_PAUSED + "')";
        }
        boolean written = mDb.update(DB_TABLE, values, where, new String[] {Long.toString(id)}) > 0;
        if (written) notifyContentChanged(ContentUris.withAppendedId(Downloads.Impl.ALL_DOWNLOADS_CONTENT_URI, id),
                ALL_DOWNLOADS_ID);
        return written;
    }

    /** A running download's status, written without touching the transfer fields. */
    void writeStatus(long id, int status) {
        ContentValues values = new ContentValues();
        values.put(Downloads.Impl.COLUMN_STATUS, status);
        values.put(Downloads.Impl.COLUMN_LAST_MODIFICATION, mHost.currentTimeMillis());
        if (mDb.update(DB_TABLE, values, Downloads.Impl._ID + "=?",
                new String[] {Long.toString(id)}) > 0) {
            notifyContentChanged(ContentUris.withAppendedId(Downloads.Impl.ALL_DOWNLOADS_CONTENT_URI, id),
                    ALL_DOWNLOADS_ID);
        }
    }

    /** A completed open-for-write updates the row's size (DownloadProvider.openFile). */
    void fileWritten(long id, long length) {
        ContentValues values = new ContentValues();
        values.put(Downloads.Impl.COLUMN_TOTAL_BYTES, length);
        values.put(Downloads.Impl.COLUMN_LAST_MODIFICATION, mHost.currentTimeMillis());
        mDb.update(DB_TABLE, values, Downloads.Impl._ID + "=?", new String[] {Long.toString(id)});
        notifyContentChanged(ContentUris.withAppendedId(Downloads.Impl.ALL_DOWNLOADS_CONTENT_URI, id),
                ALL_DOWNLOADS_ID);
    }

    /** Rows of UIDs with no installed package (Helpers.handleRemovedUidEntries). */
    int removeUid(int uid) {
        List<Long> ids = new ArrayList<>();
        try (Cursor c = mDb.query(DB_TABLE, new String[] {Downloads.Impl._ID}, UID + "=?",
                new String[] {Integer.toString(uid)}, null, null, null)) {
            while (c.moveToNext()) ids.add(c.getLong(0));
        }
        if (ids.isEmpty()) return 0;
        Caller system = system();
        int removed = 0;
        for (long id : ids) {
            removed += delete(system, ContentUris.withAppendedId(Downloads.Impl.ALL_DOWNLOADS_CONTENT_URI, id),
                    null, null);
        }
        // Rows that only name the UID as another owner lose that delegation.
        ContentValues clear = new ContentValues();
        clear.putNull(Downloads.Impl.COLUMN_OTHER_UID);
        mDb.update(DB_TABLE, clear, Downloads.Impl.COLUMN_OTHER_UID + "=?",
                new String[] {Integer.toString(uid)});
        return removed;
    }

    List<Integer> uids() {
        List<Integer> uids = new ArrayList<>();
        try (Cursor c = mDb.query(true, DB_TABLE, new String[] {UID}, null, null, null, null, null, null)) {
            while (c.moveToNext()) uids.add(c.getInt(0));
        }
        return uids;
    }

    /** DownloadInfo.sendIntentIfRequested. */
    void sendIntentIfRequested(Info info) {
        if (info.packageName == null) return;
        if (!info.isPublicApi && info.className == null) return;
        mHost.sendCompleted(info.packageName, info.className, info.isPublicApi, info.id, info.extras);
    }

    // ---- destinations ----------------------------------------------------------------

    File externalRoot() {
        try {
            return mHost.externalRoot().getCanonicalFile();
        } catch (IOException e) {
            return mHost.externalRoot().getAbsoluteFile();
        }
    }

    /** Running and success directories (Helpers.getDestinationDirectory). */
    File destinationDirectory(int destination, boolean running) throws IOException {
        switch (destination) {
            case Downloads.Impl.DESTINATION_CACHE_PARTITION:
            case Downloads.Impl.DESTINATION_CACHE_PARTITION_PURGEABLE:
            case Downloads.Impl.DESTINATION_CACHE_PARTITION_NOROAMING:
                return running ? mHost.filesDir() : mHost.cacheDir();
            case Downloads.Impl.DESTINATION_EXTERNAL: {
                File target = new File(externalRoot(), "Download");
                if (!target.isDirectory() && !target.mkdirs()) {
                    throw new IOException("unable to create external downloads directory");
                }
                return target;
            }
            default:
                throw new IllegalStateException("unexpected destination: " + destination);
        }
    }

    /** Helpers.generateSaveFile: chooses, and creates to reserve, the destination file. */
    File generateSaveFile(Info info, String url, String contentDisposition,
            String contentLocation, String mimeType) throws IOException {
        File parent;
        File[] parentTest;
        String name;
        if (info.destination == Downloads.Impl.DESTINATION_FILE_URI) {
            File file = new File(Uri.parse(info.hint).getPath());
            parent = file.getParentFile().getAbsoluteFile();
            parentTest = new File[] {parent};
            name = file.getName();
        } else {
            parent = destinationDirectory(info.destination, true);
            parentTest = new File[] {parent, destinationDirectory(info.destination, false)};
            name = chooseFilename(url, info.hint, contentDisposition, contentLocation);
        }
        for (File test : parentTest) {
            if (!(test.isDirectory() || test.mkdirs())) {
                throw new IOException("Failed to create parent for " + test);
            }
        }
        String prefix, suffix;
        int dotIndex = name.lastIndexOf('.');
        boolean missingExtension = dotIndex < 0;
        if (info.destination == Downloads.Impl.DESTINATION_FILE_URI) {
            prefix = missingExtension ? name : name.substring(0, dotIndex);
            suffix = missingExtension ? "" : name.substring(dotIndex);
        } else if (missingExtension) {
            prefix = name;
            suffix = chooseExtensionFromMimeType(mimeType, true);
        } else {
            prefix = name.substring(0, dotIndex);
            suffix = chooseExtensionFromFilename(mimeType, name, dotIndex);
        }
        synchronized (mUniqueLock) {
            name = generateAvailableFilename(parentTest, prefix, suffix);
            File file = new File(parent, name);
            // AOSP: "We're not paranoid enough to use O_EXCL"; creating it reserves the name.
            file.createNewFile();
            return file;
        }
    }

    private static final java.util.regex.Pattern CONTENT_DISPOSITION_PATTERN =
            java.util.regex.Pattern.compile("attachment;\\s*filename\\s*=\\s*\"([^\"]*)\"");

    static String chooseFilename(String url, String hint, String contentDisposition,
            String contentLocation) {
        String filename = null;
        if (hint != null && !hint.endsWith("/")) {
            int index = hint.lastIndexOf('/') + 1;
            filename = index > 0 ? hint.substring(index) : hint;
        }
        if (filename == null && contentDisposition != null) {
            java.util.regex.Matcher m = CONTENT_DISPOSITION_PATTERN.matcher(contentDisposition);
            if (m.find()) {
                filename = m.group(1);
                int index = filename.lastIndexOf('/') + 1;
                if (index > 0) filename = filename.substring(index);
            }
        }
        if (filename == null && contentLocation != null) {
            String decoded = Uri.decode(contentLocation);
            if (decoded != null && !decoded.endsWith("/") && decoded.indexOf('?') < 0) {
                int index = decoded.lastIndexOf('/') + 1;
                filename = index > 0 ? decoded.substring(index) : decoded;
            }
        }
        if (filename == null) {
            String decoded = Uri.decode(url);
            if (decoded != null && !decoded.endsWith("/") && decoded.indexOf('?') < 0) {
                int index = decoded.lastIndexOf('/') + 1;
                if (index > 0) filename = decoded.substring(index);
            }
        }
        if (filename == null) filename = DEFAULT_DL_FILENAME;
        return android.os.FileUtils.buildValidFatFilename(filename);
    }

    private static String chooseExtensionFromMimeType(String mimeType, boolean useDefaults) {
        String extension = null;
        if (mimeType != null) {
            extension = android.webkit.MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType);
            if (extension != null) extension = "." + extension;
        }
        if (extension == null) {
            if (mimeType != null && mimeType.toLowerCase(Locale.ROOT).startsWith("text/")) {
                if (mimeType.equalsIgnoreCase("text/html")) extension = ".html";
                else if (useDefaults) extension = ".txt";
            } else if (useDefaults) {
                extension = ".bin";
            }
        }
        return extension;
    }

    private static String chooseExtensionFromFilename(String mimeType, String filename, int lastDot) {
        String extension = null;
        if (mimeType != null) {
            String typeFromExt = android.webkit.MimeTypeMap.getSingleton()
                    .getMimeTypeFromExtension(filename.substring(lastDot + 1));
            if (typeFromExt == null || !typeFromExt.equalsIgnoreCase(mimeType)) {
                extension = chooseExtensionFromMimeType(mimeType, false);
            }
        }
        if (extension == null) extension = filename.substring(lastDot);
        return extension;
    }

    private static boolean isFilenameAvailable(File[] parents, String name) {
        if (RECOVERY_DIRECTORY.equalsIgnoreCase(name)) return false;
        for (File parent : parents) {
            if (new File(parent, name).exists()) return false;
        }
        return true;
    }

    /** Helpers.generateAvailableFilenameLocked: [base]-[sequence].[ext] on collision. */
    private String generateAvailableFilename(File[] parents, String prefix, String suffix)
            throws IOException {
        String name = removeInvalidCharsAndGenerateName(prefix + suffix);
        if (isFilenameAvailable(parents, name)) return name;
        int sequence = 1;
        for (int magnitude = 1; magnitude < 1000000000; magnitude *= 10) {
            for (int iteration = 0; iteration < 9; ++iteration) {
                name = removeInvalidCharsAndGenerateName(prefix + "-" + sequence + suffix);
                if (isFilenameAvailable(parents, name)) return name;
                sequence += mRandom.nextInt(magnitude) + 1;
            }
        }
        throw new IOException("Failed to generate an available filename");
    }

    private String removeInvalidCharsAndGenerateName(String name) {
        String value = name.replaceAll("[*/:<>?\\\\|]", "_");
        if (value.replaceAll("_", "").trim().isEmpty()) {
            value = DEFAULT_DOWNLOAD_FILE_NAME_PREFIX + mHost.currentTimeMillis();
        }
        return value;
    }

    // ---- path rules (Helpers), against Gesher's shared volume -----------------------

    /**
     * AOSP's {@code ("/" + path + "/").contains("/../")}, with both separators: Gesher's
     * file layer takes a backslash as a separator too (docs/decisions/0010-paths.md), and a
     * Windows JVM does.
     */
    static boolean hasParentReference(String path) {
        for (String segment : path.split("[/\\\\]", -1)) {
            if (segment.equals("..")) return true;
        }
        return false;
    }

    /**
     * FileUtils.contains on canonical files, by whole path components rather than a
     * {@code "/"} prefix, so it holds for any separator the platform canonicalizes to.
     */
    static boolean within(File dir, File file) {
        File container = dir.getAbsoluteFile();
        for (File f = file.getAbsoluteFile(); f != null; f = f.getParentFile()) {
            if (f.equals(container)) return true;
        }
        return false;
    }

    private static boolean withinCanonical(File dir, File file) {
        try {
            return within(dir.getCanonicalFile(), file);
        } catch (IOException e) {
            return false;
        }
    }

    /**
     * The canonical path's components below the shared volume's root, or null when it is
     * not below it; components, not a {@code "/"}-separated string, for any separator.
     */
    private List<String> relative(File file) {
        File root = externalRoot();
        File canonical;
        try {
            canonical = file.getCanonicalFile();
        } catch (IOException e) {
            return null;
        }
        java.util.LinkedList<String> segments = new java.util.LinkedList<>();
        for (File f = canonical; f != null; f = f.getParentFile()) {
            if (f.equals(root)) return segments.isEmpty() ? null : segments;
            segments.addFirst(f.getName());
        }
        return null;
    }

    /** PATTERN_ANDROID_DIRS: <volume>/Android/(data|obb|media)/.+ (case-insensitive). */
    boolean isFileInExternalAndroidDirs(File file) {
        List<String> s = relative(file);
        return s != null && s.size() >= 3 && s.get(0).equalsIgnoreCase("Android")
                && (s.get(1).equalsIgnoreCase("data") || s.get(1).equalsIgnoreCase("obb")
                        || s.get(1).equalsIgnoreCase("media"));
    }

    /** PATTERN_ANDROID_PRIVATE_DIRS: <volume>/Android/(data|obb)/.+ (case-insensitive). */
    boolean isFileInPrivateExternalAndroidDirs(File file) {
        List<String> s = relative(file);
        return s != null && s.size() >= 3 && s.get(0).equalsIgnoreCase("Android")
                && (s.get(1).equalsIgnoreCase("data") || s.get(1).equalsIgnoreCase("obb"));
    }

    /** PATTERN_PUBLIC_DIRS with Environment.STANDARD_DIRECTORIES. */
    boolean isFilenameValidInKnownPublicDir(File file) {
        List<String> s = relative(file);
        if (s == null || s.size() < 2) return false;
        for (String dir : android.os.Environment.STANDARD_DIRECTORIES) {
            if (dir.equals(s.get(0))) return true;
        }
        return false;
    }

    boolean isFilenameValidInPublicDownloadsDir(File file) {
        return withinCanonical(new File(externalRoot(), android.os.Environment.DIRECTORY_DOWNLOADS), file);
    }

    boolean isFilenameValidInExternalPackage(File file, String packageName) {
        if (packageName == null) return false;
        File android = new File(externalRoot(), "Android");
        return withinCanonical(new File(android, "data/" + packageName), file)
                || withinCanonical(new File(android, "obb/" + packageName), file)
                || withinCanonical(new File(android, "media/" + packageName), file);
    }

    boolean isFilenameValidInExternalObbDir(File file) {
        return withinCanonical(new File(externalRoot(), "Android/obb"), file);
    }

    /** Helpers.isFilenameValid: the provider's own directories or the shared volume. */
    boolean isFilenameValid(File file, boolean allowInternal) {
        if (allowInternal && (withinCanonical(mHost.filesDir(), file)
                || withinCanonical(mHost.cacheDir(), file))) return true;
        return withinCanonical(externalRoot(), file);
    }

    // ---- ContentValues helpers (DownloadProvider) ------------------------------------

    private static void copyInteger(String key, ContentValues from, ContentValues to) {
        Integer i = from.getAsInteger(key);
        if (i != null) to.put(key, i);
    }

    private static void copyBoolean(String key, ContentValues from, ContentValues to) {
        Boolean b = from.getAsBoolean(key);
        if (b != null) to.put(key, b);
    }

    private static void copyString(String key, ContentValues from, ContentValues to) {
        String s = from.getAsString(key);
        if (s != null) to.put(key, s);
    }

    private static void copyStringWithDefault(String key, ContentValues from, ContentValues to,
            String dflt) {
        copyString(key, from, to);
        if (!to.containsKey(key)) to.put(key, dflt);
    }
}
