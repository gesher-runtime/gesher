/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
 *
 * Access rules, column rules, placement rules and MIME classification follow AOSP's
 * MediaProvider, AccessChecker, LocalCallingIdentity, PermissionUtils, FileUtils and
 * MimeUtils (packages/providers/MediaProvider, android-16.0.0_r4; Apache-2.0,
 * Copyright (C) The Android Open Source Project). docs/media-provider.md quotes them.
 */
package com.gesher.server;

import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.net.Uri;
import android.os.Environment;
import android.webkit.MimeTypeMap;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The MediaStore index of the primary shared volume ({@code external_primary}): AOSP's
 * {@code files} table in SQLite, the Images, Video, Audio, Downloads and Files
 * collections over it, scoped-storage ownership and permission filtering, the
 * pending-item lifecycle and the scanner for files written directly to the volume.
 * {@link MediaStoreProvider} is its Binder, permission and file-descriptor glue; this
 * class has no Binder or Windows dependency so that it runs unchanged on a host JVM
 * (tools/test_media_provider_contracts.py).
 */
final class MediaStoreIndex {
    static final String AUTHORITY = "media";
    static final String VOLUME_EXTERNAL = "external";
    static final String VOLUME_EXTERNAL_PRIMARY = "external_primary";
    static final String VOLUME_INTERNAL = "internal";

    // FileColumns.MEDIA_TYPE_*.
    static final int MEDIA_TYPE_NONE = 0, MEDIA_TYPE_IMAGE = 1, MEDIA_TYPE_AUDIO = 2,
            MEDIA_TYPE_VIDEO = 3, MEDIA_TYPE_PLAYLIST = 4, MEDIA_TYPE_SUBTITLE = 5,
            MEDIA_TYPE_DOCUMENT = 6;

    // MediaStore.MATCH_*.
    static final int MATCH_DEFAULT = 0, MATCH_INCLUDE = 1, MATCH_EXCLUDE = 2, MATCH_ONLY = 3;

    // LocalUriMatcher codes, as in AOSP.
    static final int IMAGES_MEDIA = 1, IMAGES_MEDIA_ID = 2, AUDIO_MEDIA = 100,
            AUDIO_MEDIA_ID = 101, VIDEO_MEDIA = 200, VIDEO_MEDIA_ID = 201, FILES = 700,
            FILES_ID = 701, DOWNLOADS = 800, DOWNLOADS_ID = 801;

    static final int DB_VERSION = 1602; // DatabaseHelper.VERSION_B, whose files schema this is
    /** FileUtils.DEFAULT_DURATION_PENDING / DEFAULT_DURATION_TRASHED / _EXTENDED. */
    static final long DEFAULT_DURATION_PENDING = 7 * 24 * 3600 * 1000L;
    static final long DEFAULT_DURATION_TRASHED = 30 * 24 * 3600 * 1000L;
    static final long DEFAULT_DURATION_EXTENDED = 7 * 24 * 3600 * 1000L;
    private static final int MAX_FILENAME_BYTES = 255;
    private static final String MIMETYPE_UNKNOWN = "application/octet-stream";
    private static final int TARGET_R = 30, TARGET_T = 33, TARGET_U = 34, TARGET_V = 35,
            TARGET_B = 36;

    /** FileUtils.PATTERN_INVISIBLE, relative to the volume. */
    static final Pattern PATTERN_INVISIBLE = Pattern.compile(
            "(?i)^(?:(?:Android/(?:data|obb|sandbox))|(?:\\.transforms)|(?:\\.picker_transcoded)"
                    + "|(?:(?:Movies|Music|Pictures)/.thumbnails))$");

    /** FileUtils.PATTERN_EXPIRES_FILE. */
    static final Pattern PATTERN_EXPIRES_FILE = Pattern.compile("(?i)^\\.(pending|trashed)-(\\d+)-([^/]+)$");

    static final String READ_EXTERNAL_STORAGE = "android.permission.READ_EXTERNAL_STORAGE";
    static final String WRITE_EXTERNAL_STORAGE = "android.permission.WRITE_EXTERNAL_STORAGE";
    static final String MANAGE_EXTERNAL_STORAGE = "android.permission.MANAGE_EXTERNAL_STORAGE";
    static final String READ_MEDIA_IMAGES = "android.permission.READ_MEDIA_IMAGES";
    static final String READ_MEDIA_VIDEO = "android.permission.READ_MEDIA_VIDEO";
    static final String READ_MEDIA_AUDIO = "android.permission.READ_MEDIA_AUDIO";
    static final String QUERY_ALL_PACKAGES = "android.permission.QUERY_ALL_PACKAGES";
    static final String ACCESS_MEDIA_OWNER_PACKAGE_NAME =
            "com.android.providers.media.permission.ACCESS_MEDIA_OWNER_PACKAGE_NAME";
    static final String OPSTR_READ_MEDIA_IMAGES = "android:read_media_images",
            OPSTR_WRITE_MEDIA_IMAGES = "android:write_media_images",
            OPSTR_READ_MEDIA_VIDEO = "android:read_media_video",
            OPSTR_WRITE_MEDIA_VIDEO = "android:write_media_video",
            OPSTR_READ_MEDIA_AUDIO = "android:read_media_audio",
            OPSTR_WRITE_MEDIA_AUDIO = "android:write_media_audio";
    static final int MODE_ALLOWED = 0;

    /** The application (or the system) a request comes from, as AOSP's LocalCallingIdentity. */
    interface Caller {
        /** The system server itself: MediaProvider's isCallingPackageSelf. */
        boolean self();
        String packageName();
        int uid();
        /** Every package sharing the caller's UID (getSharedPackagesAsString). */
        String[] sharedPackages();
        int targetSdk();
        /** PermissionChecker's answer: granted, and its app op (if any) allowed. */
        boolean permission(String permission);
        /** The app op's mode (AppOpsManager.MODE_*). */
        int opMode(String op);
        /** OPSTR_LEGACY_STORAGE is MODE_ALLOWED for this package. */
        boolean legacyStorageOp();
        /** A URI grant from another app covers {@code uri}. */
        boolean uriGranted(Uri uri, boolean write);
        /** Package visibility of an owner (owner_package_name filtering, API 34+). */
        boolean canSeePackage(String packageName);
    }

    /** What the index needs from the device. */
    interface Host {
        /** The external_primary volume's directory (Environment.getExternalStorageDirectory). */
        File volumeRoot();
        void notifyChange(Uri uri);
        long currentTimeMillis();
        /**
         * Directory for the write-ahead journal of file moves and deletes that precede a
         * database commit ({@link FileOperations}), next to the database; null for an
         * in-memory index, which keeps no state across a restart.
         */
        default File journalDirectory() { return null; }
    }

    /** Arguments of a query besides projection: the Bundle keys AOSP reads. */
    static final class QueryArgs {
        String selection;
        String[] selectionArgs;
        String sortOrder;
        String limit;
        int matchPending = MATCH_DEFAULT;
        int matchTrashed = MATCH_DEFAULT;
        int matchFavorite = MATCH_DEFAULT;
    }

    /** A row resolved for an open request, after the access check. */
    static final class FileTarget {
        final long id;
        final File file;
        final String mimeType;
        final int mediaType;
        FileTarget(long id, File file, String mimeType, int mediaType) {
            this.id = id; this.file = file; this.mimeType = mimeType; this.mediaType = mediaType;
        }
    }

    private final SQLiteDatabase mDb;
    private final Host mHost;
    private final Object mLock = new Object();

    MediaStoreIndex(SQLiteDatabase db, Host host) {
        mDb = db;
        mHost = host;
        synchronized (mLock) {
            int version = mDb.getVersion();
            if (version != DB_VERSION) {
                mDb.beginTransaction();
                try {
                    if (version != 0) {
                        // DatabaseHelper.downgradeDatabase / unknown schema: start over.
                        mDb.execSQL("DROP TABLE IF EXISTS files");
                        mDb.execSQL("DROP TABLE IF EXISTS local_metadata");
                        mDb.execSQL("DROP TABLE IF EXISTS gesher_metadata");
                    }
                    createSchema(mDb);
                    mDb.setVersion(DB_VERSION);
                    mDb.setTransactionSuccessful();
                } finally {
                    mDb.endTransaction();
                }
            }
            recoverFileOperations();
        }
    }

    /**
     * Trusted provider-to-provider ownership check for an existing canonical path.
     * This is deliberately separate from app queries: pending/trashed state and package
     * visibility must not hide an owner from another provider's authorization check.
     */
    String ownerOf(File canonicalFile) {
        synchronized (mLock) {
            try (Cursor cursor = mDb.rawQuery("SELECT owner_package_name FROM files WHERE _data=?",
                    new String[] {canonicalFile.getPath()})) {
                return cursor.moveToFirst() ? cursor.getString(0) : null;
            }
        }
    }

    /** DatabaseHelper.createLatestSchema: the files table as AOSP declares it. */
    private static void createSchema(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE local_metadata (generation INTEGER DEFAULT 0)");
        db.execSQL("INSERT INTO local_metadata VALUES (0)");
        // AOSP keeps the UUID in an xattr of the database file (getOrCreateUuid).
        db.execSQL("CREATE TABLE gesher_metadata (uuid TEXT)");
        db.execSQL("INSERT INTO gesher_metadata VALUES ('" + UUID.randomUUID() + "')");
        db.execSQL("CREATE TABLE files (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                + "_data TEXT UNIQUE COLLATE NOCASE,_size INTEGER,format INTEGER,parent INTEGER,"
                + "date_added INTEGER,date_modified INTEGER,mime_type TEXT,title TEXT,"
                + "description TEXT,_display_name TEXT,picasa_id TEXT,orientation INTEGER,"
                + "latitude DOUBLE,longitude DOUBLE,datetaken INTEGER,mini_thumb_magic INTEGER,"
                + "bucket_id TEXT,bucket_display_name TEXT,isprivate INTEGER,title_key TEXT,"
                + "artist_id INTEGER,album_id INTEGER,composer TEXT,track INTEGER,"
                + "year INTEGER CHECK(year!=0),is_ringtone INTEGER,is_music INTEGER,"
                + "is_alarm INTEGER,is_notification INTEGER,is_podcast INTEGER,album_artist TEXT,"
                + "duration INTEGER,bookmark INTEGER,artist TEXT,album TEXT,resolution TEXT,"
                + "tags TEXT,category TEXT,language TEXT,mini_thumb_data TEXT,name TEXT,"
                + "media_type INTEGER,old_id INTEGER,is_drm INTEGER,"
                + "width INTEGER, height INTEGER, title_resource_uri TEXT,"
                + "owner_package_name TEXT DEFAULT NULL,"
                + "color_standard INTEGER, color_transfer INTEGER, color_range INTEGER,"
                + "_hash BLOB DEFAULT NULL, is_pending INTEGER DEFAULT 0,"
                + "is_download INTEGER DEFAULT 0, download_uri TEXT DEFAULT NULL,"
                + "referer_uri TEXT DEFAULT NULL, is_audiobook INTEGER DEFAULT 0,"
                + "date_expires INTEGER DEFAULT NULL,is_trashed INTEGER DEFAULT 0,"
                + "group_id INTEGER DEFAULT NULL,primary_directory TEXT DEFAULT NULL,"
                + "secondary_directory TEXT DEFAULT NULL,document_id TEXT DEFAULT NULL,"
                + "instance_id TEXT DEFAULT NULL,original_document_id TEXT DEFAULT NULL,"
                + "relative_path TEXT DEFAULT NULL,volume_name TEXT DEFAULT NULL,"
                + "artist_key TEXT DEFAULT NULL,album_key TEXT DEFAULT NULL,"
                + "genre TEXT DEFAULT NULL,genre_key TEXT DEFAULT NULL,genre_id INTEGER,"
                + "author TEXT DEFAULT NULL, bitrate INTEGER DEFAULT NULL,"
                + "capture_framerate REAL DEFAULT NULL, cd_track_number TEXT DEFAULT NULL,"
                + "compilation INTEGER DEFAULT NULL, disc_number TEXT DEFAULT NULL,"
                + "is_favorite INTEGER DEFAULT 0, num_tracks INTEGER DEFAULT NULL,"
                + "writer TEXT DEFAULT NULL, exposure_time TEXT DEFAULT NULL,"
                + "f_number TEXT DEFAULT NULL, iso INTEGER DEFAULT NULL,"
                + "scene_capture_type INTEGER DEFAULT NULL, generation_added INTEGER DEFAULT 0,"
                + "generation_modified INTEGER DEFAULT 0, xmp BLOB DEFAULT NULL,"
                + "_transcode_status INTEGER DEFAULT 0, _video_codec_type TEXT DEFAULT NULL,"
                + "_modifier INTEGER DEFAULT 0, is_recording INTEGER DEFAULT 0,"
                + "redacted_uri_id TEXT DEFAULT NULL, _user_id INTEGER DEFAULT 0,"
                + " _special_format INTEGER DEFAULT NULL,"
                + "oem_metadata BLOB DEFAULT NULL,"
                + "inferred_media_date INTEGER,"
                + "bits_per_sample INTEGER DEFAULT NULL, samplerate INTEGER DEFAULT NULL,"
                + "inferred_date INTEGER)");
        db.execSQL("CREATE INDEX path_index ON files(_data)");
        db.execSQL("CREATE INDEX media_type_index ON files(media_type)");
        db.execSQL("CREATE INDEX owner_package_name_index ON files(owner_package_name)");
    }

    // ---- columns -----------------------------------------------------------------

    /** MediaColumns visible in every collection (MediaStore @Column declarations). */
    private static final String[] MEDIA_COLUMNS = {
            "_id", "_data", "_size", "_display_name", "title", "date_added", "date_modified",
            "datetaken", "mime_type", "width", "height", "orientation", "duration",
            "owner_package_name", "volume_name", "relative_path", "is_pending", "is_trashed",
            "is_favorite", "is_download", "date_expires", "bucket_id", "bucket_display_name",
            "generation_added", "generation_modified", "document_id", "instance_id",
            "original_document_id", "resolution", "artist", "album", "album_artist", "author",
            "composer", "genre", "compilation", "cd_track_number", "disc_number", "num_tracks",
            "writer", "year", "bitrate", "capture_framerate", "is_drm", "xmp", "inferred_date",
    };
    private static final String[] IMAGE_COLUMNS = {
            "description", "picasa_id", "isprivate", "latitude", "longitude",
            "mini_thumb_magic", "exposure_time", "f_number", "iso", "scene_capture_type",
    };
    private static final String[] VIDEO_COLUMNS = {
            "description", "isprivate", "latitude", "longitude", "mini_thumb_magic",
            "tags", "category", "language", "bookmark", "color_standard", "color_transfer",
            "color_range",
    };
    private static final String[] AUDIO_COLUMNS = {
            "title_key", "artist_id", "artist_key", "album_id", "album_key", "track",
            "bookmark", "is_music", "is_podcast", "is_ringtone", "is_alarm", "is_notification",
            "is_audiobook", "is_recording", "genre_id", "genre_key", "title_resource_uri",
            "bits_per_sample", "samplerate",
    };
    private static final String[] DOWNLOAD_COLUMNS = {"download_uri", "referer_uri"};
    private static final String[] FILE_COLUMNS = {"media_type", "parent", "format", "mime_type"};

    private static final Map<Integer, Set<String>> COLUMNS = new HashMap<>();
    static {
        COLUMNS.put(IMAGES_MEDIA, columns(MEDIA_COLUMNS, IMAGE_COLUMNS));
        COLUMNS.put(VIDEO_MEDIA, columns(MEDIA_COLUMNS, VIDEO_COLUMNS));
        COLUMNS.put(AUDIO_MEDIA, columns(MEDIA_COLUMNS, AUDIO_COLUMNS));
        COLUMNS.put(DOWNLOADS, columns(MEDIA_COLUMNS, DOWNLOAD_COLUMNS));
        COLUMNS.put(FILES, columns(MEDIA_COLUMNS, IMAGE_COLUMNS, VIDEO_COLUMNS, AUDIO_COLUMNS,
                DOWNLOAD_COLUMNS, FILE_COLUMNS));
    }

    private static Set<String> columns(String[]... groups) {
        Set<String> set = new LinkedHashSet<>();
        for (String[] group : groups) set.addAll(Arrays.asList(group));
        return Collections.unmodifiableSet(set);
    }

    /** MediaProvider.sMutableColumns. */
    private static final Set<String> MUTABLE = columns(new String[] {
            "_data", "relative_path", "_display_name", "is_pending", "is_trashed",
            "is_favorite", "owner_package_name", "bookmark", "tags", "category",
            "download_uri", "referer_uri", "mime_type", "media_type"});
    /** MediaProvider.sPlacementColumns. */
    private static final Set<String> PLACEMENT = columns(new String[] {
            "_data", "relative_path", "_display_name", "mime_type", "is_pending",
            "is_trashed", "date_expires"});
    /** Columns only the provider itself derives (never accepted from an app). */
    private static final Set<String> DERIVED = columns(new String[] {
            "_id", "volume_name", "bucket_id", "bucket_display_name", "date_added",
            "generation_added", "generation_modified", "is_download", "_size", "media_type",
            "format", "parent", "owner_package_name", "latitude", "longitude"});

    // ---- URIs ------------------------------------------------------------------------

    /** The LocalUriMatcher code for {@code uri}, or -1. */
    static int match(Uri uri) {
        if (uri == null || !AUTHORITY.equals(uri.getAuthority())) return -1;
        List<String> s = uri.getPathSegments();
        if (s.size() < 2) return -1;
        boolean id = false;
        int base;
        switch (s.get(1)) {
            case "images": case "video": case "audio":
                if (s.size() < 3 || !"media".equals(s.get(2)) || s.size() > 4) return -1;
                base = "images".equals(s.get(1)) ? IMAGES_MEDIA
                        : "video".equals(s.get(1)) ? VIDEO_MEDIA : AUDIO_MEDIA;
                id = s.size() == 4;
                if (id && !isNumber(s.get(3))) return -1;
                break;
            case "file": case "downloads":
                if (s.size() > 3) return -1;
                base = "file".equals(s.get(1)) ? FILES : DOWNLOADS;
                id = s.size() == 3;
                if (id && !isNumber(s.get(2))) return -1;
                break;
            default:
                return -1;
        }
        return id ? base + 1 : base;
    }

    private static boolean isNumber(String text) {
        if (text.isEmpty() || text.length() > 18) return false;
        for (int i = 0; i < text.length(); i++) if (!Character.isDigit(text.charAt(i))) return false;
        return true;
    }

    private static int collection(int match) {
        return (match == IMAGES_MEDIA_ID || match == VIDEO_MEDIA_ID || match == AUDIO_MEDIA_ID
                || match == FILES_ID || match == DOWNLOADS_ID) ? match - 1 : match;
    }

    private static boolean isItem(int match) { return collection(match) != match; }

    private static String segmentFor(int collection) {
        switch (collection) {
            case IMAGES_MEDIA: return "images/media";
            case VIDEO_MEDIA: return "video/media";
            case AUDIO_MEDIA: return "audio/media";
            case DOWNLOADS: return "downloads";
            default: return "file";
        }
    }

    static Uri contentUri(String volume, int collection) {
        return Uri.parse("content://" + AUTHORITY + "/" + volume + "/" + segmentFor(collection));
    }

    static Uri itemUri(String volume, int collection, long id) {
        return ContentUris.withAppendedId(contentUri(volume, collection), id);
    }

    /** MediaProvider.getVolumeName / VolumeNotFoundException. */
    static String volume(Uri uri) {
        List<String> segments = uri.getPathSegments();
        String volume = segments.isEmpty() ? null : segments.get(0);
        if (VOLUME_EXTERNAL.equals(volume) || VOLUME_EXTERNAL_PRIMARY.equals(volume)) return volume;
        // Gesher indexes only the primary shared volume; "internal" (system sounds) and
        // any other name are not attached, as an unmounted volume is on Android.
        throw new VolumeNotFoundException(volume);
    }

    /** MediaProvider.VolumeNotFoundException: IllegalArgumentException from API 29 on. */
    static final class VolumeNotFoundException extends IllegalArgumentException {
        VolumeNotFoundException(String volume) { super("Volume " + volume + " not found"); }
    }

    private static Uri requireMatch(Uri uri, int match) {
        if (match < 0) throw new UnsupportedOperationException("Unknown or unsupported URL: " + uri);
        return uri;
    }

    // ---- access (AccessChecker, LocalCallingIdentity, PermissionUtils) ------------

    /** One request's permission state. */
    private static final class Access {
        final Caller caller;
        final boolean self, manager, legacyOp, legacyGranted, legacyRead, legacyWrite;
        final boolean readImages, writeImages, readVideo, writeVideo, readAudio, writeAudio;

        Access(Caller c) {
            caller = c;
            self = c.self();
            int sdk = self ? Integer.MAX_VALUE : c.targetSdk();
            manager = !self && c.permission(MANAGE_EXTERNAL_STORAGE);
            legacyOp = !self && c.legacyStorageOp();
            // PermissionUtils.checkIsLegacyStorageGranted (OPSTR_NO_ISOLATED_STORAGE is
            // not modeled: Gesher has no shell/instrumentation path that sets it).
            legacyGranted = !self && sdk < TARGET_V && legacyOp;
            boolean atLeastT = sdk >= TARGET_T;
            legacyRead = legacyGranted && (atLeastT
                    ? c.permission(READ_EXTERNAL_STORAGE) || (c.permission(READ_MEDIA_IMAGES)
                            && c.permission(READ_MEDIA_VIDEO) && c.permission(READ_MEDIA_AUDIO))
                    : c.permission(READ_EXTERNAL_STORAGE));
            legacyWrite = legacyGranted && c.permission(WRITE_EXTERNAL_STORAGE);
            readImages = read(atLeastT ? READ_MEDIA_IMAGES : READ_EXTERNAL_STORAGE, OPSTR_READ_MEDIA_IMAGES);
            readVideo = read(atLeastT ? READ_MEDIA_VIDEO : READ_EXTERNAL_STORAGE, OPSTR_READ_MEDIA_VIDEO);
            readAudio = read(atLeastT ? READ_MEDIA_AUDIO : READ_EXTERNAL_STORAGE, OPSTR_READ_MEDIA_AUDIO);
            writeImages = write(OPSTR_WRITE_MEDIA_IMAGES);
            writeVideo = write(OPSTR_WRITE_MEDIA_VIDEO);
            writeAudio = write(OPSTR_WRITE_MEDIA_AUDIO);
        }

        /** checkPermissionRead{Images,Video,Audio}. */
        private boolean read(String permission, String op) {
            if (self) return true;
            return caller.permission(permission) && opAllowingLegacy(op);
        }

        /** checkPermissionWrite{Images,Video,Audio}: checkPermissionAllowingNonLegacy. */
        private boolean write(String op) {
            if (self) return true;
            if (legacyOp && !caller.permission(WRITE_EXTERNAL_STORAGE)) return false;
            return opAllowingLegacy(op);
        }

        /** checkAppOpAllowingLegacy. */
        private boolean opAllowingLegacy(String op) {
            return caller.opMode(op) == MODE_ALLOWED || legacyOp;
        }

        /** AccessChecker.hasAccessToCollection. */
        boolean collection(int match, boolean forWrite) {
            switch (MediaStoreIndex.collection(match)) {
                case AUDIO_MEDIA: return forWrite ? writeAudio : readAudio || writeAudio;
                case IMAGES_MEDIA: return forWrite ? writeImages : readImages || writeImages;
                case VIDEO_MEDIA: return forWrite ? writeVideo : readVideo || writeVideo;
                case DOWNLOADS: case FILES: return !forWrite && legacyRead;
                default: throw new UnsupportedOperationException("Unknown or unsupported type: " + match);
            }
        }

        String ownerMatch() {
            StringBuilder in = new StringBuilder("owner_package_name IN (");
            String[] packages = caller.sharedPackages();
            if (packages == null || packages.length == 0) packages = new String[] {caller.packageName()};
            for (int i = 0; i < packages.length; i++) {
                if (i > 0) in.append(',');
                in.append(quote(packages[i]));
            }
            return in.append(')').toString();
        }

        /** AccessChecker.getWhereForConstrainedAccess. */
        String constrained(int match, boolean forWrite) {
            switch (MediaStoreIndex.collection(match)) {
                case AUDIO_MEDIA:
                    return ownerMatch() + " OR is_ringtone=1 OR is_alarm=1 OR is_notification=1";
                case IMAGES_MEDIA: case VIDEO_MEDIA:
                    return ownerMatch();
                case DOWNLOADS: {
                    List<String> options = new ArrayList<>();
                    options.add(ownerMatch());
                    if (forWrite && legacyWrite) options.add("volume_name='" + VOLUME_EXTERNAL_PRIMARY + "'");
                    return String.join(" OR ", options);
                }
                case FILES: {
                    List<String> options = new ArrayList<>();
                    options.add(ownerMatch());
                    if (forWrite && legacyWrite) options.add("volume_name='" + VOLUME_EXTERNAL_PRIMARY + "'");
                    if (collection(AUDIO_MEDIA, forWrite)) {
                        options.add("media_type=" + MEDIA_TYPE_AUDIO);
                        options.add("media_type=" + MEDIA_TYPE_PLAYLIST);
                        options.add("media_type=" + MEDIA_TYPE_SUBTITLE);
                    }
                    if (collection(VIDEO_MEDIA, forWrite)) {
                        options.add("media_type=" + MEDIA_TYPE_VIDEO);
                        options.add("media_type=" + MEDIA_TYPE_SUBTITLE);
                    }
                    if (collection(IMAGES_MEDIA, forWrite)) options.add("media_type=" + MEDIA_TYPE_IMAGE);
                    return String.join(" OR ", options);
                }
                default:
                    throw new UnsupportedOperationException("Unknown or unsupported type: " + match);
            }
        }

        /** MediaProvider.checkCallingPermissionGlobal. */
        boolean global(Uri uri, int match, boolean forWrite) {
            if (self || manager) return true;
            // Collections never get global access; an outstanding grant covers one item.
            return isItem(match) && caller.uriGranted(uri, forWrite);
        }
    }

    private static String quote(String value) {
        return "'" + value.replace("'", "''") + "'";
    }

    // ---- query builder (MediaProvider.getQueryBuilderInternal) ------------------------

    private SQLiteQueryBuilder builder(Access access, boolean forWrite, Uri uri, int match,
            int matchPending, int matchTrashed, int matchFavorite) {
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables("files");
        if (uri.getBooleanQueryParameter("distinct", false)) qb.setDistinct(true);
        qb.setStrict(true);
        if (!access.self) {
            qb.setStrictColumns(true);
            qb.setStrictGrammar(true);
        }
        qb.setProjectionMap(projectionMap(access, collection(match)));

        if (uri.getBooleanQueryParameter("includePending", false)) matchPending = MATCH_INCLUDE;
        if (matchPending == MATCH_DEFAULT) matchPending = MATCH_EXCLUDE;
        if (matchTrashed == MATCH_DEFAULT) matchTrashed = MATCH_EXCLUDE;
        if (matchFavorite == MATCH_DEFAULT) matchFavorite = MATCH_INCLUDE;

        // appendAccessCheckQuery
        if (!access.global(uri, match, forWrite) && !access.collection(match, forWrite)) {
            qb.appendWhereStandalone(access.constrained(match, forWrite));
        }
        if (isItem(match)) {
            qb.appendWhereStandalone("_id=" + ContentUris.parseId(uri));
            matchPending = MATCH_INCLUDE;
            matchTrashed = MATCH_INCLUDE;
        }
        switch (collection(match)) {
            case IMAGES_MEDIA: qb.appendWhereStandalone("media_type=" + MEDIA_TYPE_IMAGE); break;
            case VIDEO_MEDIA: qb.appendWhereStandalone("media_type=" + MEDIA_TYPE_VIDEO); break;
            case AUDIO_MEDIA: qb.appendWhereStandalone("media_type=" + MEDIA_TYPE_AUDIO); break;
            case DOWNLOADS: qb.appendWhereStandalone("is_download=1"); break;
            default: break;
        }
        appendMatch(qb, "is_pending", matchPending);
        appendMatch(qb, "is_trashed", matchTrashed);
        appendMatch(qb, "is_favorite", matchFavorite);
        // "external" names every external volume; Gesher attaches external_primary only.
        qb.appendWhereStandalone("volume_name IN ('" + VOLUME_EXTERNAL_PRIMARY + "')");
        return qb;
    }

    private static void appendMatch(SQLiteQueryBuilder qb, String column, int match) {
        switch (match) {
            case MATCH_INCLUDE: break;
            // There is no FUSE daemon, so no pending row is "pending from FUSE".
            case MATCH_EXCLUDE: qb.appendWhereStandalone(column + "=0"); break;
            case MATCH_ONLY: qb.appendWhereStandalone(column + "=1"); break;
            default: throw new IllegalArgumentException();
        }
    }

    /**
     * Projection map for a collection. owner_package_name is filtered for API 34+
     * callers without QUERY_ALL_PACKAGES / ACCESS_MEDIA_OWNER_PACKAGE_NAME
     * (MediaProvider.isApplicableForOwnerPackageNameFiltering).
     */
    private Map<String, String> projectionMap(Access access, int collection) {
        Map<String, String> map = new HashMap<>();
        for (String column : COLUMNS.get(collection)) map.put(column, column);
        if (!access.self && access.caller.targetSdk() >= TARGET_U
                && !access.caller.permission(QUERY_ALL_PACKAGES)
                && !access.caller.permission(ACCESS_MEDIA_OWNER_PACKAGE_NAME)) {
            map.put("owner_package_name", ownerProjection(access));
        }
        return map;
    }

    private String ownerProjection(Access access) {
        List<String> visible = new ArrayList<>();
        try (Cursor c = mDb.rawQuery("SELECT DISTINCT owner_package_name FROM files WHERE "
                + "owner_package_name IS NOT NULL AND owner_package_name <> ''", null)) {
            while (c.moveToNext()) {
                String owner = c.getString(0);
                if (Arrays.asList(access.caller.sharedPackages()).contains(owner)
                        || access.caller.canSeePackage(owner)) visible.add(quote(owner));
            }
        }
        if (visible.isEmpty()) return "NULL AS owner_package_name";
        return "CASE WHEN owner_package_name IN (" + String.join(",", visible)
                + ") THEN owner_package_name ELSE NULL END AS owner_package_name";
    }

    // ---- query -------------------------------------------------------------------------

    Cursor query(Caller caller, Uri uri, String[] projection, QueryArgs args) {
        int match = match(uri);
        requireMatch(uri, match);
        try {
            volume(uri);
        } catch (VolumeNotFoundException e) {
            if (!caller.self() && caller.targetSdk() < 29) return null; // translateForQuery
            throw e;
        }
        Access access = new Access(caller);
        SQLiteQueryBuilder qb = builder(access, false, uri, match,
                args.matchPending, args.matchTrashed, args.matchFavorite);
        String limit = args.limit != null ? args.limit : uri.getQueryParameter("limit");
        synchronized (mLock) {
            return qb.query(mDb, projection, args.selection, args.selectionArgs, null, null,
                    args.sortOrder, limit);
        }
    }

    /** MediaProvider.getType: the row's MIME type for an item, the collection type otherwise. */
    String getType(Uri uri) {
        int match = match(uri);
        if (match < 0) return null;
        if (isItem(match)) {
            synchronized (mLock) {
                try (Cursor c = mDb.rawQuery("SELECT mime_type FROM files WHERE _id=?",
                        new String[] {Long.toString(ContentUris.parseId(uri))})) {
                    if (c.moveToFirst() && !c.isNull(0)) return c.getString(0);
                }
            }
            return null;
        }
        switch (collection(match)) {
            case IMAGES_MEDIA: return "vnd.android.cursor.dir/image";
            case VIDEO_MEDIA: return "vnd.android.cursor.dir/video";
            case AUDIO_MEDIA: return "vnd.android.cursor.dir/audio";
            default: return null;
        }
    }

    // ---- insert (MediaProvider.insertInternal / insertFile / ensureFileColumns) -----------

    Uri insert(Caller caller, Uri uri, ContentValues initial) {
        int match = match(uri);
        requireMatch(uri, match);
        if (isItem(match)) throw new UnsupportedOperationException("Invalid URI " + uri);
        String volume;
        try {
            volume = volume(uri);
        } catch (VolumeNotFoundException e) {
            if (!caller.self() && caller.targetSdk() < 29) return null; // translateForInsert
            throw e;
        }
        Access access = new Access(caller);
        ContentValues values = initial == null ? new ContentValues() : new ContentValues(initial);
        values.remove("_id");
        boolean pending = asBoolean(values, "is_pending", false);
        boolean trashed = asBoolean(values, "is_trashed", false);
        computeDateExpires(values, pending, trashed);

        // sDataColumns: only the system, legacy writers and managers place by raw path.
        if (values.containsKey("_data")) {
            if (!(access.self || access.legacyWrite || access.manager)) {
                if (caller.targetSdk() >= TARGET_U) {
                    throw new IllegalArgumentException("Mutation of _data is not allowed.");
                }
                values.remove("_data");
            }
        }
        String owner;
        if (access.self) {
            owner = values.getAsString("owner_package_name");
        } else {
            values.remove("owner_package_name");
            owner = caller.packageName();
        }
        for (String column : new ArrayList<>(values.keySet())) {
            if (!COLUMNS.get(FILES).contains(column) && !"date_expires".equals(column)) {
                throw new IllegalArgumentException("Invalid column " + column);
            }
            if (!access.self && DERIVED.contains(column)) values.remove(column);
        }
        if (!access.self) {
            if (values.containsKey("latitude")) values.putNull("latitude");
            if (values.containsKey("longitude")) values.putNull("longitude");
        }

        int collection = collection(match);
        int mediaType = defaultMediaType(collection);
        ensureFileColumns(access, collection, uri, values, true, null);
        String path = values.getAsString("_data");
        computeValuesFromData(values);
        values.put("volume_name", VOLUME_EXTERNAL_PRIMARY);
        values.put("date_added", mHost.currentTimeMillis() / 1000);
        if (values.getAsString("title") == null) values.put("title", extractFileName(path));
        String mimeType = values.getAsString("mime_type");
        if (mimeType == null) mimeType = resolveMimeType(new File(path));
        values.put("mime_type", mimeType);
        values.put("media_type", mimeType != null ? resolveMediaType(mimeType) : mediaType);
        values.put("format", resolveFormatCode(mimeType));
        values.put("_modifier", 1); // FileColumns._MODIFIER_CR
        values.put("_user_id", 0);
        if (owner != null) values.put("owner_package_name", owner);
        if (collection == DOWNLOADS || isInDownloads(values.getAsString("relative_path"))) {
            values.put("is_download", 1); // maybeMarkAsDownload
        }
        File file = new File(path);
        if (file.exists()) {
            values.put("date_modified", file.lastModified() / 1000);
            if (!values.containsKey("_size")) values.put("_size", file.length());
        }
        long id;
        synchronized (mLock) {
            mDb.beginTransaction();
            try {
                long generation = nextGeneration();
                values.put("generation_added", generation);
                values.put("generation_modified", generation);
                id = insertRow(values);
                mDb.setTransactionSuccessful();
            } finally {
                mDb.endTransaction();
            }
        }
        Uri result = ContentUris.withAppendedId(uri, id);
        notifyFor(volume, values.getAsInteger("media_type"), id);
        return result;
    }

    private long insertRow(ContentValues values) {
        StringBuilder sql = new StringBuilder("INSERT INTO files (");
        StringBuilder marks = new StringBuilder();
        List<Object> args = new ArrayList<>();
        for (String key : values.keySet()) {
            if (!args.isEmpty()) { sql.append(','); marks.append(','); }
            sql.append(key);
            marks.append('?');
            args.add(values.get(key));
        }
        sql.append(") VALUES (").append(marks).append(')');
        return android.database.DatabaseUtils.executeInsert(mDb, sql.toString(), args.toArray());
    }

    private static int defaultMediaType(int collection) {
        switch (collection) {
            case IMAGES_MEDIA: return MEDIA_TYPE_IMAGE;
            case VIDEO_MEDIA: return MEDIA_TYPE_VIDEO;
            case AUDIO_MEDIA: return MEDIA_TYPE_AUDIO;
            default: return MEDIA_TYPE_NONE;
        }
    }

    private static boolean isInDownloads(String relativePath) {
        return relativePath != null && relativePath.toLowerCase(Locale.ROOT)
                .startsWith(Environment.DIRECTORY_DOWNLOADS.toLowerCase(Locale.ROOT) + "/");
    }

    /** FileUtils.computeDateExpires. */
    private void computeDateExpires(ContentValues values, boolean pending, boolean trashed) {
        if (!pending && !trashed) {
            values.remove("date_expires");
            return;
        }
        if (values.containsKey("date_expires")) return;
        long duration = pending ? DEFAULT_DURATION_PENDING : DEFAULT_DURATION_TRASHED;
        values.put("date_expires", (mHost.currentTimeMillis() + duration) / 1000);
    }

    /**
     * MediaProvider.ensureFileColumns: defaults, MIME checks, allowed primary
     * directories, _data computation; {@code currentPath} is the row's path on update.
     */
    private void ensureFileColumns(Access access, int collection, Uri uri, ContentValues values,
            boolean makeUnique, String currentPath) {
        String defaultMimeType = MIMETYPE_UNKNOWN;
        int defaultMediaType = MEDIA_TYPE_NONE;
        String defaultPrimary = Environment.DIRECTORY_DOWNLOADS;
        List<String> allowedPrimary = Arrays.asList(Environment.DIRECTORY_DOWNLOADS,
                Environment.DIRECTORY_DOCUMENTS);
        switch (collection) {
            case AUDIO_MEDIA:
                defaultMimeType = "audio/mpeg";
                defaultMediaType = MEDIA_TYPE_AUDIO;
                defaultPrimary = Environment.DIRECTORY_MUSIC;
                allowedPrimary = Arrays.asList(Environment.DIRECTORY_ALARMS,
                        Environment.DIRECTORY_AUDIOBOOKS, Environment.DIRECTORY_MUSIC,
                        Environment.DIRECTORY_NOTIFICATIONS, Environment.DIRECTORY_PODCASTS,
                        Environment.DIRECTORY_RECORDINGS, Environment.DIRECTORY_RINGTONES);
                break;
            case VIDEO_MEDIA:
                defaultMimeType = "video/mp4";
                defaultMediaType = MEDIA_TYPE_VIDEO;
                defaultPrimary = Environment.DIRECTORY_MOVIES;
                allowedPrimary = Arrays.asList(Environment.DIRECTORY_DCIM,
                        Environment.DIRECTORY_MOVIES, Environment.DIRECTORY_PICTURES);
                break;
            case IMAGES_MEDIA:
                defaultMimeType = "image/jpeg";
                defaultMediaType = MEDIA_TYPE_IMAGE;
                defaultPrimary = Environment.DIRECTORY_PICTURES;
                allowedPrimary = Arrays.asList(Environment.DIRECTORY_DCIM,
                        Environment.DIRECTORY_PICTURES);
                break;
            case DOWNLOADS:
                defaultPrimary = Environment.DIRECTORY_DOWNLOADS;
                allowedPrimary = Collections.singletonList(defaultPrimary);
                break;
            default:
                break;
        }

        File volumePath = volumeRoot();
        if (!isEmpty(values.getAsString("_data"))) {
            // Force values when raw path provided
            values.put("_data", requireUnderVolume(values.getAsString("_data")).getPath());
            enforcePrivateStorageAccess(access.caller, values.getAsString("_data"));
            computeValuesFromData(values);
        }

        boolean targetR = access.self || access.caller.targetSdk() >= TARGET_R;
        String displayName = values.getAsString("_display_name");
        String mimeTypeFromExt = isEmpty(displayName) ? null : resolveMimeTypeOrNull(displayName);
        if (isEmpty(values.getAsString("mime_type"))) {
            if (targetR) {
                values.put("mime_type", mimeTypeFromExt != null ? mimeTypeFromExt : defaultMimeType);
            } else if (defaultMediaType == MEDIA_TYPE_NONE) {
                values.put("mime_type", mimeTypeFromExt);
            } else {
                values.put("mime_type", defaultMimeType);
            }
        }
        String mimeType = values.getAsString("mime_type");
        if (defaultMediaType != MEDIA_TYPE_NONE && mimeType != null
                && MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType) == null) {
            if (mimeTypeFromExt != null && defaultMediaType == resolveMediaType(mimeTypeFromExt)) {
                values.put("mime_type", mimeTypeFromExt);
            } else if (targetR) {
                throw new IllegalArgumentException("Unsupported MIME type " + mimeType);
            } else {
                values.put("mime_type", defaultMimeType);
            }
        }
        if (isEmpty(values.getAsString("_display_name"))) {
            values.put("_display_name", String.valueOf(mHost.currentTimeMillis()));
        }
        mimeType = values.getAsString("mime_type");
        if (mimeType != null) {
            int actualMediaType = resolveMediaType(mimeType);
            if (defaultMediaType == MEDIA_TYPE_NONE) {
                if (actualMediaType == MEDIA_TYPE_PLAYLIST) {
                    defaultPrimary = Environment.DIRECTORY_MUSIC;
                    allowedPrimary = new ArrayList<>(allowedPrimary);
                    allowedPrimary.add(Environment.DIRECTORY_MUSIC);
                    allowedPrimary.add(Environment.DIRECTORY_MOVIES);
                } else if (actualMediaType == MEDIA_TYPE_SUBTITLE) {
                    defaultPrimary = Environment.DIRECTORY_MOVIES;
                    allowedPrimary = new ArrayList<>(allowedPrimary);
                    allowedPrimary.add(Environment.DIRECTORY_MUSIC);
                    allowedPrimary.add(Environment.DIRECTORY_MOVIES);
                }
            } else if (defaultMediaType != actualMediaType) {
                String[] split = defaultMimeType.split("/");
                throw new IllegalArgumentException("MIME type " + mimeType
                        + " cannot be inserted into " + uri + "; expected MIME type under "
                        + split[0] + "/*");
            }
        }
        if (isEmpty(values.getAsString("relative_path"))) {
            values.put("relative_path", defaultPrimary + '/');
        }

        if (isEmpty(values.getAsString("_data"))) {
            sanitizeValues(values, true);
            computeDataFromValues(values, volumePath);
            File res = new File(values.getAsString("_data"));
            try {
                res = makeUnique ? buildUniqueFile(res.getParentFile(), mimeType, res.getName())
                        : buildNonUniqueFile(res.getParentFile(), mimeType, res.getName());
            } catch (FileNotFoundException e) {
                throw new IllegalStateException("Failed to build unique file: " + res + " " + values);
            }
            String currentDir = currentPath != null ? new File(currentPath).getParent() : null;
            boolean validPath = res.getParent().equals(currentDir);
            String primary = extractTopLevelDir(values.getAsString("relative_path"));
            if (!validPath) validPath = containsIgnoreCase(allowedPrimary, primary);
            if (!validPath) {
                // An app's own Android/media directory (isExternalMediaDirectory).
                String relative = values.getAsString("relative_path");
                for (String pkg : access.caller.sharedPackages()) {
                    if (relative.toLowerCase(Locale.ROOT).startsWith(
                            ("Android/media/" + pkg + "/").toLowerCase(Locale.ROOT))) validPath = true;
                }
            }
            if (!validPath) validPath = access.manager || access.self;
            if (!validPath) {
                throw new IllegalArgumentException("Primary directory " + primary
                        + " not allowed for " + uri + "; allowed directories are " + allowedPrimary);
            }
            enforcePrivateStorageAccess(access.caller, res.getPath());
            File parent = res.getParentFile();
            parent.mkdirs();
            if (!parent.isDirectory()) {
                throw new IllegalStateException("Failed to create directory: " + res);
            }
            values.put("_data", res.getPath());
            computeValuesFromData(values);
        } else {
            // Raw _data (system, legacy writer, manager): already under the volume.
            File parent = new File(values.getAsString("_data")).getParentFile();
            if (parent != null) parent.mkdirs();
        }
        if (!access.self && !access.manager && !access.legacyWrite) {
            // assertFileColumnsConsistent: the path stays on the primary volume.
            requireUnderVolume(values.getAsString("_data"));
        }
    }

    private File volumeRoot() {
        return mHost.volumeRoot().getAbsoluteFile();
    }

    /** The absolute path of {@code data} if it lies inside the volume (VolumeArgumentException). */
    private File requireUnderVolume(String data) {
        String relative = relativeToVolume(data);
        if (relative == null) {
            throw new IllegalArgumentException("Requested path " + data
                    + " doesn't appear under [" + volumeRoot() + "]");
        }
        return new File(volumeRoot(), relative);
    }

    /**
     * Global MediaStore access and row ownership do not authorize another UID's
     * Android/data or Android/obb files. The system server performs the actual
     * file operation, so enforce this boundary before handing out a descriptor
     * or creating, moving or deleting a file on the caller's behalf. Public
     * MediaStore-owned items retain their normal scoped-storage access rules.
     */
    private void enforcePrivateStorageAccess(Caller caller, String data) {
        if (caller.self() || data == null) return;
        File file = requireUnderVolume(data);
        enforcePrivateStorageRelative(caller, relativeToVolume(file.getPath()));
        try {
            // The first insert may create the volume directory; canonicalize
            // its trusted location without requiring that leaf to exist yet.
            java.nio.file.Path root = volumeRoot().getCanonicalFile().toPath();
            java.nio.file.Path target = file.toPath().toAbsolutePath().normalize();
            // Resolve an existing ancestor for inserts into directories not yet
            // created. A dangling link fails toRealPath instead of being treated
            // as a new, ordinary destination below the shared volume.
            List<java.nio.file.Path> missing = new ArrayList<>();
            while (!java.nio.file.Files.exists(target, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
                if (target.getParent() == null) throw new IOException("No existing storage ancestor");
                missing.add(target.getFileName());
                target = target.getParent();
            }
            target = target.toRealPath();
            for (int i = missing.size() - 1; i >= 0; --i) target = target.resolve(missing.get(i));
            if (!target.startsWith(root) || target.equals(root)) {
                throw new SecurityException("Requested media path is outside the shared volume: " + data);
            }
            enforcePrivateStorageRelative(caller,
                    root.relativize(target).toString().replace(File.separatorChar, '/'));
        } catch (IOException e) {
            throw new SecurityException("Cannot resolve media storage path: " + data, e);
        }
    }

    private static void enforcePrivateStorageRelative(Caller caller, String relative) {
        String[] parts = relative.split("/");
        if (parts.length < 2 || !"Android".equalsIgnoreCase(parts[0])
                || !("data".equalsIgnoreCase(parts[1]) || "obb".equalsIgnoreCase(parts[1]))) return;
        String[] packages = caller.sharedPackages();
        if (packages == null || packages.length == 0) packages = new String[] {caller.packageName()};
        if (parts.length >= 3) {
            for (String pkg : packages) {
                if (pkg != null && pkg.equalsIgnoreCase(parts[2])) return;
            }
        }
        throw new SecurityException(caller.packageName() + " has no access to private storage " + relative);
    }

    /** {@code path} relative to the volume root with '/' separators, or null if outside it. */
    String relativeToVolume(String path) {
        if (path == null) return null;
        java.nio.file.Path root = volumeRoot().toPath().normalize();
        java.nio.file.Path target = new File(path).getAbsoluteFile().toPath().normalize();
        if (!target.startsWith(root) || target.equals(root)) {
            try {
                java.nio.file.Path canonicalRoot = volumeRoot().getCanonicalFile().toPath();
                java.nio.file.Path canonical = new File(path).getCanonicalFile().toPath();
                if (!canonical.startsWith(canonicalRoot) || canonical.equals(canonicalRoot)) return null;
                root = canonicalRoot;
                target = canonical;
            } catch (IOException e) {
                return null;
            }
        }
        StringBuilder relative = new StringBuilder();
        for (java.nio.file.Path part : root.relativize(target)) {
            if (relative.length() > 0) relative.append('/');
            relative.append(part.toString());
        }
        return relative.toString();
    }

    /**
     * FileUtils.computeValuesFromData over Gesher's volume directory (AOSP matches
     * {@code /storage/emulated/<user>/}; the same relative layout is under
     * {@link Host#volumeRoot}).
     */
    private void computeValuesFromData(ContentValues values) {
        values.remove("volume_name");
        values.remove("relative_path");
        values.remove("is_trashed");
        values.remove("date_expires");
        values.remove("_display_name");
        values.remove("bucket_id");
        values.remove("bucket_display_name");
        String data = values.getAsString("_data");
        if (isEmpty(data)) return;
        String relative = relativeToVolume(data);
        if (relative == null) throw new IllegalArgumentException("Invalid file path:" + data + " in request.");
        values.put("_data", new File(volumeRoot(), relative).getPath());
        values.put("volume_name", VOLUME_EXTERNAL_PRIMARY);
        int slash = relative.lastIndexOf('/');
        values.put("relative_path", slash < 0 ? "/" : relative.substring(0, slash + 1));
        String displayName = slash < 0 ? relative : relative.substring(slash + 1);
        Matcher matcher = PATTERN_EXPIRES_FILE.matcher(displayName);
        if (matcher.matches()) {
            values.put("is_pending", matcher.group(1).equalsIgnoreCase("pending") ? 1 : 0);
            values.put("is_trashed", matcher.group(1).equalsIgnoreCase("trashed") ? 1 : 0);
            values.put("date_expires", Long.parseLong(matcher.group(2)));
            values.put("_display_name", matcher.group(3));
        } else {
            values.put("is_pending", 0);
            values.put("is_trashed", 0);
            values.putNull("date_expires");
            values.put("_display_name", displayName);
        }
        String parentLower = slash < 0 ? volumeRoot().getPath().toLowerCase(Locale.ROOT)
                : new File(volumeRoot(), relative.substring(0, slash)).getPath().toLowerCase(Locale.ROOT);
        values.put("bucket_id", parentLower.hashCode());
        if (slash < 0) {
            values.putNull("bucket_display_name");
        } else {
            String parent = relative.substring(0, slash);
            values.put("bucket_display_name", parent.substring(parent.lastIndexOf('/') + 1));
        }
    }

    /** FileUtils.computeDataFromValues. */
    private void computeDataFromValues(ContentValues values, File volumePath) {
        values.remove("_data");
        String displayName = values.getAsString("_display_name");
        String resolved;
        if (asBoolean(values, "is_pending", false)) {
            long expires = asLong(values, "date_expires",
                    (mHost.currentTimeMillis() + DEFAULT_DURATION_PENDING) / 1000);
            resolved = trimFilename(String.format(Locale.US, ".pending-%d-%s", expires, displayName));
        } else if (asBoolean(values, "is_trashed", false)) {
            long expires = asLong(values, "date_expires",
                    (mHost.currentTimeMillis() + DEFAULT_DURATION_TRASHED) / 1000);
            resolved = trimFilename(String.format(Locale.US, ".trashed-%d-%s", expires, displayName));
        } else {
            resolved = displayName;
        }
        String relativePath = values.getAsString("relative_path");
        if (relativePath == null) relativePath = "";
        File file = volumePath;
        for (String segment : relativePath.split("/")) {
            if (!segment.isEmpty()) file = new File(file, segment);
        }
        file = new File(file, resolved);
        values.put("_data", file.getPath());
    }

    /** FileUtils.sanitizeValues. */
    private static void sanitizeValues(ContentValues values, boolean rewriteHiddenFileName) {
        String[] relativePath = values.getAsString("relative_path").split("/");
        for (int i = 0; i < relativePath.length; i++) {
            relativePath[i] = sanitizeDisplayName(relativePath[i], rewriteHiddenFileName);
        }
        values.put("relative_path", String.join("/", relativePath) + "/");
        values.put("_display_name", sanitizeDisplayName(values.getAsString("_display_name"),
                rewriteHiddenFileName));
    }

    private static String sanitizeDisplayName(String name, boolean rewriteHiddenFileName) {
        if (name == null) return null;
        if (rewriteHiddenFileName && name.startsWith(".")) return "_" + name;
        return buildValidFatFilename(name);
    }

    /** FileUtils.buildValidFatFilename. */
    static String buildValidFatFilename(String name) {
        if (name == null || name.isEmpty() || ".".equals(name) || "..".equals(name)) return "(invalid)";
        StringBuilder res = new StringBuilder(name.length());
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            boolean valid = c > 0x1f;
            switch (c) {
                case '"': case '*': case '/': case ':': case '<': case '>': case '?':
                case '\\': case '|': case 0x7F:
                    valid = false;
                    break;
                default:
                    break;
            }
            res.append(valid ? c : '_');
        }
        return trimFilename(res.toString());
    }

    private static String trimFilename(String name) {
        StringBuilder res = new StringBuilder(name);
        byte[] raw = res.toString().getBytes(StandardCharsets.UTF_8);
        if (raw.length > MAX_FILENAME_BYTES) {
            int max = MAX_FILENAME_BYTES - 3;
            while (raw.length > max) {
                res.deleteCharAt(res.length() / 2);
                raw = res.toString().getBytes(StandardCharsets.UTF_8);
            }
            res.insert(res.length() / 2, "...");
        }
        return res.toString();
    }

    /** FileUtils.splitFileName. */
    private static String[] splitFileName(String mimeType, String displayName) {
        String name, ext, mimeTypeFromExt;
        int lastDot = displayName.lastIndexOf('.');
        if (lastDot > 0) {
            name = displayName.substring(0, lastDot);
            ext = displayName.substring(lastDot + 1);
            mimeTypeFromExt = MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                    ext.toLowerCase(Locale.ROOT));
        } else {
            name = displayName;
            ext = null;
            mimeTypeFromExt = null;
        }
        if (mimeTypeFromExt == null) mimeTypeFromExt = MIMETYPE_UNKNOWN;
        String extFromMimeType = mimeType == null || MIMETYPE_UNKNOWN.equalsIgnoreCase(mimeType)
                ? null : MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType);
        if (!(mimeType != null && mimeType.equalsIgnoreCase(mimeTypeFromExt))
                && !(ext != null && ext.equalsIgnoreCase(extFromMimeType))) {
            name = displayName;
            ext = extFromMimeType;
        }
        return new String[] {name, ext == null ? "" : ext};
    }

    private static File buildFile(File parent, String name, String ext) {
        return ext.isEmpty() ? new File(parent, name) : new File(parent, name + "." + ext);
    }

    private static File buildNonUniqueFile(File parent, String mimeType, String displayName) {
        String[] parts = splitFileName(mimeType, displayName);
        return buildFile(parent, parts[0], parts[1]);
    }

    /** FileUtils.buildUniqueFile, with buildUniqueNameIterator's "name (n)" sequence. */
    private static File buildUniqueFile(File parent, String mimeType, String displayName)
            throws FileNotFoundException {
        String[] parts = splitFileName(mimeType, displayName);
        for (int i = 0; i < 32; i++) {
            File file = buildFile(parent, i == 0 ? parts[0] : parts[0] + " (" + i + ")", parts[1]);
            if (!file.exists()) return file;
        }
        throw new FileNotFoundException("Failed to create unique file");
    }

    private static String extractTopLevelDir(String relativePath) {
        if (relativePath == null) return null;
        String[] segments = relativePath.split("/");
        return segments.length > 0 ? segments[0] : null;
    }

    private static boolean containsIgnoreCase(List<String> list, String item) {
        if (item == null) return false;
        for (String s : list) if (s.equalsIgnoreCase(item)) return true;
        return false;
    }

    private static String extractFileName(String data) {
        if (data == null) return null;
        String name = data.substring(data.lastIndexOf('/') + 1);
        name = name.substring(name.lastIndexOf(File.separatorChar) + 1);
        int lastDot = name.lastIndexOf('.');
        return lastDot == -1 ? name : name.substring(0, lastDot);
    }

    // ---- MIME (MimeUtils) ----------------------------------------------------------------

    private static String resolveMimeTypeOrNull(String name) {
        int lastDot = name.lastIndexOf('.');
        if (lastDot < 0) return null;
        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                name.substring(lastDot + 1).toLowerCase(Locale.ROOT));
    }

    /** MimeUtils.resolveMimeType. */
    static String resolveMimeType(File file) {
        String mime = resolveMimeTypeOrNull(file.getName());
        return mime == null ? MIMETYPE_UNKNOWN : mime;
    }

    /** MimeUtils.resolveMediaType. */
    static int resolveMediaType(String mimeType) {
        if (mimeType == null) return MEDIA_TYPE_NONE;
        String lower = mimeType.toLowerCase(Locale.ROOT);
        switch (lower) {
            case "application/vnd.apple.mpegurl": case "application/vnd.ms-wpl":
            case "application/x-extension-smpl": case "application/x-mpegurl":
            case "application/xspf+xml": case "audio/mpegurl": case "audio/x-mpegurl":
            case "audio/x-scpls":
                return MEDIA_TYPE_PLAYLIST;
            case "application/lrc": case "application/smil+xml": case "application/ttml+xml":
            case "application/x-extension-cap": case "application/x-extension-srt":
            case "application/x-extension-sub": case "application/x-extension-vtt":
            case "application/x-subrip": case "text/vtt":
                return MEDIA_TYPE_SUBTITLE;
            default:
                break;
        }
        if (lower.startsWith("audio/")) return MEDIA_TYPE_AUDIO;
        if (lower.startsWith("video/") || lower.equals("application/vnd.ms-asf")) return MEDIA_TYPE_VIDEO;
        if (lower.startsWith("image/")) return MEDIA_TYPE_IMAGE;
        if (lower.startsWith("text/") || DOCUMENT_TYPES.contains(lower)) return MEDIA_TYPE_DOCUMENT;
        return MEDIA_TYPE_NONE;
    }

    private static final Set<String> DOCUMENT_TYPES = columns(new String[] {
            "application/epub+zip", "application/msword", "application/pdf", "application/rtf",
            "application/vnd.ms-excel", "application/vnd.ms-excel.addin.macroenabled.12",
            "application/vnd.ms-excel.sheet.binary.macroenabled.12",
            "application/vnd.ms-excel.sheet.macroenabled.12",
            "application/vnd.ms-excel.template.macroenabled.12", "application/vnd.ms-powerpoint",
            "application/vnd.ms-powerpoint.addin.macroenabled.12",
            "application/vnd.ms-powerpoint.presentation.macroenabled.12",
            "application/vnd.ms-powerpoint.slideshow.macroenabled.12",
            "application/vnd.ms-powerpoint.template.macroenabled.12",
            "application/vnd.ms-word.document.macroenabled.12",
            "application/vnd.ms-word.template.macroenabled.12",
            "application/vnd.oasis.opendocument.chart", "application/vnd.oasis.opendocument.database",
            "application/vnd.oasis.opendocument.formula", "application/vnd.oasis.opendocument.graphics",
            "application/vnd.oasis.opendocument.graphics-template",
            "application/vnd.oasis.opendocument.presentation",
            "application/vnd.oasis.opendocument.presentation-template",
            "application/vnd.oasis.opendocument.spreadsheet",
            "application/vnd.oasis.opendocument.spreadsheet-template",
            "application/vnd.oasis.opendocument.text", "application/vnd.oasis.opendocument.text-master",
            "application/vnd.oasis.opendocument.text-template",
            "application/vnd.oasis.opendocument.text-web",
            "application/vnd.openxmlformats-officedocument.presentationml.presentation",
            "application/vnd.openxmlformats-officedocument.presentationml.slideshow",
            "application/vnd.openxmlformats-officedocument.presentationml.template",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.template",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.template",
            "application/vnd.stardivision.calc", "application/vnd.stardivision.chart",
            "application/vnd.stardivision.draw", "application/vnd.stardivision.impress",
            "application/vnd.stardivision.impress-packed", "application/vnd.stardivision.mail",
            "application/vnd.stardivision.math", "application/vnd.stardivision.writer",
            "application/vnd.stardivision.writer-global", "application/vnd.sun.xml.calc",
            "application/vnd.sun.xml.calc.template", "application/vnd.sun.xml.draw",
            "application/vnd.sun.xml.draw.template", "application/vnd.sun.xml.impress",
            "application/vnd.sun.xml.impress.template", "application/vnd.sun.xml.math",
            "application/vnd.sun.xml.writer", "application/vnd.sun.xml.writer.global",
            "application/vnd.sun.xml.writer.template", "application/x-mspublisher"});

    /** MimeUtils.resolveFormatCode (MtpConstants FORMAT_UNDEFINED_AUDIO/VIDEO, DEFINED, UNDEFINED). */
    private static int resolveFormatCode(String mimeType) {
        switch (resolveMediaType(mimeType)) {
            case MEDIA_TYPE_AUDIO: return 0xB900;
            case MEDIA_TYPE_VIDEO: return 0xB980;
            case MEDIA_TYPE_IMAGE: return 0x3800;
            default: return 0x3000;
        }
    }

    // ---- update ----------------------------------------------------------------------

    private static final class Row {
        long id;
        String data;
        int mediaType;
        boolean pending, trashed;
    }

    /** Rows the access-checked builder admits for {@code selection}, with their paths. */
    private List<Row> rows(SQLiteQueryBuilder qb, String selection, String[] selectionArgs) {
        List<Row> rows = new ArrayList<>();
        Map<String, String> map = new HashMap<>(qb.getProjectionMap());
        for (String column : new String[] {"_id", "_data", "media_type", "is_pending", "is_trashed"}) {
            map.put(column, column);
        }
        qb.setProjectionMap(map);
        try (Cursor c = qb.query(mDb, new String[] {"_id", "_data", "media_type", "is_pending",
                "is_trashed"}.clone(), selection, selectionArgs, null, null, null, null)) {
            while (c.moveToNext()) {
                Row row = new Row();
                row.id = c.getLong(0);
                row.data = c.getString(1);
                row.mediaType = c.isNull(2) ? MEDIA_TYPE_NONE : c.getInt(2);
                row.pending = c.getInt(3) != 0;
                row.trashed = c.getInt(4) != 0;
                rows.add(row);
            }
        }
        return rows;
    }

    /**
     * MediaProvider.enforceCallingPermissionInternal for a specific item: without write
     * access the caller gets SecurityException. AOSP throws a RecoverableSecurityException
     * whose action launches the user-consent screen; Gesher has no such screen, so the
     * recoverable form (and its PendingIntent) is not offered.
     */
    private void enforceItemWrite(Access access, Uri uri, int match) {
        if (access.global(uri, match, true)) return;
        SQLiteQueryBuilder write = builder(access, true, uri, match, MATCH_INCLUDE, MATCH_INCLUDE,
                MATCH_INCLUDE);
        if (!rows(write, null, null).isEmpty()) return;
        throw new SecurityException(access.caller.packageName() + " has no access to " + uri);
    }

    int update(Caller caller, Uri uri, ContentValues initial, QueryArgs args) {
        int match = match(uri);
        requireMatch(uri, match);
        String volume;
        try {
            volume = volume(uri);
        } catch (VolumeNotFoundException e) {
            if (!caller.self() && caller.targetSdk() < 29) return 0; // translateForUpdateDelete
            throw e;
        }
        Access access = new Access(caller);
        if (initial == null || initial.size() == 0) return 0;
        ContentValues requested = new ContentValues(initial);
        for (String column : requested.keySet()) {
            if (!COLUMNS.get(FILES).contains(column) && !"date_expires".equals(column)) {
                throw new IllegalArgumentException("Invalid column " + column);
            }
        }
        requested.remove("_id");
        if (!access.self) {
            // Only the system (scanner, shell) changes ownership (sMutableColumns lists the
            // column, but updateInternal removes it for every other caller).
            requested.remove("owner_package_name");
            requested.remove("generation_modified");
            // sDataColumns: raw paths only from legacy writers; ignored otherwise.
            if (requested.containsKey("_data") && !access.legacyWrite) requested.remove("_data");
        }
        // Give callers interacting with a specific media item a chance to escalate access.
        if (match == IMAGES_MEDIA_ID || match == VIDEO_MEDIA_ID || match == AUDIO_MEDIA_ID) {
            enforceItemWrite(access, uri, match);
        }
        // MediaProvider.updateInternal: placement columns move a file only for a single
        // item ("We only support movement under well-defined collections"). DATE_EXPIRES
        // is not a mutable column, so updateInternal has already dropped it for a
        // collection URI (isPending() is false there) before this check.
        if (!access.self && !requested.containsKey("_data") && !isItem(match)) {
            for (String column : requested.keySet()) {
                if (PLACEMENT.contains(column) && !"date_expires".equals(column)) {
                    throw new IllegalArgumentException("Movement of " + uri
                            + " which isn't part of well-defined collection not allowed");
                }
            }
        }
        int count = 0;
        List<Integer> types = new ArrayList<>();
        synchronized (mLock) {
            SQLiteQueryBuilder qb = builder(access, true, uri, match, args.matchPending,
                    args.matchTrashed, args.matchFavorite);
            List<Row> rows = rows(qb, args.selection, args.selectionArgs);
            for (Row row : rows) enforcePrivateStorageAccess(caller, row.data);
            FileOperations ops = new FileOperations();
            try {
                mDb.beginTransaction();
                try {
                    long generation = rows.isEmpty() ? 0 : nextGeneration();
                    for (Row row : rows) {
                        if (updateRow(access, uri, collection(match), row,
                                new ContentValues(requested), generation, ops)) {
                            count++;
                            types.add(row.mediaType);
                        }
                    }
                    mDb.setTransactionSuccessful();
                } finally {
                    mDb.endTransaction();
                }
            } catch (RuntimeException | Error e) {
                // The rows rolled back: put every file this update moved where they point.
                ops.abort();
                throw e;
            }
            ops.finish();
        }
        if (count > 0) notifyCollections(volume, types);
        return count;
    }

    private boolean updateRow(Access access, Uri uri, int collection, Row row, ContentValues values,
            long generation, FileOperations ops) {
        boolean publishing = false;
        if (!access.self) {
            // Column values controlled by the scanner are only writable while pending.
            for (String column : new ArrayList<>(values.keySet())) {
                if (MUTABLE.contains(column)) {
                    if ("is_pending".equals(column)) publishing = true;
                } else if (!row.pending || DERIVED.contains(column)) {
                    values.remove(column);
                }
            }
            if (values.containsKey("latitude")) values.putNull("latitude");
            if (values.containsKey("longitude")) values.putNull("longitude");
        }
        if (values.size() == 0) return false;
        boolean placement = false;
        for (String column : values.keySet()) if (PLACEMENT.contains(column)) placement = true;
        File before = row.data == null ? null : new File(row.data);
        if (placement) {
            // Merge the current placement with the requested change, then recompute.
            ContentValues current = currentPlacement(row.id);
            boolean pending = values.containsKey("is_pending")
                    ? asBoolean(values, "is_pending", false) : asBoolean(current, "is_pending", false);
            boolean trashed = values.containsKey("is_trashed")
                    ? asBoolean(values, "is_trashed", false) : asBoolean(current, "is_trashed", false);
            ContentValues placementValues = new ContentValues();
            for (String column : new String[] {"relative_path", "_display_name", "mime_type"}) {
                placementValues.put(column, values.containsKey(column)
                        ? values.getAsString(column) : current.getAsString(column));
            }
            placementValues.put("is_pending", pending ? 1 : 0);
            placementValues.put("is_trashed", trashed ? 1 : 0);
            boolean expiresChanged = values.containsKey("is_pending") || values.containsKey("is_trashed");
            if (!expiresChanged && current.get("date_expires") != null) {
                placementValues.put("date_expires", current.getAsLong("date_expires"));
            } else if (values.containsKey("date_expires")) {
                placementValues.put("date_expires", values.getAsLong("date_expires"));
            }
            computeDateExpires(placementValues, pending, trashed);
            if (values.containsKey("_data")) {
                placementValues.put("_data", values.getAsString("_data"));
            }
            // ensureNonUniqueFileColumns probes the path; an actual movement then takes a
            // unique destination (ensureUniqueFileColumns), as MediaProvider.updateInternal.
            ContentValues resolved = new ContentValues(placementValues);
            ensureFileColumns(access, collection, uri, resolved, false, row.data);
            if (row.data != null && !resolved.getAsString("_data").equalsIgnoreCase(row.data)) {
                resolved = new ContentValues(placementValues);
                ensureFileColumns(access, collection, uri, resolved, true, row.data);
            }
            placementValues = resolved;
            for (String column : new String[] {"_data", "relative_path", "_display_name",
                    "mime_type", "is_pending", "is_trashed", "date_expires", "volume_name",
                    "bucket_id", "bucket_display_name"}) {
                if (placementValues.containsKey(column)) putObject(values, column, placementValues.get(column));
                else if (column.equals("date_expires")) values.putNull(column);
            }
            String type = values.getAsString("mime_type");
            if (type != null) values.put("media_type", resolveMediaType(type));
            File after = new File(values.getAsString("_data"));
            if (before != null && !before.getPath().equals(after.getPath()) && before.exists()) {
                // A case-only rename names the same file on a case-insensitive volume.
                if (!before.getPath().equalsIgnoreCase(after.getPath()) && after.exists()) {
                    throw new IllegalStateException("Failed to rename " + before + " to " + after
                            + ": target exists");
                }
                ops.move(row.id, before, after);
            }
            values.put("is_download", isInDownloads(values.getAsString("relative_path")) ? 1 : 0);
        }
        values.put("generation_modified", generation);
        if (publishing || placement) {
            // Publishing: a blocking scan refreshes the metadata from the file.
            File file = new File(values.containsKey("_data") ? values.getAsString("_data") : row.data);
            values.putAll(fileMetadata(file, values.containsKey("mime_type")
                    ? values.getAsString("mime_type") : currentPlacement(row.id).getAsString("mime_type")));
        }
        StringBuilder sql = new StringBuilder("UPDATE files SET ");
        List<Object> args = new ArrayList<>();
        for (String key : values.keySet()) {
            if (!args.isEmpty()) sql.append(',');
            sql.append(key).append("=?");
            args.add(values.get(key));
        }
        sql.append(" WHERE _id=?");
        args.add(row.id);
        return android.database.DatabaseUtils.executeUpdateDelete(mDb, sql.toString(),
                args.toArray()) > 0;
    }

    private ContentValues currentPlacement(long id) {
        ContentValues values = new ContentValues();
        try (Cursor c = mDb.rawQuery("SELECT relative_path,_display_name,mime_type,is_pending,"
                + "is_trashed,date_expires FROM files WHERE _id=?", new String[] {Long.toString(id)})) {
            if (c.moveToFirst()) {
                values.put("relative_path", c.getString(0));
                values.put("_display_name", c.getString(1));
                values.put("mime_type", c.getString(2));
                values.put("is_pending", c.getInt(3));
                values.put("is_trashed", c.getInt(4));
                if (!c.isNull(5)) values.put("date_expires", c.getLong(5));
            }
        }
        return values;
    }

    // ---- delete ------------------------------------------------------------------------

    int delete(Caller caller, Uri uri, QueryArgs args) {
        int match = match(uri);
        requireMatch(uri, match);
        String volume;
        try {
            volume = volume(uri);
        } catch (VolumeNotFoundException e) {
            if (!caller.self() && caller.targetSdk() < 29) return 0;
            throw e;
        }
        Access access = new Access(caller);
        if (match == IMAGES_MEDIA_ID || match == VIDEO_MEDIA_ID || match == AUDIO_MEDIA_ID) {
            enforceItemWrite(access, uri, match);
        }
        List<Integer> types = new ArrayList<>();
        synchronized (mLock) {
            SQLiteQueryBuilder qb = builder(access, true, uri, match, args.matchPending,
                    args.matchTrashed, args.matchFavorite);
            List<Row> rows = rows(qb, args.selection, args.selectionArgs);
            // Preflight the whole selection before deleting any file: a private
            // row must not cause a partially applied collection delete.
            for (Row row : rows) enforcePrivateStorageAccess(caller, row.data);
            FileOperations ops = new FileOperations();
            try {
                for (Row row : rows) {
                    // deleteRecursive: the file goes with its row.
                    if (row.data != null) {
                        File file = new File(row.data);
                        if (file.isFile() && !ops.delete(row.id, file)) {
                            SystemServer.log("media: failed to delete " + file);
                            continue;
                        }
                    }
                    deleteRow(row.id);
                    types.add(row.mediaType);
                }
            } catch (RuntimeException | Error e) {
                // A deleted file cannot come back; the journal makes the next start
                // forget its row (recoverFileOperations).
                ops.abort();
                if (!types.isEmpty()) notifyCollections(volume, types);
                throw e;
            }
            ops.finish();
        }
        if (!types.isEmpty()) notifyCollections(volume, types);
        return types.size();
    }

    // ---- openFile ----------------------------------------------------------------------

    /**
     * MediaProvider.openFileAndEnforcePathPermissionsHelper: the item must exist, be
     * readable (writable for a write mode) by the caller, and a pending or trashed item
     * is only for its owner. A caller that is not the owner and lacks
     * ACCESS_MEDIA_LOCATION gets location metadata redacted on Android; Gesher has no
     * redactor, so it serves such a file only when it carries no location metadata
     * ({@link LocationMetadata}) and otherwise refuses as AOSP does for an original.
     */
    FileTarget openFileTarget(Caller caller, Uri uri, boolean forWrite) throws FileNotFoundException {
        return resolveFileTarget(caller, uri, forWrite, false);
    }

    /**
     * Resolves images and videos for pixel-only encoding, with the same read, state and
     * path checks as openFile. The provider never returns this source file to the client.
     */
    FileTarget thumbnailTarget(Caller caller, Uri uri) throws FileNotFoundException {
        FileTarget target = resolveFileTarget(caller, uri, false, true);
        if (target.mediaType != MEDIA_TYPE_IMAGE && target.mediaType != MEDIA_TYPE_VIDEO) {
            throw new FileNotFoundException("No image or video thumbnail for " + uri);
        }
        return target;
    }

    private FileTarget resolveFileTarget(Caller caller, Uri uri, boolean forWrite,
            boolean thumbnail) throws FileNotFoundException {
        int match = match(uri);
        if (match < 0 || !isItem(match)) throw new FileNotFoundException("Invalid URI " + uri);
        volume(uri);
        Access access = new Access(caller);
        long id = ContentUris.parseId(uri);
        String data, mime, owner;
        boolean pendingOrTrashed;
        int mediaType;
        synchronized (mLock) {
            // queryForSingleItem as MediaProvider itself, within the URI's collection.
            SQLiteQueryBuilder self = builder(new Access(SELF), false, uri, match, MATCH_INCLUDE,
                    MATCH_INCLUDE, MATCH_INCLUDE);
            if (rows(self, null, null).isEmpty()) throw new FileNotFoundException("No item at " + uri);
            try (Cursor c = mDb.rawQuery("SELECT _data, mime_type, owner_package_name, is_pending,"
                    + " is_trashed, media_type FROM files WHERE _id=?", new String[] {Long.toString(id)})) {
                if (!c.moveToFirst()) throw new FileNotFoundException("No item at " + uri);
                data = c.getString(0);
                mime = c.getString(1);
                owner = c.getString(2);
                pendingOrTrashed = c.getInt(3) != 0 || c.getInt(4) != 0;
                mediaType = c.isNull(5) ? MEDIA_TYPE_NONE : c.getInt(5);
            }
            if (isEmpty(data)) throw new FileNotFoundException("Missing path for " + uri);
            // checkAccess -> enforceCallingPermission
            if (!access.global(uri, match, forWrite)) {
                SQLiteQueryBuilder qb = builder(access, forWrite, uri, match, MATCH_INCLUDE,
                        MATCH_INCLUDE, MATCH_INCLUDE);
                if (rows(qb, null, null).isEmpty()) {
                    throw new SecurityException(caller.packageName() + " has no access to " + uri);
                }
            }
        }
        boolean callerIsOwner = !access.self && owner != null
                && Arrays.asList(caller.sharedPackages()).contains(owner);
        if (pendingOrTrashed && owner != null && !access.self && !callerIsOwner) {
            throw new IllegalStateException(
                    "Only owner is able to interact with pending/trashed item " + uri);
        }
        File file = new File(data);
        if (relativeToVolume(file.getPath()) == null) {
            throw new FileNotFoundException("Item " + uri + " is outside the shared volume");
        }
        enforcePrivateStorageAccess(caller, data);
        boolean redactionNeeded = !access.self && !callerIsOwner && !access.manager
                && !caller.permission(ACCESS_MEDIA_LOCATION)
                && (mediaType == MEDIA_TYPE_IMAGE || mediaType == MEDIA_TYPE_VIDEO);
        if (redactionNeeded && !forWrite && !thumbnail && LocationMetadata.mayContain(file)) {
            throw new UnsupportedOperationException("Caller must hold ACCESS_MEDIA_LOCATION "
                    + "permission to access original: " + uri + " carries location metadata "
                    + "and this device cannot redact it");
        }
        return new FileTarget(id, file, mime, mediaType);
    }

    static final String ACCESS_MEDIA_LOCATION = "android.permission.ACCESS_MEDIA_LOCATION";

    /** MediaProvider's own identity (clearLocalCallingIdentity). */
    private static final Caller SELF = new Caller() {
        @Override public boolean self() { return true; }
        @Override public String packageName() { return "android"; }
        @Override public int uid() { return 1000; }
        @Override public String[] sharedPackages() { return new String[] {"android"}; }
        @Override public int targetSdk() { return Integer.MAX_VALUE; }
        @Override public boolean permission(String permission) { return true; }
        @Override public int opMode(String op) { return MODE_ALLOWED; }
        @Override public boolean legacyStorageOp() { return false; }
        @Override public boolean uriGranted(Uri uri, boolean write) { return true; }
        @Override public boolean canSeePackage(String packageName) { return true; }
    };

    /** After a writable descriptor closes: size, date_modified and metadata from the file. */
    void fileWritten(long id) {
        int mediaType;
        String volume = VOLUME_EXTERNAL_PRIMARY;
        synchronized (mLock) {
            String data, mime;
            try (Cursor c = mDb.rawQuery("SELECT _data, mime_type, media_type FROM files WHERE _id=?",
                    new String[] {Long.toString(id)})) {
                if (!c.moveToFirst()) return;
                data = c.getString(0);
                mime = c.getString(1);
                mediaType = c.isNull(2) ? MEDIA_TYPE_NONE : c.getInt(2);
            }
            ContentValues values = fileMetadata(new File(data), mime);
            values.put("generation_modified", nextGeneration());
            StringBuilder sql = new StringBuilder("UPDATE files SET ");
            List<Object> args = new ArrayList<>();
            for (String key : values.keySet()) {
                if (!args.isEmpty()) sql.append(',');
                sql.append(key).append("=?");
                args.add(values.get(key));
            }
            sql.append(" WHERE _id=?");
            args.add(id);
            android.database.DatabaseUtils.executeUpdateDelete(mDb, sql.toString(), args.toArray());
        }
        notifyFor(volume, mediaType, id);
    }

    /**
     * Metadata read from the file itself: size, modification time and, for images whose
     * header Gesher parses (PNG, JPEG, GIF, BMP, WebP), width and height. Nothing is
     * guessed: unknown values stay NULL.
     */
    static ContentValues fileMetadata(File file, String mimeType) {
        ContentValues values = new ContentValues();
        if (!file.isFile()) {
            values.putNull("_size");
            return values;
        }
        values.put("_size", file.length());
        values.put("date_modified", file.lastModified() / 1000);
        if (resolveMediaType(mimeType) == MEDIA_TYPE_IMAGE) {
            int[] size = ImageHeaders.dimensions(file);
            if (size != null) {
                values.put("width", size[0]);
                values.put("height", size[1]);
            } else {
                values.putNull("width");
                values.putNull("height");
            }
        }
        return values;
    }

    // ---- scanner (ModernMediaScanner.scanFile for one file) -------------------------------

    /**
     * MediaProvider.getResultForScanFile: index (or refresh, or forget) one file under the
     * volume; returns its content URI in the matching collection, or null when the file is
     * not indexable (outside the volume, hidden, .nomedia, or missing).
     */
    Uri scanFile(File requested) {
        String relative = relativeToVolume(requested.getPath());
        if (relative == null || relative.isEmpty()) return null;
        File file = new File(volumeRoot(), relative);
        String name = file.getName();
        if (".nomedia".equals(name)) return null;
        // ModernMediaScanner.shouldScanDirectory: FileUtils.shouldBeInvisible directories
        // (and everything below them) are never scanned.
        String parentPath = relative.contains("/") ? relative.substring(0, relative.lastIndexOf('/')) : "";
        for (String dir = parentPath; !dir.isEmpty();
                dir = dir.contains("/") ? dir.substring(0, dir.lastIndexOf('/')) : "") {
            if (PATTERN_INVISIBLE.matcher(dir).matches()) return null;
        }
        Long existing = null;
        int existingType = MEDIA_TYPE_NONE;
        synchronized (mLock) {
            try (Cursor c = mDb.rawQuery("SELECT _id, media_type FROM files WHERE _data=?",
                    new String[] {file.getPath()})) {
                if (c.moveToFirst()) {
                    existing = c.getLong(0);
                    existingType = c.isNull(1) ? MEDIA_TYPE_NONE : c.getInt(1);
                }
            }
            if (!file.exists()) {
                // A scan of a deleted file removes its stale row.
                if (existing != null) {
                    deleteRow(existing);
                }
            }
        }
        if (!file.exists()) {
            if (existing != null) notifyFor(VOLUME_EXTERNAL_PRIMARY, existingType, existing);
            return null;
        }
        if (file.isDirectory()) return null; // directories are not indexed in this slice
        boolean hidden = isHidden(file);
        String mimeType = resolveMimeType(file);
        int mediaType = hidden ? MEDIA_TYPE_NONE : resolveMediaType(mimeType);
        ContentValues values = new ContentValues();
        values.put("_data", file.getPath());
        computeValuesFromData(values);
        values.put("mime_type", mimeType);
        values.put("media_type", mediaType);
        values.put("format", resolveFormatCode(mimeType));
        values.put("title", extractFileName(file.getName()));
        values.put("is_download", isInDownloads(values.getAsString("relative_path")) ? 1 : 0);
        values.putAll(fileMetadata(file, mimeType));
        long id;
        synchronized (mLock) {
            mDb.beginTransaction();
            try {
                long generation = nextGeneration();
                values.put("generation_modified", generation);
                if (existing != null) {
                    StringBuilder sql = new StringBuilder("UPDATE files SET ");
                    List<Object> args = new ArrayList<>();
                    for (String key : values.keySet()) {
                        if (!args.isEmpty()) sql.append(',');
                        sql.append(key).append("=?");
                        args.add(values.get(key));
                    }
                    sql.append(" WHERE _id=?");
                    args.add(existing);
                    android.database.DatabaseUtils.executeUpdateDelete(mDb, sql.toString(), args.toArray());
                    id = existing;
                } else {
                    values.put("generation_added", generation);
                    values.put("date_added", mHost.currentTimeMillis() / 1000);
                    values.put("_user_id", 0);
                    values.put("_modifier", 2); // FileColumns._MODIFIER_SCHEMA_SCAN-like: scanner
                    // A scanned file has no owner unless one was recorded before.
                    id = insertRow(values);
                }
                mDb.setTransactionSuccessful();
            } finally {
                mDb.endTransaction();
            }
        }
        notifyFor(VOLUME_EXTERNAL_PRIMARY, mediaType, id);
        switch (mediaType) {
            case MEDIA_TYPE_AUDIO: return itemUri(VOLUME_EXTERNAL_PRIMARY, AUDIO_MEDIA, id);
            case MEDIA_TYPE_VIDEO: return itemUri(VOLUME_EXTERNAL_PRIMARY, VIDEO_MEDIA, id);
            case MEDIA_TYPE_IMAGE: return itemUri(VOLUME_EXTERNAL_PRIMARY, IMAGES_MEDIA, id);
            default: return itemUri(VOLUME_EXTERNAL_PRIMARY, FILES, id);
        }
    }

    /** FileUtils.shouldFileBeHidden: a hidden name, or a hidden (or .nomedia) directory. */
    private boolean isHidden(File file) {
        if (file.getName().startsWith(".") && !PATTERN_EXPIRES_FILE.matcher(file.getName()).matches()) {
            return true;
        }
        File root = volumeRoot();
        for (File dir = file.getParentFile(); dir != null && !dir.equals(root); dir = dir.getParentFile()) {
            if (dir.getName().startsWith(".")) return true;
            if (new File(dir, ".nomedia").exists() && !isDefaultTopLevel(dir)) return true;
        }
        return false;
    }

    private boolean isDefaultTopLevel(File dir) {
        if (!volumeRoot().equals(dir.getParentFile())) return false;
        for (String name : DEFAULT_FOLDER_NAMES) if (name.equalsIgnoreCase(dir.getName())) return true;
        return false;
    }

    /** FileUtils.DEFAULT_FOLDER_NAMES. */
    private static final String[] DEFAULT_FOLDER_NAMES = {
            Environment.DIRECTORY_MUSIC, Environment.DIRECTORY_PODCASTS,
            Environment.DIRECTORY_RINGTONES, Environment.DIRECTORY_ALARMS,
            Environment.DIRECTORY_NOTIFICATIONS, Environment.DIRECTORY_PICTURES,
            Environment.DIRECTORY_MOVIES, Environment.DIRECTORY_DOWNLOADS,
            Environment.DIRECTORY_DCIM, Environment.DIRECTORY_DOCUMENTS,
            Environment.DIRECTORY_AUDIOBOOKS, Environment.DIRECTORY_RECORDINGS,
    };

    // ---- package removal and expiry ----------------------------------------------------

    /**
     * MediaProvider.onPackageOrphaned: rows under the package's own Android/media
     * directory whose file is gone are deleted; every other row the package owned keeps
     * its media and loses its owner.
     */
    void packageOrphaned(String packageName) {
        List<Integer> types = new ArrayList<>();
        synchronized (mLock) {
            mDb.beginTransaction();
            try {
                try (Cursor c = mDb.rawQuery("SELECT _id, _data, media_type FROM files WHERE "
                        + "relative_path LIKE ? ESCAPE '\\' AND owner_package_name=?",
                        new String[] {"Android/media/" + escapeForLike(packageName) + "/%", packageName})) {
                    while (c.moveToNext()) {
                        if (c.isNull(1) || !new File(c.getString(1)).exists()) {
                            deleteRow(c.getLong(0));
                            types.add(c.isNull(2) ? MEDIA_TYPE_NONE : c.getInt(2));
                        }
                    }
                }
                try (Cursor c = mDb.rawQuery("SELECT DISTINCT media_type FROM files WHERE owner_package_name=?",
                        new String[] {packageName})) {
                    while (c.moveToNext()) types.add(c.isNull(0) ? MEDIA_TYPE_NONE : c.getInt(0));
                }
                int orphaned = android.database.DatabaseUtils.executeUpdateDelete(mDb,
                        "UPDATE files SET owner_package_name=NULL, generation_modified="
                                + "(SELECT generation+1 FROM local_metadata) WHERE owner_package_name=?",
                        new Object[] {packageName});
                if (orphaned != 0) mDb.execSQL("UPDATE local_metadata SET generation=generation+1");
                mDb.setTransactionSuccessful();
            } finally {
                mDb.endTransaction();
            }
        }
        if (!types.isEmpty()) notifyCollections(VOLUME_EXTERNAL_PRIMARY, types);
    }

    private static String escapeForLike(String arg) {
        StringBuilder sb = new StringBuilder();
        for (char c : arg.toCharArray()) {
            if (c == '%' || c == '_' || c == '\\') sb.append('\\');
            sb.append(c);
        }
        return sb.toString();
    }

    /**
     * MediaProvider.deleteOrExtendExpiredItems: pending or trashed items whose
     * date_expires passed within the last week are deleted with their file; older ones
     * are extended by a week (protection against a wrong clock). AOSP runs this during
     * idle maintenance; Gesher runs it when the provider starts and after package removal.
     */
    int deleteExpired() {
        long now = mHost.currentTimeMillis() / 1000;
        long expiredOneWeek = (mHost.currentTimeMillis() - 7 * 24 * 3600 * 1000L) / 1000;
        long extended = now + DEFAULT_DURATION_EXTENDED / 1000;
        List<Integer> types = new ArrayList<>();
        int deleted = 0;
        synchronized (mLock) {
            List<long[]> items = new ArrayList<>();
            List<String> paths = new ArrayList<>();
            try (Cursor c = mDb.rawQuery("SELECT _id, date_expires, _data, media_type FROM files "
                    + "WHERE date_expires < ? AND (is_pending=1 OR is_trashed=1) AND volume_name=?",
                    new String[] {Long.toString(now), VOLUME_EXTERNAL_PRIMARY})) {
                while (c.moveToNext()) {
                    items.add(new long[] {c.getLong(0), c.getLong(1), c.isNull(3) ? 0 : c.getInt(3)});
                    paths.add(c.getString(2));
                }
            }
            FileOperations ops = new FileOperations();
            try {
                mDb.beginTransaction();
                try {
                    for (int i = 0; i < items.size(); i++) {
                        long[] item = items.get(i);
                        File file = paths.get(i) == null ? null : new File(paths.get(i));
                        if (item[1] > expiredOneWeek) {
                            if (file != null && file.exists() && !ops.delete(item[0], file)) continue;
                            deleteRow(item[0]);
                            deleted++;
                            types.add((int) item[2]);
                        } else if (file != null) {
                            Matcher m = PATTERN_EXPIRES_FILE.matcher(file.getName());
                            if (!m.matches()) continue;
                            File renamed = new File(file.getParentFile(), "." + m.group(1) + "-"
                                    + (extended + i) + "-" + m.group(3));
                            if (file.exists() && !ops.tryMove(item[0], file, renamed)) continue;
                            mDb.execSQL("UPDATE files SET _data=?, date_expires=?, generation_modified=? WHERE _id=?",
                                    new Object[] {renamed.getPath(), extended + i, nextGeneration(), item[0]});
                            types.add((int) item[2]);
                        }
                    }
                    mDb.setTransactionSuccessful();
                } finally {
                    mDb.endTransaction();
                }
            } catch (RuntimeException | Error e) {
                ops.abort();
                throw e;
            }
            ops.finish();
        }
        if (!types.isEmpty()) notifyCollections(VOLUME_EXTERNAL_PRIMARY, types);
        return deleted;
    }

    // ---- file operations and their journal ---------------------------------------------

    private static final int OP_MOVE = 1, OP_DELETE = 2;
    private static final String JOURNAL_SUFFIX = ".journal";

    /**
     * The file moves and deletes one index operation makes before its database change
     * commits. AOSP's MediaProvider renames or deletes the file and then writes the row;
     * when the row write fails or the process dies in between, Android's media scanner
     * later reconciles the volume (ModernMediaScanner.reconcileAndClean deletes rows
     * whose file it did not find, and the moved file is scanned as a new item). Gesher
     * has no full volume scan, so it keeps the row and the file together itself:
     * <ul>
     * <li>every completed move is reversed when the transaction does not commit, so a
     *     rolled-back row still names its file;</li>
     * <li>each move or delete is first appended to a journal file and forced to disk;
     *     the journal is removed once the database change committed (or the moves were
     *     reversed). A journal found at startup ({@link #recoverFileOperations}) means
     *     the system server stopped in between, and is resolved against the rows.</li>
     * </ul>
     */
    private final class FileOperations {
        private final List<File[]> mMoves = new ArrayList<>();
        private File mJournal;
        private FileOutputStream mStream;
        private DataOutputStream mOut;
        private boolean mDeleted;

        /** Renames {@code before} to {@code after} or throws IllegalStateException. */
        void move(long id, File before, File after) {
            if (!tryMove(id, before, after)) {
                throw new IllegalStateException("Failed to rename " + before + " to " + after);
            }
        }

        boolean tryMove(long id, File before, File after) {
            record(OP_MOVE, id, before, after);
            if (!before.renameTo(after)) return false;
            mMoves.add(new File[] {before, after});
            return true;
        }

        boolean delete(long id, File file) {
            record(OP_DELETE, id, file, file);
            if (!file.delete()) return false;
            mDeleted = true;
            return true;
        }

        /** The database change committed: the journal is no longer needed. */
        void finish() {
            close(true);
        }

        /**
         * The database change did not commit: moves are reversed, newest first. Deleted
         * files cannot be restored, so their journal stays for the next start.
         */
        void abort() {
            boolean restored = true;
            for (int i = mMoves.size() - 1; i >= 0; i--) {
                File before = mMoves.get(i)[0];
                File after = mMoves.get(i)[1];
                if (!after.renameTo(before)) {
                    restored = false;
                    SystemServer.log("media: failed to move " + after + " back to " + before);
                }
            }
            mMoves.clear();
            close(restored && !mDeleted);
        }

        private void record(int op, long id, File first, File second) {
            File directory = mHost.journalDirectory();
            if (directory == null) return;
            try {
                if (mOut == null) {
                    if (!directory.isDirectory() && !directory.mkdirs()) {
                        throw new IOException("cannot create " + directory);
                    }
                    mJournal = File.createTempFile("ops-", JOURNAL_SUFFIX, directory);
                    mStream = new FileOutputStream(mJournal);
                    mOut = new DataOutputStream(mStream);
                }
                mOut.writeInt(op);
                mOut.writeLong(id);
                mOut.writeUTF(first.getPath());
                mOut.writeUTF(second.getPath());
                mOut.flush();
                mStream.getFD().sync();
            } catch (IOException e) {
                // Nothing on the volume changed yet; the caller's transaction rolls back.
                throw new IllegalStateException("Failed to journal a media file operation", e);
            }
        }

        private void close(boolean discard) {
            if (mOut == null) return;
            try {
                mOut.close();
            } catch (IOException e) {
                SystemServer.log("media: closing " + mJournal + " failed: " + e);
            }
            mOut = null;
            if (discard && !mJournal.delete()) {
                SystemServer.log("media: failed to remove " + mJournal);
            }
        }
    }

    /**
     * Resolves the journals of operations that did not finish: the system server stopped
     * (or the database failed) after a file move or delete and before the row change was
     * committed. The database is authoritative for what committed:
     * <ul>
     * <li>a move whose row still names the old path is undone, so the row and its owner,
     *     id and state stay as the app last saw them;</li>
     * <li>a delete whose row still names a file that is gone completes by deleting the
     *     row, as ModernMediaScanner's reconcile would.</li>
     * </ul>
     */
    private void recoverFileOperations() {
        File directory = mHost.journalDirectory();
        File[] journals = directory == null ? null
                : directory.listFiles((dir, name) -> name.endsWith(JOURNAL_SUFFIX));
        if (journals == null || journals.length == 0) return;
        Arrays.sort(journals, (x, y) -> Long.compare(x.lastModified(), y.lastModified()));
        for (int j = journals.length - 1; j >= 0; j--) {
            List<Object[]> entries = new ArrayList<>();
            try (DataInputStream in = new DataInputStream(new java.io.BufferedInputStream(
                    new FileInputStream(journals[j])))) {
                while (true) {
                    int op;
                    try {
                        op = in.readInt();
                    } catch (EOFException end) {
                        break;
                    }
                    entries.add(new Object[] {op, in.readLong(), in.readUTF(), in.readUTF()});
                }
            } catch (EOFException | java.io.UTFDataFormatException torn) {
                // A record cut short by the stop: its file operation never started.
            } catch (IOException e) {
                SystemServer.log("media: cannot read " + journals[j] + ": " + e);
                continue;
            }
            boolean resolved = true;
            mDb.beginTransaction();
            try {
                for (int i = entries.size() - 1; i >= 0; i--) {
                    Object[] entry = entries.get(i);
                    long id = (Long) entry[1];
                    File first = new File((String) entry[2]);
                    File second = new File((String) entry[3]);
                    String data = null;
                    boolean exists;
                    try (Cursor c = mDb.rawQuery("SELECT _data FROM files WHERE _id=?",
                            new String[] {Long.toString(id)})) {
                        exists = c.moveToFirst();
                        if (exists) data = c.getString(0);
                    }
                    if (!exists || !first.getPath().equals(data) || first.exists()) continue;
                    if ((Integer) entry[0] == OP_MOVE) {
                        if (second.exists() && !second.renameTo(first)) {
                            resolved = false;
                            SystemServer.log("media: failed to move " + second + " back to " + first);
                        }
                    } else {
                        deleteRow(id);
                    }
                }
                mDb.setTransactionSuccessful();
            } finally {
                mDb.endTransaction();
            }
            if (resolved && !journals[j].delete()) {
                SystemServer.log("media: failed to remove " + journals[j]);
            }
        }
    }

    // ---- version and generation ---------------------------------------------------------

    /**
     * Every deletion changes getGeneration, including scanner cleanup and crash recovery.
     * Keep the row and generation in the same transaction, as DatabaseHelper does on Android.
     * Callers hold mLock; a nested transaction joins an existing package/expiry/recovery batch.
     */
    private void deleteRow(long id) {
        mDb.beginTransaction();
        try {
            int count = android.database.DatabaseUtils.executeUpdateDelete(mDb,
                    "DELETE FROM files WHERE _id=?", new Object[] {id});
            if (count != 0) mDb.execSQL("UPDATE local_metadata SET generation=generation+1");
            mDb.setTransactionSuccessful();
        } finally {
            mDb.endTransaction();
        }
    }

    /** DatabaseHelper.getGeneration. */
    long generation(String volumeName) {
        if (!VOLUME_EXTERNAL_PRIMARY.equals(volumeName) && !VOLUME_EXTERNAL.equals(volumeName)) {
            throw new VolumeNotFoundException(volumeName);
        }
        synchronized (mLock) {
            try (Cursor c = mDb.rawQuery("SELECT generation FROM local_metadata", null)) {
                return c.moveToFirst() ? c.getLong(0) : 0;
            }
        }
    }

    /**
     * MediaProvider.getResultForGetVersion: {@code <schema version>:<database UUID>}, or for
     * an API 36+ caller (LOCKDOWN_MEDIASTORE_VERSION) an opaque per-UID digest of the UUID.
     */
    String version(Caller caller, String volumeName) {
        if (!VOLUME_EXTERNAL_PRIMARY.equals(volumeName) && !VOLUME_EXTERNAL.equals(volumeName)) {
            throw new VolumeNotFoundException(volumeName);
        }
        String uuid;
        synchronized (mLock) {
            try (Cursor c = mDb.rawQuery("SELECT uuid FROM gesher_metadata", null)) {
                uuid = c.moveToFirst() ? c.getString(0) : "";
            }
        }
        if (!caller.self() && caller.targetSdk() >= TARGET_B) {
            try {
                byte[] digest = MessageDigest.getInstance("SHA-256").digest(
                        (uuid + caller.uid()).getBytes(StandardCharsets.UTF_8));
                StringBuilder hex = new StringBuilder();
                for (int i = 0; i < 8; i++) hex.append(String.format(Locale.ROOT, "%02x", digest[i]));
                return hex.toString();
            } catch (NoSuchAlgorithmException e) {
                throw new IllegalStateException(e);
            }
        }
        return DB_VERSION + ":" + uuid;
    }

    private long nextGeneration() {
        mDb.execSQL("UPDATE local_metadata SET generation=generation+1");
        try (Cursor c = mDb.rawQuery("SELECT generation FROM local_metadata", null)) {
            return c.moveToFirst() ? c.getLong(0) : 0;
        }
    }

    // ---- createRequest validation (MediaProvider.createRequest) ----------------------------

    /**
     * Validates a create_*_request call as AOSP does before it builds the consent
     * PendingIntent; the caller then reports that no consent screen exists.
     */
    static void validateCreateRequest(String method, List<Uri> uris, Set<String> valueKeys) {
        if (uris.isEmpty()) throw new IllegalArgumentException("No URIs");
        for (Uri uri : uris) {
            int match = match(uri);
            switch (match) {
                case IMAGES_MEDIA_ID: case AUDIO_MEDIA_ID: case VIDEO_MEDIA_ID:
                    break;
                case FILES_ID:
                    throw new IllegalArgumentException("All requested items must be Media items");
                default:
                    throw new IllegalArgumentException(
                            "All requested items must be referenced by specific ID");
            }
        }
        List<String> allowed;
        switch (method) {
            case "create_favorite_request": allowed = Collections.singletonList("is_favorite"); break;
            case "create_trash_request": allowed = Collections.singletonList("is_trashed"); break;
            default: allowed = Collections.emptyList(); break;
        }
        if (valueKeys != null) {
            for (String key : valueKeys) {
                if (!allowed.contains(key)) throw new IllegalArgumentException("Invalid column " + key);
            }
        }
    }

    // ---- notifications -----------------------------------------------------------------

    private void notifyFor(String volume, Integer mediaType, long id) {
        List<Integer> types = new ArrayList<>();
        types.add(mediaType == null ? MEDIA_TYPE_NONE : mediaType);
        notifyCollections(volume, types);
    }

    /** MediaProvider.notifyChange: the collection of each changed media type, and Files. */
    private void notifyCollections(String volume, List<Integer> types) {
        Set<Integer> seen = new LinkedHashSet<>(types);
        for (String name : new String[] {volume, VOLUME_EXTERNAL}) {
            for (int type : seen) {
                switch (type) {
                    case MEDIA_TYPE_IMAGE: mHost.notifyChange(contentUri(name, IMAGES_MEDIA)); break;
                    case MEDIA_TYPE_VIDEO: mHost.notifyChange(contentUri(name, VIDEO_MEDIA)); break;
                    case MEDIA_TYPE_AUDIO: mHost.notifyChange(contentUri(name, AUDIO_MEDIA)); break;
                    default: break;
                }
            }
            mHost.notifyChange(contentUri(name, FILES));
            mHost.notifyChange(contentUri(name, DOWNLOADS));
            if (name.equals(VOLUME_EXTERNAL)) break;
        }
    }

    // ---- helpers ------------------------------------------------------------------------

    private static boolean isEmpty(String s) { return s == null || s.isEmpty(); }

    private static void putObject(ContentValues values, String key, Object value) {
        if (value == null) values.putNull(key);
        else if (value instanceof Long) values.put(key, (Long) value);
        else if (value instanceof Integer) values.put(key, (Integer) value);
        else if (value instanceof Boolean) values.put(key, (Boolean) value);
        else values.put(key, value.toString());
    }

    private static boolean asBoolean(ContentValues values, String key, boolean def) {
        Object value = values.get(key);
        if (value == null) return def;
        if (value instanceof Boolean) return (Boolean) value;
        if (value instanceof Number) return ((Number) value).intValue() != 0;
        String text = value.toString();
        return "1".equals(text) || "true".equalsIgnoreCase(text);
    }

    private static long asLong(ContentValues values, String key, long def) {
        Long value = values.getAsLong(key);
        return value == null ? def : value;
    }

    /** Image header parsing for width/height (no decoding, no guessing). */
    static final class ImageHeaders {
        private ImageHeaders() {}

        static int[] dimensions(File file) {
            byte[] head = new byte[64 * 1024];
            int n;
            try (InputStream in = new java.io.FileInputStream(file)) {
                n = in.readNBytes(head, 0, head.length);
            } catch (IOException e) {
                return null;
            }
            return dimensions(head, n);
        }

        static int[] dimensions(byte[] b, int n) {
            if (n >= 24 && (b[0] & 0xff) == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G'
                    && b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R') {
                return positive(be32(b, 16), be32(b, 20));
            }
            if (n >= 10 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') {
                return positive(le16(b, 6), le16(b, 8));
            }
            if (n >= 26 && b[0] == 'B' && b[1] == 'M') {
                return positive(le32(b, 18), Math.abs(le32(b, 22)));
            }
            if (n >= 30 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F'
                    && b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') {
                if (b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == ' ') {
                    return positive(le16(b, 26) & 0x3fff, le16(b, 28) & 0x3fff);
                }
                if (b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == 'L') {
                    int bits = le32(b, 21);
                    return positive((bits & 0x3fff) + 1, ((bits >> 14) & 0x3fff) + 1);
                }
                if (b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == 'X') {
                    return positive(le24(b, 24) + 1, le24(b, 27) + 1);
                }
                return null;
            }
            if (n >= 4 && (b[0] & 0xff) == 0xFF && (b[1] & 0xff) == 0xD8) {
                int i = 2;
                while (i + 9 < n) {
                    if ((b[i] & 0xff) != 0xFF) return null;
                    int marker = b[i + 1] & 0xff;
                    if (marker == 0xFF) { i++; continue; }
                    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
                        i += 2;
                        continue;
                    }
                    int length = ((b[i + 2] & 0xff) << 8) | (b[i + 3] & 0xff);
                    if (length < 2) return null;
                    boolean sof = marker >= 0xC0 && marker <= 0xCF
                            && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
                    if (sof) {
                        int height = ((b[i + 5] & 0xff) << 8) | (b[i + 6] & 0xff);
                        int width = ((b[i + 7] & 0xff) << 8) | (b[i + 8] & 0xff);
                        return positive(width, height);
                    }
                    i += 2 + length;
                }
            }
            return null;
        }

        private static int[] positive(int w, int h) { return w > 0 && h > 0 ? new int[] {w, h} : null; }
        private static int be32(byte[] b, int o) {
            return ((b[o] & 0xff) << 24) | ((b[o + 1] & 0xff) << 16) | ((b[o + 2] & 0xff) << 8) | (b[o + 3] & 0xff);
        }
        private static int le16(byte[] b, int o) { return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8); }
        private static int le24(byte[] b, int o) { return le16(b, o) | ((b[o + 2] & 0xff) << 16); }
        private static int le32(byte[] b, int o) { return le24(b, o) | ((b[o + 3] & 0xff) << 24); }
    }

    /**
     * Whether a file may carry location metadata that Android would redact for a caller
     * without ACCESS_MEDIA_LOCATION (RedactionUtils: EXIF/XMP GPS in images, location
     * boxes in ISO media). Formats whose metadata is not parsed here answer true, so an
     * unverified file is refused rather than served unredacted.
     */
    static final class LocationMetadata {
        private LocationMetadata() {}
        private static final int MAX_METADATA = 64 * 1024 * 1024;

        static boolean mayContain(File file) {
            try (java.io.RandomAccessFile in = new java.io.RandomAccessFile(file, "r")) {
                byte[] head = new byte[16];
                int n = in.read(head);
                if (n < 4) return false;
                if ((head[0] & 0xff) == 0xFF && (head[1] & 0xff) == 0xD8) return jpeg(in);
                if ((head[0] & 0xff) == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return png(in);
                if (head[0] == 'G' && head[1] == 'I' && head[2] == 'F') return false;
                if (head[0] == 'B' && head[1] == 'M') return false;
                if (n >= 12 && head[0] == 'R' && head[1] == 'I' && head[2] == 'F' && head[3] == 'F'
                        && head[8] == 'W' && head[9] == 'E' && head[10] == 'B' && head[11] == 'P') {
                    return webp(in);
                }
                if (n >= 8 && head[4] == 'f' && head[5] == 't' && head[6] == 'y' && head[7] == 'p') {
                    return isoMedia(in);
                }
                return true;
            } catch (IOException | RuntimeException e) {
                return true;
            }
        }

        private static boolean jpeg(java.io.RandomAccessFile in) throws IOException {
            long pos = 2;
            while (pos + 4 <= in.length()) {
                in.seek(pos);
                int ff = in.read(), marker = in.read();
                if (ff != 0xFF) return true;
                if (marker == 0xFF) { pos++; continue; }
                if (marker == 0xD9 || marker == 0xDA) return false; // EOI / start of scan
                if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { pos += 2; continue; }
                int length = in.readUnsignedShort();
                if (length < 2) return true;
                if (marker == 0xE1) {
                    byte[] segment = new byte[length - 2];
                    in.readFully(segment);
                    if (startsWith(segment, "Exif\0\0")) {
                        if (tiffHasGps(segment, 6)) return true;
                    } else if (startsWith(segment, "http://ns.adobe.com/xap/1.0/")
                            || startsWith(segment, "http://ns.adobe.com/xmp/extension/")) {
                        if (contains(segment, "GPS") || contains(segment, "Location")) return true;
                    }
                }
                pos += 2 + length;
            }
            return false;
        }

        private static boolean png(java.io.RandomAccessFile in) throws IOException {
            long pos = 8;
            while (pos + 8 <= in.length()) {
                in.seek(pos);
                int length = in.readInt();
                byte[] type = new byte[4];
                in.readFully(type);
                String name = new String(type, StandardCharsets.ISO_8859_1);
                if (length < 0) return true;
                if ("IEND".equals(name)) return false;
                if ("eXIf".equals(name)) {
                    if (length > MAX_METADATA) return true;
                    byte[] exif = new byte[length];
                    in.readFully(exif);
                    if (tiffHasGps(exif, 0)) return true;
                } else if ("iTXt".equals(name) || "tEXt".equals(name) || "zTXt".equals(name)) {
                    if (length > MAX_METADATA) return true;
                    byte[] text = new byte[length];
                    in.readFully(text);
                    // XMP or raw EXIF profiles may hold location; compressed ones are not read.
                    if (startsWith(text, "XML:com.adobe.xmp") || startsWith(text, "Raw profile type")) return true;
                }
                pos += 12L + length;
            }
            return false;
        }

        private static boolean webp(java.io.RandomAccessFile in) throws IOException {
            long pos = 12;
            while (pos + 8 <= in.length()) {
                in.seek(pos);
                byte[] type = new byte[4];
                in.readFully(type);
                long size = Integer.toUnsignedLong(Integer.reverseBytes(in.readInt()));
                String name = new String(type, StandardCharsets.ISO_8859_1);
                if ("EXIF".equals(name) || "XMP ".equals(name)) return true;
                pos += 8 + size + (size & 1);
            }
            return false;
        }

        /** ISO base media (MP4, MOV, 3GP, HEIF): location boxes live in moov or meta. */
        private static boolean isoMedia(java.io.RandomAccessFile in) throws IOException {
            long pos = 0;
            boolean sawMoov = false;
            while (pos + 8 <= in.length()) {
                in.seek(pos);
                long size = Integer.toUnsignedLong(in.readInt());
                byte[] type = new byte[4];
                in.readFully(type);
                long header = 8;
                if (size == 1) { size = in.readLong(); header = 16; }
                else if (size == 0) size = in.length() - pos;
                if (size < header) return true;
                String name = new String(type, StandardCharsets.ISO_8859_1);
                if ("moov".equals(name) || "meta".equals(name)) {
                    if (size - header > MAX_METADATA) return true;
                    byte[] box = new byte[(int) (size - header)];
                    in.readFully(box);
                    if (contains(box, "\u00a9xyz") || contains(box, "loci") || contains(box, "Exif")
                            || contains(box, "GPS") || contains(box, "com.apple.quicktime.location")) {
                        return true;
                    }
                    if ("moov".equals(name)) sawMoov = true;
                }
                pos += size;
            }
            return !sawMoov;
        }

        /** An EXIF TIFF structure at {@code base} with a non-empty GPS IFD (tag 0x8825). */
        private static boolean tiffHasGps(byte[] b, int base) {
            if (b.length < base + 8) return false;
            boolean le;
            if (b[base] == 'I' && b[base + 1] == 'I') le = true;
            else if (b[base] == 'M' && b[base + 1] == 'M') le = false;
            else return true;
            long ifd = u32(b, base + 4, le);
            if (ifd <= 0 || base + ifd + 2 > b.length) return true;
            int at = (int) (base + ifd);
            int count = u16(b, at, le);
            for (int i = 0; i < count; i++) {
                int entry = at + 2 + i * 12;
                if (entry + 12 > b.length) return true;
                if (u16(b, entry, le) == 0x8825) {
                    long gps = u32(b, entry + 8, le);
                    if (base + gps + 2 > b.length) return true;
                    return u16(b, (int) (base + gps), le) > 0;
                }
            }
            return false;
        }

        private static int u16(byte[] b, int o, boolean le) {
            return le ? (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8) : ((b[o] & 0xff) << 8) | (b[o + 1] & 0xff);
        }

        private static long u32(byte[] b, int o, boolean le) {
            long v = le ? (b[o] & 0xffL) | ((b[o + 1] & 0xffL) << 8) | ((b[o + 2] & 0xffL) << 16) | ((b[o + 3] & 0xffL) << 24)
                    : ((b[o] & 0xffL) << 24) | ((b[o + 1] & 0xffL) << 16) | ((b[o + 2] & 0xffL) << 8) | (b[o + 3] & 0xffL);
            return v;
        }

        private static boolean startsWith(byte[] data, String prefix) {
            byte[] p = prefix.getBytes(StandardCharsets.ISO_8859_1);
            if (data.length < p.length) return false;
            for (int i = 0; i < p.length; i++) if (data[i] != p[i]) return false;
            return true;
        }

        private static boolean contains(byte[] data, String needle) {
            byte[] p = needle.getBytes(StandardCharsets.ISO_8859_1);
            outer:
            for (int i = 0; i + p.length <= data.length; i++) {
                for (int j = 0; j < p.length; j++) if (data[i + j] != p[j]) continue outer;
                return true;
            }
            return false;
        }
    }
}
