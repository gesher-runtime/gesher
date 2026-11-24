/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.Process;
import android.webkit.IWebViewUpdateService;
import android.webkit.WebViewProviderInfo;
import android.webkit.WebViewProviderResponse;

import java.util.function.Supplier;

/**
 * The {@code webviewupdate} service: which package implements WebView (docs/webview.md).
 *
 * <p>Gesher's WebView provider is built in (ADR 0041): WebViews are Microsoft Edge
 * WebView2 controllers, with no WebView APK. {@code WebView.getCurrentWebViewPackage()}
 * asks this service for the provider package when none is loaded in the process, as on
 * a device; it is Gesher's provider, at the installed WebView2 Runtime's Chromium
 * version, or null when no runtime is installed (WebViews then stay empty). The
 * provider is not an installed package: PackageManager does not list it, as a
 * device's provider can be absent from a caller's package visibility.
 */
public final class WebViewUpdateService extends IWebViewUpdateService.Default {
    /** The service's name (Context.WEBVIEW_UPDATE_SERVICE, which is hidden). */
    public static final String NAME = "webviewupdate";
    /** The provider's package name: Gesher's own, not a Google WebView package. */
    public static final String PROVIDER_PACKAGE = "com.gesher.webview";
    static final String DESCRIPTION = "Gesher WebView (Microsoft Edge WebView2)";

    /** How long a runtime version read stays current (WebView2 updates in the background). */
    private static final long VERSION_CACHE_MS = 10_000;

    private final PackageManagerService mPm;
    private final Supplier<String> mRuntimeVersion;
    private final Object mLock = new Object();
    private String mCachedVersion;
    private long mCachedAt = Long.MIN_VALUE;

    WebViewUpdateService(PackageManagerService pm) {
        this(pm, com.gesher.app.webview.WebView2Host::runtimeVersion);
    }

    /** {@code runtimeVersion}: the installed WebView2 Runtime's version, or null (fixtures). */
    public WebViewUpdateService(PackageManagerService pm, Supplier<String> runtimeVersion) {
        mPm = pm;
        mRuntimeVersion = runtimeVersion;
    }

    private static WebViewProviderInfo provider() {
        // Built in, so available by default and never a fallback; no APK, no signatures.
        return new WebViewProviderInfo(PROVIDER_PACKAGE, DESCRIPTION, true, false, null);
    }

    /** The provider at the installed runtime's version, or null without a runtime. */
    private PackageInfo current() {
        String version = runtimeVersion();
        if (version == null || version.isEmpty()) return null;
        PackageInfo info = new PackageInfo();
        info.packageName = PROVIDER_PACKAGE;
        info.versionName = version;
        info.setLongVersionCode(versionCode(version));
        ApplicationInfo app = new ApplicationInfo();
        app.packageName = PROVIDER_PACKAGE;
        app.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        app.enabled = true;
        app.minSdkVersion = Build.VERSION.SDK_INT;
        app.targetSdkVersion = Build.VERSION.SDK_INT;
        info.applicationInfo = app;
        return info;
    }

    /** The runtime's version, read at most every {@link #VERSION_CACHE_MS}. */
    private String runtimeVersion() {
        long now = android.os.SystemClock.elapsedRealtime();
        synchronized (mLock) {
            if (mCachedAt != Long.MIN_VALUE && now - mCachedAt < VERSION_CACHE_MS) {
                return mCachedVersion;
            }
            String version;
            try {
                version = mRuntimeVersion.get();
            } catch (LinkageError e) {
                version = null;  // No WebView bridge library: no runtime to report.
            }
            mCachedVersion = version;
            mCachedAt = now;
            return version;
        }
    }

    /**
     * Chromium's "major.minor.build.patch" as Chrome for Android numbers its versions
     * (build * 100000 + patch * 100), so the code grows with the runtime; 0 if the
     * version is not in that form.
     */
    static long versionCode(String version) {
        String[] parts = version.split("\\.");
        if (parts.length != 4) return 0;
        try {
            long build = Long.parseLong(parts[2]);
            long patch = Long.parseLong(parts[3]);
            if (build < 0 || patch < 0 || patch >= 1000) return 0;
            return build * 100000 + patch * 100;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    @Override
    public void notifyRelroCreationCompleted() {
        // No RELRO: the provider is not a native library loaded from an APK.
    }

    @Override
    public WebViewProviderResponse waitForAndGetProvider() {
        PackageInfo info = current();
        return info != null
                ? new WebViewProviderResponse(info, WebViewProviderResponse.STATUS_SUCCESS)
                : new WebViewProviderResponse(null,
                        WebViewProviderResponse.STATUS_FAILED_LISTING_WEBVIEW_PACKAGES);
    }

    @Override
    public String changeProviderAndSetting(String newProvider) {
        if (!callerHolds(android.Manifest.permission.WRITE_SECURE_SETTINGS)) {
            throw new SecurityException("Requires " + android.Manifest.permission.WRITE_SECURE_SETTINGS);
        }
        // One provider: a request for another package leaves it selected, as AOSP
        // answers a package that is not a valid provider.
        return getCurrentWebViewPackageName();
    }

    private boolean callerHolds(String permission) {
        int uid = Binder.getCallingUid();
        if (uid == Process.SYSTEM_UID || uid == Process.ROOT_UID) return true;
        String[] names = mPm.getPackagesForUid(uid);
        if (names == null) return false;
        for (String name : names) {
            if (mPm.checkPermission(permission, name, 0) == PackageManager.PERMISSION_GRANTED) {
                return true;
            }
        }
        return false;
    }

    @Override
    public WebViewProviderInfo[] getValidWebViewPackages() {
        return current() != null ? new WebViewProviderInfo[] {provider()}
                : new WebViewProviderInfo[0];
    }

    @Override
    public WebViewProviderInfo[] getAllWebViewPackages() {
        return new WebViewProviderInfo[] {provider()};
    }

    @Override
    public String getCurrentWebViewPackageName() {
        PackageInfo info = current();
        return info != null ? info.packageName : null;
    }

    @Override
    public PackageInfo getCurrentWebViewPackage() {
        return current();
    }

    @Override
    public WebViewProviderInfo getDefaultWebViewPackage() {
        return provider();
    }
}
