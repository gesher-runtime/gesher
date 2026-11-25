/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.app.ContentProviderHolder;
import android.content.AttributionSource;
import android.content.ClipData;
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.graphics.Point;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Environment;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.ParcelFileDescriptor;
import android.os.Process;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/**
 * {@code content://media}: the MediaStore provider (AOSP's MediaProvider module, which
 * runs in its own process) served from the system server over the shared volume
 * {@code $ANDROID_DATA/media/0}. {@link MediaStoreIndex} owns the rows and every access
 * rule; this class authenticates the Binder caller, maps the ContentProvider API and its
 * {@code call} protocol onto the index, and hands out file descriptors.
 *
 * <p>Not implemented, and therefore not reported: the user-consent screen behind
 * {@code createWriteRequest}/{@code createDeleteRequest}/trash/favorite requests (they
 * fail with UnsupportedOperationException after AOSP's argument validation), audio artwork,
 * the internal volume, playlists, genres, albums and artists tables, FUSE indexing of
 * files written without the provider, and location redaction (see
 * {@link MediaStoreIndex#openFileTarget}). docs/media-provider.md lists the rest.
 */
final class MediaStoreProvider extends ContentProvider {
    static final String AUTHORITY = MediaStoreIndex.AUTHORITY;
    static final String CLASS_NAME = "com.android.providers.media.MediaProvider";

    private final PackageManagerService mPm;
    private final UriGrants mGrants;
    private final CallerIdentity mCallers;
    private final ProviderInfo mInfo;
    final ContentProviders.RegisteredProvider registration;
    private final MediaStoreIndex mIndex;
    /** The journal of file moves and deletes awaiting a commit; null without state. */
    private final File mJournal;
    private final java.util.concurrent.ThreadPoolExecutor mMaintenance =
            new java.util.concurrent.ThreadPoolExecutor(1, 1, 30, java.util.concurrent.TimeUnit.SECONDS,
                    new java.util.concurrent.LinkedBlockingQueue<>(), task -> {
                        Thread thread = new Thread(task, "gesher-media-maintenance");
                        thread.setDaemon(true);
                        return thread;
                    });
    {
        mMaintenance.allowCoreThreadTimeOut(true);
    }
    private volatile Handler mCloseHandler;

    /**
     * Creates and registers the provider; {@code stateDir} null keeps the index in memory
     * (a process without persistent device state).
     */
    static MediaStoreProvider create(ActivityManagerService am, PackageManagerService pm,
            File stateDir) {
        // An installed provider that cannot initialize is not an absent media index.
        // Keep existing-file registration denied until the complete startup succeeds.
        SharedStorageOwnership.register(file -> {
            throw new IllegalStateException("MediaStore ownership index is not available");
        });
        SQLiteDatabase db;
        if (stateDir != null) {
            if (!(stateDir.isDirectory() || stateDir.mkdirs())) {
                throw new IllegalStateException("Cannot create the media state directory");
            }
            db = SQLiteDatabase.openOrCreateDatabase(new File(stateDir, "external.db"), null);
        } else {
            db = SQLiteDatabase.create(null);
        }
        MediaStoreProvider provider = new MediaStoreProvider(am, pm, db,
                stateDir == null ? null : new File(stateDir, "external-file-ops"));
        am.providers().setMedia(provider);
        am.addSystemReceiver(Intent.ACTION_MEDIA_SCANNER_SCAN_FILE, provider::onScanFileBroadcast);
        provider.start();
        SharedStorageOwnership.register(provider.mIndex::ownerOf);
        return provider;
    }

    private MediaStoreProvider(ActivityManagerService am, PackageManagerService pm,
            SQLiteDatabase db, File journal) {
        mJournal = journal;
        mPm = pm;
        mGrants = am.uriGrants();
        mCallers = am.callers();
        mInfo = providerInfo();
        registration = new ContentProviders.RegisteredProvider(mInfo, getIContentProvider());
        mIndex = new MediaStoreIndex(db, new Host());
        am.providers().registerProvider(registration);
    }

    /** The manifest declaration of AOSP's MediaProvider (exported, URI grants, no permission). */
    static ProviderInfo providerInfo() {
        ProviderInfo info = new ProviderInfo();
        ApplicationInfo app = new ApplicationInfo();
        app.packageName = "android";
        app.processName = "system";
        app.uid = Process.SYSTEM_UID;
        app.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_PERSISTENT;
        app.enabled = true;
        info.applicationInfo = app;
        info.packageName = app.packageName;
        info.processName = app.processName;
        info.name = CLASS_NAME;
        info.authority = AUTHORITY;
        info.enabled = true;
        info.exported = true;
        info.grantUriPermissions = true;
        info.forceUriPermissions = true;
        info.flags = ProviderInfo.FLAG_SINGLE_USER;
        return info;
    }

    /** The shared volume: StorageService's emulated view of the data directory. */
    static File volumeRoot() {
        return new File(Environment.getDataDirectory(), "media/0");
    }

    private void start() {
        mPm.onPackageRemoval(name -> mMaintenance.execute(() -> {
            try {
                // MediaService: PACKAGE_FULLY_REMOVED -> onPackageOrphaned.
                mIndex.packageOrphaned(name);
                mIndex.deleteExpired();
            } catch (RuntimeException e) {
                SystemServer.log("media: orphaning " + name + " failed: " + e);
            }
        }));
        mMaintenance.execute(() -> {
            try {
                mIndex.deleteExpired();
            } catch (RuntimeException e) {
                SystemServer.log("media: expiring pending items failed: " + e);
            }
        });
    }

    synchronized ContentProviderHolder holder() {
        if (getContext() == null) attachTo(android.app.GesherSettingsContext.get());
        ContentProviderHolder result = new ContentProviderHolder(new ProviderInfo(mInfo));
        result.provider = getIContentProvider();
        result.noReleaseNeeded = true;
        return result;
    }

    private synchronized void attachTo(Context base) {
        if (getContext() != null) return;
        attachInfo(new ContextWrapper(java.util.Objects.requireNonNull(base)) {
            @Override public int checkUriPermission(Uri uri, int pid, int uid, int modes) {
                return mGrants.check(uri, pid, uid, modes, 0);
            }
            @Override public void grantUriPermission(String target, Uri uri, int modes) {
                mGrants.grantFromProvider(registration, target, uri, modes);
            }
            @Override public void revokeUriPermission(Uri uri, int modes) {
                mGrants.revokeFromProvider(registration, uri, modes);
            }
        }, mInfo);
    }

    @Override public boolean onCreate() { return true; }

    private void authenticate(AttributionSource source) {
        if (mCallers.isSystem()) return;
        if (source == null || source.getUid() != mCallers.callerUid()) {
            throw new SecurityException("Media attribution must identify its Binder caller");
        }
        mCallers.requirePackage(source.getPackageName());
    }

    @Override protected int enforceReadPermissionInner(Uri uri, AttributionSource source) {
        authenticate(source);
        return super.enforceReadPermissionInner(uri, source);
    }

    @Override protected int enforceWritePermissionInner(Uri uri, AttributionSource source) {
        authenticate(source);
        return super.enforceWritePermissionInner(uri, source);
    }

    // ---- ContentProvider -------------------------------------------------------------

    @Override public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        Bundle args = new Bundle();
        if (selection != null) args.putString(ContentResolver.QUERY_ARG_SQL_SELECTION, selection);
        if (selectionArgs != null) args.putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, selectionArgs);
        if (sortOrder != null) args.putString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER, sortOrder);
        return query(uri, projection, args, null);
    }

    @Override public Cursor query(Uri uri, String[] projection, Bundle queryArgs,
            CancellationSignal signal) {
        Cursor cursor = mIndex.query(caller(), uri, projection, queryArgs(queryArgs));
        if (cursor != null && getContext() != null) {
            cursor.setNotificationUri(getContext().getContentResolver(), uri);
        }
        return cursor;
    }

    /** The Bundle keys MediaProvider.queryInternal honors. */
    private static MediaStoreIndex.QueryArgs queryArgs(Bundle extras) {
        MediaStoreIndex.QueryArgs args = new MediaStoreIndex.QueryArgs();
        if (extras == null) return args;
        args.selection = extras.getString(ContentResolver.QUERY_ARG_SQL_SELECTION);
        args.selectionArgs = extras.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS);
        args.sortOrder = extras.getString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER);
        if (args.sortOrder == null && extras.containsKey(ContentResolver.QUERY_ARG_SORT_COLUMNS)) {
            String[] columns = extras.getStringArray(ContentResolver.QUERY_ARG_SORT_COLUMNS);
            if (columns != null && columns.length > 0) {
                String direction = extras.getInt(ContentResolver.QUERY_ARG_SORT_DIRECTION,
                        ContentResolver.QUERY_SORT_DIRECTION_ASCENDING)
                        == ContentResolver.QUERY_SORT_DIRECTION_DESCENDING ? " DESC" : " ASC";
                StringBuilder order = new StringBuilder();
                for (String column : columns) {
                    if (order.length() > 0) order.append(", ");
                    order.append(column).append(direction);
                }
                args.sortOrder = order.toString();
            }
        }
        args.limit = extras.getString(ContentResolver.QUERY_ARG_SQL_LIMIT);
        if (args.limit == null && extras.containsKey(ContentResolver.QUERY_ARG_LIMIT)) {
            int limit = extras.getInt(ContentResolver.QUERY_ARG_LIMIT);
            int offset = extras.getInt(ContentResolver.QUERY_ARG_OFFSET, 0);
            args.limit = offset > 0 ? offset + "," + limit : Integer.toString(limit);
        }
        args.matchPending = extras.getInt(android.provider.MediaStore.QUERY_ARG_MATCH_PENDING,
                MediaStoreIndex.MATCH_DEFAULT);
        args.matchTrashed = extras.getInt(android.provider.MediaStore.QUERY_ARG_MATCH_TRASHED,
                MediaStoreIndex.MATCH_DEFAULT);
        args.matchFavorite = extras.getInt(android.provider.MediaStore.QUERY_ARG_MATCH_FAVORITE,
                MediaStoreIndex.MATCH_DEFAULT);
        return args;
    }

    @Override public String getType(Uri uri) {
        return mIndex.getType(uri);
    }

    @Override public Uri insert(Uri uri, ContentValues values) {
        return mIndex.insert(caller(), uri, values);
    }

    @Override public Uri insert(Uri uri, ContentValues values, Bundle extras) {
        return insert(uri, values);
    }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        MediaStoreIndex.QueryArgs args = new MediaStoreIndex.QueryArgs();
        args.selection = selection;
        args.selectionArgs = selectionArgs;
        return mIndex.update(caller(), uri, values, args);
    }

    @Override public int update(Uri uri, ContentValues values, Bundle extras) {
        return mIndex.update(caller(), uri, values, queryArgs(extras));
    }

    @Override public int delete(Uri uri, String selection, String[] selectionArgs) {
        MediaStoreIndex.QueryArgs args = new MediaStoreIndex.QueryArgs();
        args.selection = selection;
        args.selectionArgs = selectionArgs;
        return mIndex.delete(caller(), uri, args);
    }

    @Override public int delete(Uri uri, Bundle extras) {
        return mIndex.delete(caller(), uri, queryArgs(extras));
    }

    @Override public AssetFileDescriptor openTypedAssetFile(Uri uri, String mimeTypeFilter,
            Bundle opts) throws FileNotFoundException {
        return openTypedAssetFile(uri, mimeTypeFilter, opts, null);
    }

    @Override public AssetFileDescriptor openTypedAssetFile(Uri uri, String mimeTypeFilter,
            Bundle opts, CancellationSignal signal) throws FileNotFoundException {
        if (signal != null) signal.throwIfCanceled();
        if (opts == null || !opts.containsKey(ContentResolver.EXTRA_SIZE) || mimeTypeFilter == null
                || !mimeTypeFilter.regionMatches(true, 0, "image/", 0, 6)) {
            return super.openTypedAssetFile(uri, mimeTypeFilter, opts);
        }
        Point requested = opts.getParcelable(ContentResolver.EXTRA_SIZE, Point.class);
        MediaStoreIndex.FileTarget target = mIndex.thumbnailTarget(caller(), uri);
        return MediaStoreThumbnail.open(target.file, target.mediaType == MediaStoreIndex.MEDIA_TYPE_VIDEO,
                mimeTypeFilter, requested, signal);
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        int modeBits = ParcelFileDescriptor.parseMode(mode);
        boolean forWrite = (modeBits & ParcelFileDescriptor.MODE_WRITE_ONLY) != 0
                || (modeBits & ParcelFileDescriptor.MODE_READ_WRITE) == ParcelFileDescriptor.MODE_READ_WRITE;
        MediaStoreIndex.FileTarget target = mIndex.openFileTarget(caller(), uri, forWrite);
        if (!forWrite) return ParcelFileDescriptor.open(target.file, modeBits);
        // MediaProvider opens the item's file read-write and scans it once it is closed.
        modeBits |= ParcelFileDescriptor.MODE_CREATE;
        File parent = target.file.getParentFile();
        if (parent != null) parent.mkdirs();
        try {
            return ParcelFileDescriptor.open(target.file, modeBits, closeHandler(),
                    e -> mMaintenance.execute(() -> mIndex.fileWritten(target.id)));
        } catch (IOException e) {
            throw new FileNotFoundException("Failed to open for writing: " + e);
        }
    }

    private Handler closeHandler() {
        Handler handler = mCloseHandler;
        if (handler != null) return handler;
        synchronized (this) {
            if (mCloseHandler == null) {
                HandlerThread thread = new HandlerThread("gesher-media-close");
                thread.start();
                mCloseHandler = new Handler(thread.getLooper());
            }
            return mCloseHandler;
        }
    }

    // ---- call ---------------------------------------------------------------------------

    @Override public Bundle call(String method, String arg, Bundle extras) {
        // ContentProvider.Transport.call performs no read/write enforcement.
        MediaStoreIndex.Caller caller = caller();
        switch (method) {
            case "scan_file": {
                // MediaProvider.getResultForScanFile.
                Bundle result = new Bundle();
                Uri uri = null;
                if (arg != null) {
                    long identity = Binder.clearCallingIdentity();
                    try {
                        uri = mIndex.scanFile(new File(arg));
                    } finally {
                        Binder.restoreCallingIdentity(identity);
                    }
                }
                result.putParcelable(Intent.EXTRA_STREAM, uri);
                return result;
            }
            case "get_version": {
                Bundle result = new Bundle();
                result.putString(Intent.EXTRA_TEXT, mIndex.version(caller,
                        extras == null ? null : extras.getString(Intent.EXTRA_TEXT)));
                return result;
            }
            case "get_generation": {
                Bundle result = new Bundle();
                result.putLong(Intent.EXTRA_INDEX, mIndex.generation(
                        extras == null ? null : extras.getString(Intent.EXTRA_TEXT)));
                return result;
            }
            case "create_write_request":
            case "create_delete_request":
            case "create_trash_request":
            case "create_favorite_request": {
                // MediaProvider.createRequest validates, then returns a PendingIntent to its
                // PermissionActivity, where the user grants or denies. Gesher has no such
                // consent screen, so no request (and no grant) is fabricated.
                List<Uri> uris = new ArrayList<>();
                ClipData clip = extras == null ? null : extras.getParcelable("clip_data", ClipData.class);
                if (clip != null) {
                    for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
                }
                ContentValues values = extras == null ? null
                        : extras.getParcelable("content_values", ContentValues.class);
                Set<String> keys = values == null ? null : values.keySet();
                MediaStoreIndex.validateCreateRequest(method, uris, keys);
                throw new UnsupportedOperationException("Unsupported call: " + method
                        + " (no media consent screen on this device)");
            }
            default:
                throw new UnsupportedOperationException("Unsupported call: " + method);
        }
    }

    /** MediaService.onScanFile for ACTION_MEDIA_SCANNER_SCAN_FILE (any sender, as AOSP). */
    private void onScanFileBroadcast(Intent intent) {
        Uri data = intent.getData();
        if (data == null || !"file".equals(data.getScheme()) || data.getPath() == null) return;
        String path = data.getPath();
        mMaintenance.execute(() -> {
            try {
                mIndex.scanFile(new File(path));
            } catch (RuntimeException e) {
                SystemServer.log("media: scanning " + path + " failed: " + e);
            }
        });
    }

    // ---- the Binder caller -----------------------------------------------------------

    private MediaStoreIndex.Caller caller() {
        // As DownloadProvider/SettingsProvider: an authenticated application identity takes
        // precedence over server-local authority for a standalone APK in this process.
        String authenticated = mCallers.packageForPid(Binder.getCallingPid());
        final boolean system = authenticated == null && mCallers.isSystem();
        final String packageName = system ? "android"
                : authenticated != null ? authenticated : mCallers.callerPackage();
        final ApkPackage pkg = system ? null : mPm.get(packageName);
        if (!system && (pkg == null || mPm.isRemovalPending(packageName))) {
            throw new SecurityException("Unrecognized application Binder identity");
        }
        final int uid = system ? Binder.getCallingUid() : pkg.uid();
        final int pid = Binder.getCallingPid();
        final ApplicationInfo app = system ? null : pkg.applicationInfo(0);
        return new MediaStoreIndex.Caller() {
            @Override public boolean self() { return system; }
            @Override public String packageName() { return packageName; }
            @Override public int uid() { return uid; }
            @Override public String[] sharedPackages() {
                if (system) return new String[] {packageName};
                String[] packages = mPm.getPackagesForUid(uid);
                if (packages == null) return new String[0];
                return java.util.Arrays.stream(packages)
                        .filter(name -> !mPm.isRemovalPending(name))
                        .toArray(String[]::new);
            }
            @Override public int targetSdk() { return system ? Build.VERSION.SDK_INT : app.targetSdkVersion; }
            @Override public boolean permission(String permission) {
                return system || hasEffectivePermission(mPm, pkg, permission);
            }
            @Override public int opMode(String op) {
                if (system) return AppOpsManager.MODE_ALLOWED;
                return mPm.appOps().checkOperation(AppOpsManager.strOpToOp(op), uid, packageName);
            }
            @Override public boolean legacyStorageOp() {
                if (system) return false;
                // StorageManagerService's OP_LEGACY_STORAGE rule: pre-Q targets, or a
                // pre-R target that requested legacy external storage.
                return app.targetSdkVersion < Build.VERSION_CODES.Q
                        || (app.targetSdkVersion < Build.VERSION_CODES.R
                                && app.hasRequestedLegacyExternalStorage());
            }
            @Override public boolean uriGranted(Uri uri, boolean write) {
                if (system) return true;
                int mode = write ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                        : Intent.FLAG_GRANT_READ_URI_PERMISSION;
                return mGrants.check(uri, pid, uid, mode, 0) == PackageManager.PERMISSION_GRANTED;
            }
            @Override public boolean canSeePackage(String owner) {
                if (system) return true;
                return mPm.getVisibleSearchPackage(uid, packageName, owner) != null;
            }
        };
    }

    /** PermissionChecker-style permission state, including runtime AppOps restrictions. */
    static boolean hasEffectivePermission(PackageManagerService pm, ApkPackage pkg, String permission) {
        int uid = pkg.uid();
        if (MediaStoreIndex.MANAGE_EXTERNAL_STORAGE.equals(permission)) {
            // App-op permissions consult the grant only when the op is MODE_DEFAULT.
            if (!ConnectivityService.requests(pkg, permission)) return false;
            int mode = pm.appOps().checkOperation(
                    AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, uid, pkg.packageName);
            return mode == AppOpsManager.MODE_ALLOWED || (mode == AppOpsManager.MODE_DEFAULT
                    && pm.permissions().checkUidPermission(uid, permission, 0)
                            == PackageManager.PERMISSION_GRANTED);
        }
        if (MediaStoreIndex.ACCESS_MEDIA_OWNER_PACKAGE_NAME.equals(permission)) {
            return false; // signature|privileged in the MediaProvider module
        }
        if (pm.permissions().checkUidPermission(uid, permission, 0)
                != PackageManager.PERMISSION_GRANTED) return false;
        int op = AppOpsManager.permissionToOpCode(permission);
        // A runtime grant can remain set after AppOps revocation. Use the effective
        // operation so legacy collection access and failed storage publication deny it too.
        return op == AppOpsManager.OP_NONE || pm.appOps().checkOperation(op, uid, pkg.packageName)
                == AppOpsManager.MODE_ALLOWED;
    }

    private final class Host implements MediaStoreIndex.Host {
        @Override public File volumeRoot() { return MediaStoreProvider.volumeRoot(); }
        @Override public void notifyChange(Uri uri) {
            Context context = getContext();
            if (context == null) return;
            long identity = Binder.clearCallingIdentity();
            try {
                context.getContentResolver().notifyChange(uri, null);
            } catch (RuntimeException e) {
                SystemServer.log("media: change notification failed: " + e);
            } finally {
                Binder.restoreCallingIdentity(identity);
            }
        }
        @Override public long currentTimeMillis() { return System.currentTimeMillis(); }
        @Override public File journalDirectory() { return mJournal; }
    }
}
