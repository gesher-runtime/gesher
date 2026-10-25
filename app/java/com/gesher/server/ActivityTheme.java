/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.util.DisplayMetrics;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

/**
 * What the system server reads of an activity's theme, as ActivityRecord does through
 * AttributeCache (docs/lifecycle.md): whether its window is translucent or floating
 * ({@code windowIsTranslucent}, {@code windowIsFloating}, {@code windowSwipeToDismiss}:
 * {@link ActivityInfo#isTranslucentOrFloating}), so that it does not occlude the
 * activity below it ({@code ActivityRecord.occludesParent}), which then stays visible,
 * paused instead of stopped. The APK's resources are read with the framework's
 * AssetManager (with the system's resources, as an app's), its theme resolved as the app's
 * would be (the activity's theme, else the application's, else the default theme for its
 * target SDK). Results are kept per APK (and its modification time) and theme.
 */
final class ActivityTheme {
    private static final Map<String, Boolean> sCache = new HashMap<>();

    private ActivityTheme() {
    }

    /** Whether the activity's window is translucent or floating (false when unknown). */
    static boolean isTranslucentOrFloating(ActivityInfo info) {
        ApplicationInfo app = info.applicationInfo;
        String apk = app != null ? app.sourceDir : null;
        if (apk == null) return false;
        int theme = info.getThemeResource();
        String key = apk + "@" + new File(apk).lastModified() + "#" + Integer.toHexString(theme);
        synchronized (sCache) {
            Boolean cached = sCache.get(key);
            if (cached != null) return cached;
        }
        boolean result = false;
        AssetManager assets = null;
        try {
            assets = new AssetManager();
            if (assets.addAssetPath(apk) == 0) {
                throw new IllegalStateException("cannot read the resources of " + apk);
            }
            DisplayMetrics metrics = new DisplayMetrics();
            metrics.setToDefaults();
            Configuration config = new Configuration();
            config.setToDefaults();
            Resources res = new Resources(assets, metrics, config);
            Resources.Theme t = res.newTheme();
            t.applyStyle(Resources.selectDefaultTheme(theme, app.targetSdkVersion), true);
            TypedArray a = t.obtainStyledAttributes(com.android.internal.R.styleable.Window);
            try {
                result = ActivityInfo.isTranslucentOrFloating(a);
            } finally {
                a.recycle();
            }
        } catch (Throwable e) {
            SystemServer.log("the theme of " + info.packageName + "/" + info.name
                    + " not read (taken as opaque): " + e);
        } finally {
            if (assets != null) assets.close();
        }
        synchronized (sCache) {
            sCache.put(key, result);
        }
        return result;
    }
}
