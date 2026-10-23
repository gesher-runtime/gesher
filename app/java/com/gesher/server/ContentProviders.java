/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.app.IApplicationThread;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.os.IBinder;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * ContentProviderHelper, lite: the content providers apps reach through
 * {@code ContentResolver}.
 *
 * <ul>
 * <li>An app's own providers (the manifest's {@code <provider>}s) are given to its process
 *     in {@code bindApplication}, which installs them (onCreate, before
 *     Application.onCreate) and publishes them back ({@code publishContentProviders}); a
 *     resolver in the same process finds them installed, and other processes get the
 *     published one from {@code getContentProvider}.
 * <li>{@code settings} is the system server's own stand-in ({@link SettingsProvider}),
 *     so that Settings.System, Secure and Global answer.
 * </ul>
 */
final class ContentProviders {
    private final PackageManagerService mPm;
    private final SettingsProvider mSettings = new SettingsProvider();

    // Guarded by this. Published providers, by authority.
    private final Map<String, ContentProviderHolder> mPublished = new HashMap<>();
    private final Map<String, ActivityTaskManagerService.ProcessRecord> mPublishedBy =
            new HashMap<>();

    ContentProviders(PackageManagerService pm) {
        mPm = pm;
    }

    /** The providers a process of {@code ai}'s package installs at bindApplication. */
    List<ProviderInfo> providersFor(ApplicationInfo ai) {
        ApkPackage p = mPm.get(ai.packageName);
        return p == null ? List.of()
                : p.providers(PackageManager.GET_META_DATA
                        | PackageManager.GET_SHARED_LIBRARY_FILES);
    }

    ContentProviderHolder getContentProvider(IApplicationThread caller, String callingPackage,
            String name, int userId, boolean stable) {
        if (SettingsProvider.AUTHORITY.equals(name)) {
            return mSettings.holder();
        }
        synchronized (this) {
            ContentProviderHolder published = mPublished.get(name);
            if (published != null) {
                ContentProviderHolder h = new ContentProviderHolder(published.info);
                h.provider = published.provider;
                h.noReleaseNeeded = true;
                return h;
            }
        }
        ProviderInfo declared = mPm.provider(name, PackageManager.GET_META_DATA);
        SystemServer.log("getContentProvider " + name + " from " + callingPackage + ": "
                + (declared != null ? "declared by " + declared.packageName
                        + ", not published (its process is not running)" : "no such provider"));
        return null;
    }

    void publishContentProviders(ActivityTaskManagerService.ProcessRecord app,
            List<ContentProviderHolder> providers) {
        if (providers == null) return;
        synchronized (this) {
            for (ContentProviderHolder h : providers) {
                if (h == null || h.info == null || h.info.authority == null) continue;
                for (String authority : h.info.authority.split(";")) {
                    mPublished.put(authority, h);
                    mPublishedBy.put(authority, app);
                }
            }
        }
        for (ContentProviderHolder h : providers) {
            if (h == null || h.info == null) continue;
            SystemServer.log("publishContentProviders: " + h.info.authority + " ("
                    + h.info.name + ") in " + app);
        }
    }

    boolean refContentProvider(IBinder connection, int stable, int unstable) {
        return true;  // connections are not counted: providers live as long as their process
    }

    /** The process went away: so did the providers it published. */
    void processDied(ActivityTaskManagerService.ProcessRecord app) {
        synchronized (this) {
            mPublishedBy.entrySet().removeIf(e -> {
                if (e.getValue() != app) return false;
                mPublished.remove(e.getKey());
                return true;
            });
        }
    }
}
