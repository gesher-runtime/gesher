/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.ContentProviderHolder;
import android.app.DownloadManager;
import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PathPermission;
import android.content.pm.ProviderInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.ParcelFileDescriptor;
import android.os.PatternMatcher;
import android.os.Process;
import android.os.SystemClock;
import android.provider.Downloads;
import android.provider.Settings;
import android.security.net.config.ApplicationConfig;
import android.security.net.config.ManifestConfigSource;
import android.util.DisplayMetrics;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * {@code content://downloads}: Android's DownloadManager backend in the system server
 * (AOSP's DownloadProvider runs in android.process.media). Registered like
 * {@link DocumentsStorageProvider}: the framework's ContentProvider.Transport enforces
 * the manifest's path permissions (INTERNET for {@code my_downloads} and
 * {@code download}, ACCESS_ALL_DOWNLOADS for {@code all_downloads}) and URI grants;
 * {@link DownloadRequests} owns the rows, {@link DownloadQueue} and
 * {@link DownloadTransfer} run them over the system server's HTTP stack.
 *
 * <p>Not implemented yet, and therefore not reported: download notifications
 * (setNotificationVisibility is stored and validated, but no notification is posted, so
 * ACTION_NOTIFICATION_CLICKED is never sent) and MediaStore insertion (Gesher has no
 * MediaStore provider; mediaprovider_uri stays null).
 */
public final class DownloadProvider extends ContentProvider {
    public static final String AUTHORITY = Downloads.Impl.AUTHORITY;

    private final PackageManagerService mPm;
    private final ActivityManagerService mAm;
    private final UriGrants mGrants;
    private final CallerIdentity mCallers;
    private final ProviderInfo mInfo;
    final ContentProviders.RegisteredProvider registration;
    private final File mFilesDir, mCacheDir;
    private final DownloadRequests mRequests;
    private final DownloadQueue mQueue;
    private final java.util.concurrent.ThreadPoolExecutor mMaintenance =
            new java.util.concurrent.ThreadPoolExecutor(1, 1, 30, java.util.concurrent.TimeUnit.SECONDS,
                    new java.util.concurrent.LinkedBlockingQueue<>(), task -> {
                        Thread thread = new Thread(task, "gesher-download-maintenance");
                        thread.setDaemon(true);
                        return thread;
                    });
    {
        mMaintenance.allowCoreThreadTimeOut(true);
    }
    private volatile Handler mCloseHandler;

    /** Creates, registers and starts the provider; {@code stateDir} null keeps no state. */
    static DownloadProvider create(ActivityManagerService am, PackageManagerService pm,
            ConnectivityService connectivity, File stateDir) {
        File files, cache;
        SQLiteDatabase db;
        if (stateDir != null) {
            files = new File(stateDir, "files");
            cache = new File(stateDir, "cache");
            if (!(stateDir.isDirectory() || stateDir.mkdirs())) {
                throw new IllegalStateException("Cannot create the downloads state directory");
            }
            db = SQLiteDatabase.openOrCreateDatabase(new File(stateDir, "downloads.db"), null);
        } else {
            // A process without persistent device state keeps no downloads across runs.
            File temporary = new File(System.getProperty("java.io.tmpdir"),
                    "gesher-downloads-" + Process.myPid());
            files = new File(temporary, "files");
            cache = new File(temporary, "cache");
            db = SQLiteDatabase.create(null);
        }
        files.mkdirs();
        cache.mkdirs();
        DownloadProvider provider = new DownloadProvider(am, pm, connectivity, db, files, cache);
        am.providers().setDownloads(provider);
        provider.start(connectivity);
        return provider;
    }

    private DownloadProvider(ActivityManagerService am, PackageManagerService pm,
            ConnectivityService connectivity, SQLiteDatabase db, File files, File cache) {
        mAm = am;
        mPm = pm;
        mGrants = am.uriGrants();
        mCallers = am.callers();
        mFilesDir = files;
        mCacheDir = cache;
        mInfo = providerInfo(getClass().getName());
        registration = new ContentProviders.RegisteredProvider(mInfo, getIContentProvider());
        am.providers().registerProvider(registration);
        mRequests = new DownloadRequests(db, new Host());
        mQueue = new DownloadQueue(mRequests, new QueueEnvironment(connectivity));
    }

    /** The manifest declaration of AOSP's DownloadProvider (path permissions, grants). */
    static ProviderInfo providerInfo(String className) {
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
        info.name = className;
        info.authority = AUTHORITY;
        info.enabled = true;
        info.exported = true;
        info.flags = ProviderInfo.FLAG_SINGLE_USER;
        info.pathPermissions = new PathPermission[] {
                new PathPermission("/my_downloads", PatternMatcher.PATTERN_PREFIX,
                        Manifest.permission.INTERNET, Manifest.permission.INTERNET),
                new PathPermission("/all_downloads", PatternMatcher.PATTERN_PREFIX,
                        Downloads.Impl.PERMISSION_ACCESS_ALL, Downloads.Impl.PERMISSION_ACCESS_ALL),
                new PathPermission("/download", PatternMatcher.PATTERN_PREFIX,
                        Manifest.permission.INTERNET, Manifest.permission.INTERNET),
        };
        info.grantUriPermissions = false;
        info.uriPermissionPatterns = new PatternMatcher[] {
                new PatternMatcher("/all_downloads/", PatternMatcher.PATTERN_PREFIX),
                new PatternMatcher("/my_downloads/", PatternMatcher.PATTERN_PREFIX),
        };
        return info;
    }

    private void start(ConnectivityService connectivity) {
        // The framework context attaches with the first holder(), as for the documents
        // provider; the settings context may not be prepared while services are created.
        // DownloadProvider.onCreate: each download's owner may open all_downloads/<id>.
        for (long id : mRequests.ids()) {
            DownloadRequests.Info info = mRequests.info(id);
            String pkg = info == null ? null : firstPackage(info.uid);
            if (pkg != null) {
                try { mGrants.grantFromProvider(registration, pkg, allDownloadsUri(id), grantModes()); }
                catch (RuntimeException e) { SystemServer.log("downloads: grant " + id + ": " + e); }
            }
        }
        mPm.onPackageRemoval(name -> mMaintenance.execute(this::removeOrphans));
        mMaintenance.execute(() -> {
            removeOrphans();
            mQueue.start();
        });
        if (connectivity != null) connectivity.addSystemListener(mQueue::networkChanged);
    }

    /** Helpers.handleRemovedUidEntries for every UID that no longer has a package. */
    private void removeOrphans() {
        try {
            for (int uid : mRequests.uids()) {
                if (firstPackage(uid) == null) mRequests.removeUid(uid);
            }
        } catch (RuntimeException e) {
            SystemServer.log("downloads: removing uninstalled owners failed: " + e);
        }
    }

    public ProviderInfo providerInfo() { return new ProviderInfo(mInfo); }

    synchronized ContentProviderHolder holder() {
        if (getContext() == null) attachTo(android.app.GesherSettingsContext.get());
        ContentProviderHolder result = new ContentProviderHolder(providerInfo());
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

    private void authenticate(android.content.AttributionSource source) {
        if (mCallers.isSystem()) return;
        if (source == null || source.getUid() != mCallers.callerUid()) {
            throw new SecurityException("Download attribution must identify its Binder caller");
        }
        mCallers.requirePackage(source.getPackageName());
    }

    @Override protected int enforceReadPermissionInner(Uri uri, android.content.AttributionSource source) {
        authenticate(source);
        return super.enforceReadPermissionInner(uri, source);
    }

    @Override protected int enforceWritePermissionInner(Uri uri, android.content.AttributionSource source) {
        authenticate(source);
        return super.enforceWritePermissionInner(uri, source);
    }

    // ---- ContentProvider -------------------------------------------------------------

    @Override public Uri insert(Uri uri, ContentValues values) {
        return mRequests.insert(caller(), uri, values);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        Cursor cursor = mRequests.query(caller(), uri, projection, selection, selectionArgs, sortOrder);
        if (cursor != null) cursor.setNotificationUri(getContext().getContentResolver(), uri);
        return cursor;
    }

    @Override public int update(Uri uri, ContentValues values, String where, String[] whereArgs) {
        return mRequests.update(caller(), uri, values, where, whereArgs);
    }

    @Override public int delete(Uri uri, String where, String[] whereArgs) {
        return mRequests.delete(caller(), uri, where, whereArgs);
    }

    @Override public String getType(Uri uri) {
        return mRequests.getType(uri);
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        DownloadRequests.OpenTarget target = mRequests.openFileTarget(caller(), uri);
        int pfdMode = ParcelFileDescriptor.parseMode(mode);
        if (pfdMode == ParcelFileDescriptor.MODE_READ_ONLY) {
            return ParcelFileDescriptor.open(target.file, pfdMode);
        }
        long id = ContentUris.parseId(uri);
        try {
            // When finished writing, update size and timestamp (DownloadProvider.openFile).
            return ParcelFileDescriptor.open(target.file, pfdMode, closeHandler(),
                    e -> mRequests.fileWritten(id, target.file.length()));
        } catch (IOException e) {
            throw new FileNotFoundException("Failed to open for writing: " + e);
        }
    }

    private Handler closeHandler() {
        Handler handler = mCloseHandler;
        if (handler != null) return handler;
        synchronized (this) {
            if (mCloseHandler == null) {
                HandlerThread thread = new HandlerThread("gesher-download-close");
                thread.start();
                mCloseHandler = new Handler(thread.getLooper());
            }
            return mCloseHandler;
        }
    }

    // ---- the Binder caller -----------------------------------------------------------

    private DownloadRequests.Caller caller() {
        // A standalone APK shares this process's PID (gesher-app's own services): its
        // authenticated application identity takes precedence over server-local
        // authority, as in SettingsProvider.
        String authenticated = mCallers.packageForPid(Binder.getCallingPid());
        final boolean system = authenticated == null && mCallers.isSystem();
        final String packageName = system ? "android"
                : authenticated != null ? authenticated : mCallers.callerPackage();
        final ApkPackage pkg = system ? null : mPm.get(packageName);
        if (!system && (pkg == null || mPm.isRemovalPending(packageName))) {
            throw new SecurityException("Unrecognized application Binder identity");
        }
        final int uid = system ? Binder.getCallingUid() : pkg.uid();
        return new DownloadRequests.Caller() {
            @Override public int uid() { return uid; }
            @Override public String packageName() { return packageName; }
            @Override public boolean holds(String permission) {
                if (system) return true;
                if (Downloads.Impl.PERMISSION_NO_NOTIFICATION.equals(permission)) {
                    // A normal permission declared by DownloadProvider's own manifest:
                    // granted to a package that requests it.
                    return pkg != null && ConnectivityService.requests(pkg, permission);
                }
                return mPm.permissions().checkUidPermission(uid, permission, 0)
                        == PackageManager.PERMISSION_GRANTED;
            }
            @Override public boolean local() { return system; }
            @Override public boolean legacyStorage() {
                if (system) return true;
                if (pkg == null) return false;
                // StorageManagerService's OP_LEGACY_STORAGE rule: pre-Q targets, or a
                // pre-R target that requested legacy external storage.
                ApplicationInfo app = pkg.applicationInfo(0);
                return app.targetSdkVersion < Build.VERSION_CODES.Q
                        || (app.targetSdkVersion < Build.VERSION_CODES.R
                                && app.hasRequestedLegacyExternalStorage());
            }
            @Override public boolean writeExternalStorage() {
                if (system) return true;
                return holds(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                        && mPm.appOps().checkOperation(AppOpsManager.OP_WRITE_EXTERNAL_STORAGE,
                                uid, packageName) == AppOpsManager.MODE_ALLOWED;
            }
            @Override public boolean installer() {
                if (system) return true;
                return holds(Manifest.permission.REQUEST_INSTALL_PACKAGES)
                        || mPm.appOps().checkOperation(AppOpsManager.OP_REQUEST_INSTALL_PACKAGES,
                                uid, packageName) == AppOpsManager.MODE_ALLOWED;
            }
        };
    }

    private String firstPackage(int uid) {
        String[] packages = mPm.getPackagesForUid(uid);
        return packages == null || packages.length == 0 ? null : packages[0];
    }

    private static Uri allDownloadsUri(long id) {
        return ContentUris.withAppendedId(Downloads.Impl.ALL_DOWNLOADS_CONTENT_URI, id);
    }

    private static int grantModes() {
        return Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    }

    private final class Host implements DownloadRequests.Host {
        @Override public File externalRoot() { return Environment.getExternalStorageDirectory(); }
        @Override public File filesDir() { return mFilesDir; }
        @Override public File cacheDir() { return mCacheDir; }
        @Override public boolean uidOwnsPackage(int uid, String packageName) {
            ApkPackage pkg = mPm.get(packageName);
            return pkg != null && pkg.uid() == uid && !mPm.isRemovalPending(packageName);
        }
        @Override public void grantAllDownloads(String packageName, long id) {
            long identity = Binder.clearCallingIdentity();
            try {
                if (!"android".equals(packageName)) {
                    mGrants.grantFromProvider(registration, packageName, allDownloadsUri(id), grantModes());
                }
            } finally {
                Binder.restoreCallingIdentity(identity);
            }
        }
        @Override public void revokeAllDownloads(long id) {
            long identity = Binder.clearCallingIdentity();
            try {
                mGrants.revokeFromProvider(registration, allDownloadsUri(id), grantModes());
            } finally {
                Binder.restoreCallingIdentity(identity);
            }
        }
        @Override public void notifyChange(Uri uri) {
            Context context = getContext();
            if (context == null) return;
            long identity = Binder.clearCallingIdentity();
            try {
                context.getContentResolver().notifyChange(uri, null);
            } catch (RuntimeException e) {
                SystemServer.log("downloads: change notification failed: " + e);
            } finally {
                Binder.restoreCallingIdentity(identity);
            }
        }
        @Override public void sendCompleted(String packageName, String className, boolean publicApi,
                long id, String extras) {
            Intent intent;
            if (publicApi) {
                intent = new Intent(DownloadManager.ACTION_DOWNLOAD_COMPLETE);
                intent.setPackage(packageName);
                intent.putExtra(DownloadManager.EXTRA_DOWNLOAD_ID, id);
            } else {
                intent = new Intent(Downloads.Impl.ACTION_DOWNLOAD_COMPLETED);
                intent.setClassName(packageName, className);
                if (extras != null) intent.putExtra(Downloads.Impl.COLUMN_NOTIFICATION_EXTRAS, extras);
                intent.setData(ContentUris.withAppendedId(Downloads.Impl.CONTENT_URI, id));
            }
            try {
                mAm.sendBroadcastFor("android", intent, null);
            } catch (RuntimeException e) {
                SystemServer.log("downloads: completion broadcast for " + id + " failed: " + e);
            }
        }
        @Override public void schedule(long id) { mQueue.schedule(id); }
        @Override public void cancel(long id) { mQueue.cancel(id); }
        @Override public long currentTimeMillis() { return System.currentTimeMillis(); }
    }

    private final class QueueEnvironment implements DownloadQueue.Environment {
        private final ConnectivityService mConnectivity;

        QueueEnvironment(ConnectivityService connectivity) { mConnectivity = connectivity; }

        /** The network Windows routes through, when it reaches the internet (as validated). */
        @Override public DownloadQueue.NetworkState network() {
            String text = mConnectivity != null ? mConnectivity.snapshotForSystem()
                    : com.gesher.net.WindowsNetworks.snapshot();
            com.gesher.net.WindowsNetworks.Net net = com.gesher.net.WindowsNetworks.parse(text)
                    .defaultNetwork();
            if (net == null || !net.internet) return null;
            return new DownloadQueue.NetworkState(net.metered, net.roaming);
        }
        @Override public long maxBytesOverMobile() {
            return SettingsProvider.globalLong(Settings.Global.DOWNLOAD_MAX_BYTES_OVER_MOBILE,
                    Long.MAX_VALUE);
        }
        @Override public long recommendedMaxBytesOverMobile() {
            return SettingsProvider.globalLong(
                    Settings.Global.DOWNLOAD_RECOMMENDED_MAX_BYTES_OVER_MOBILE, Long.MAX_VALUE);
        }
        @Override public DownloadQueue.Transport transport(DownloadRequests.Info info) {
            return new PackageTransport(info.packageName != null ? info.packageName
                    : firstPackage(info.uid));
        }
        @Override public String userAgent() { return defaultUserAgent(); }
        @Override public long currentTimeMillis() { return System.currentTimeMillis(); }
        @Override public long elapsedRealtime() { return SystemClock.elapsedRealtime(); }
        /** The power status JobSchedulerService's charging constraint reads. */
        @Override public int[] powerStatus() { return WindowsHost.powerStatus(); }
        /** The display-off inactivity JobSchedulerService's idle constraint reads. */
        @Override public long idleMillis() { return WindowsHost.jobIdleReading(); }
        /** The events JobSchedulerService re-evaluates those constraints on. */
        @Override public void listen(Runnable listener) {
            WindowsHost.addListener(events -> {
                if ((events & (WindowsHost.EVENT_POWER_STATUS | WindowsHost.EVENT_DISPLAY_STATE
                        | WindowsHost.EVENT_RESUMED)) != 0) {
                    listener.run();
                }
            });
        }
    }

    /** Constants.DEFAULT_USER_AGENT. */
    static String defaultUserAgent() {
        StringBuilder builder = new StringBuilder("AndroidDownloadManager");
        boolean validRelease = Build.VERSION.RELEASE_OR_CODENAME != null
                && !Build.VERSION.RELEASE_OR_CODENAME.isEmpty();
        boolean validId = Build.ID != null && !Build.ID.isEmpty();
        boolean includeModel = "REL".equals(Build.VERSION.CODENAME)
                && Build.MODEL != null && !Build.MODEL.isEmpty();
        if (validRelease) builder.append('/').append(Build.VERSION.RELEASE_OR_CODENAME);
        builder.append(" (Linux; U; Android");
        if (validRelease) builder.append(' ').append(Build.VERSION.RELEASE_OR_CODENAME);
        if (includeModel || validId) {
            builder.append(';');
            if (includeModel) builder.append(' ').append(Build.MODEL);
            if (validId) builder.append(" Build/").append(Build.ID);
        }
        return builder.append(')').toString();
    }

    /**
     * RealSystemFacade's per-package policy: the requesting app's network security
     * config (NetworkSecurityPolicy.getApplicationConfigForPackage) decides cleartext
     * and TLS trust. A package whose config cannot be read gets no cleartext and no TLS
     * (AOSP fails cleartext closed for an unknown package).
     */
    private final class PackageTransport implements DownloadQueue.Transport {
        private final String mPackage;
        private ApplicationConfig mConfig;
        private boolean mLoaded;

        PackageTransport(String packageName) { mPackage = packageName; }

        private synchronized ApplicationConfig config() {
            if (mLoaded) return mConfig;
            mLoaded = true;
            ApkPackage pkg = mPackage == null ? null : mPm.get(mPackage);
            if (pkg == null) return null;
            ApplicationInfo app = pkg.applicationInfo(0);
            try {
                // The AssetManager stays open: trust anchors in res/raw load on first use.
                AssetManager assets = new AssetManager();
                if (assets.addAssetPath(app.sourceDir) == 0) {
                    throw new IOException("cannot read APK resources " + app.sourceDir);
                }
                DisplayMetrics metrics = new DisplayMetrics();
                metrics.setToDefaults();
                Configuration configuration = new Configuration();
                configuration.setToDefaults();
                Resources resources = new Resources(assets, metrics, configuration);
                Context packageContext = new ContextWrapper(android.app.GesherSettingsContext.get()) {
                    @Override public Resources getResources() { return resources; }
                    @Override public ApplicationInfo getApplicationInfo() { return app; }
                    @Override public String getPackageName() { return mPackage; }
                };
                ApplicationConfig config = new ApplicationConfig(new ManifestConfigSource(packageContext));
                config.isCleartextTrafficPermitted(); // parse now, report failures here
                mConfig = config;
            } catch (IOException | RuntimeException e) {
                SystemServer.log("downloads: network security config of " + mPackage
                        + " was not read: " + e);
                mConfig = null;
            }
            return mConfig;
        }

        @Override public boolean cleartextPermitted(String host) {
            ApplicationConfig config = config();
            return config != null && config.isCleartextTrafficPermitted(host);
        }

        @Override public javax.net.ssl.SSLSocketFactory socketFactory()
                throws java.security.GeneralSecurityException {
            ApplicationConfig config = config();
            if (config == null) {
                throw new java.security.GeneralSecurityException("No network security config");
            }
            javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
            context.init(null, new javax.net.ssl.TrustManager[] {config.getTrustManager()}, null);
            return context.getSocketFactory();
        }

        @Override public HttpURLConnection open(URL url) throws IOException {
            // The system server's default network and Windows' proxy (WindowsProxy).
            return (HttpURLConnection) url.openConnection();
        }
    }
}
