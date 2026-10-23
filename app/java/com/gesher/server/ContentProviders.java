/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.os.IBinder;
import android.os.SystemClock;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

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
 * <li>A provider of a package with no process: {@code getContentProvider} starts the
 *     process ({@link ActivityTaskManagerService#withProcess}) and waits until it publishes
 *     the provider, as ContentProviderHelper waits for a provider being launched.
 * <li>The processes of other packages that got a process's provider are its clients
 *     (ContentProviderConnection): while it has any, nothing ends it; when the last one is
 *     gone, it may exit ({@link ActivityTaskManagerService#maybeExit}).
 * <li>{@code settings} is the system server's own stand-in ({@link SettingsProvider}),
 *     so that Settings.System, Secure and Global answer.
 * </ul>
 */
final class ContentProviders {
    /** How long getContentProvider waits for a provider's process to publish it. */
    private static final long PUBLISH_TIMEOUT_MS = 120_000;

    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final SettingsProvider mSettings = new SettingsProvider();

    // Guarded by this. Published providers, by authority.
    private final Map<String, ContentProviderHolder> mPublished = new HashMap<>();
    private final Map<String, ActivityTaskManagerService.ProcessRecord> mPublishedBy =
            new HashMap<>();
    /** The processes (of other packages) using each process's providers. */
    private final Map<ActivityTaskManagerService.ProcessRecord,
            Set<ActivityTaskManagerService.ProcessRecord>> mClients = new HashMap<>();

    ContentProviders(PackageManagerService pm, ActivityTaskManagerService atm) {
        mPm = pm;
        mAtm = atm;
    }

    /** The providers a process of {@code ai}'s package installs at bindApplication. */
    List<ProviderInfo> providersFor(ApplicationInfo ai) {
        ApkPackage p = mPm.get(ai.packageName);
        return p == null ? List.of()
                : p.providers(PackageManager.GET_META_DATA
                        | PackageManager.GET_SHARED_LIBRARY_FILES);
    }

    /**
     * The provider of authority {@code name} for {@code caller} (its process, null for a
     * caller that is not an app process); starts the provider's process if needed.
     */
    ContentProviderHolder getContentProvider(ActivityTaskManagerService.ProcessRecord caller,
            String callingPackage, String name) {
        if (SettingsProvider.AUTHORITY.equals(name)) {
            return mSettings.holder();
        }
        ProviderInfo declared = null;
        long deadline = 0;
        while (true) {
            synchronized (this) {
                ContentProviderHolder published = mPublished.get(name);
                if (published != null) {
                    ActivityTaskManagerService.ProcessRecord owner = mPublishedBy.get(name);
                    if (caller != null && owner != null && caller != owner) {
                        if (mClients.computeIfAbsent(owner, k -> new HashSet<>()).add(caller)) {
                            SystemServer.log("getContentProvider " + name + " from "
                                    + callingPackage + ": " + owner + " has a client, "
                                    + caller);
                        }
                    }
                    ContentProviderHolder h = new ContentProviderHolder(published.info);
                    h.provider = published.provider;
                    h.noReleaseNeeded = true;
                    return h;
                }
                if (deadline != 0) {
                    long left = deadline - SystemClock.uptimeMillis();
                    if (left <= 0) {
                        SystemServer.log("getContentProvider " + name + " from "
                                + callingPackage + ": " + declared.packageName
                                + " did not publish it in time");
                        return null;
                    }
                    try {
                        wait(left);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return null;
                    }
                    continue;
                }
            }
            declared = mPm.provider(name, PackageManager.GET_META_DATA);
            if (declared == null) {
                SystemServer.log("getContentProvider " + name + " from " + callingPackage
                        + ": no such provider");
                return null;
            }
            // Its process publishes it once it is bound (bindApplication installs it).
            SystemServer.log("getContentProvider " + name + " from " + callingPackage
                    + ": declared by " + declared.packageName + ", waiting for its process");
            deadline = SystemClock.uptimeMillis() + PUBLISH_TIMEOUT_MS;
            mAtm.withProcess(declared.packageName, "provider " + name, app -> { });
        }
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
            notifyAll();
        }
        for (ContentProviderHolder h : providers) {
            if (h == null || h.info == null) continue;
            SystemServer.log("publishContentProviders: " + h.info.authority + " ("
                    + h.info.name + ") in " + app);
        }
    }

    boolean refContentProvider(IBinder connection, int stable, int unstable) {
        return true;  // connections are counted per client process, not per reference
    }

    /** Whether processes of other packages use the process's providers. */
    synchronized boolean hasClients(ActivityTaskManagerService.ProcessRecord app) {
        Set<ActivityTaskManagerService.ProcessRecord> clients = mClients.get(app);
        return clients != null && !clients.isEmpty();
    }

    /** The process is exiting: its providers are not handed out any more. */
    synchronized void unpublish(ActivityTaskManagerService.ProcessRecord app) {
        mPublishedBy.entrySet().removeIf(e -> {
            if (e.getValue() != app) return false;
            mPublished.remove(e.getKey());
            return true;
        });
    }

    /**
     * The process went away: so did the providers it published, and its use of others'.
     * Returns the processes that lost their last client.
     */
    synchronized List<ActivityTaskManagerService.ProcessRecord> processDied(
            ActivityTaskManagerService.ProcessRecord app) {
        unpublish(app);
        mClients.remove(app);
        List<ActivityTaskManagerService.ProcessRecord> unused = new ArrayList<>();
        for (Map.Entry<ActivityTaskManagerService.ProcessRecord,
                Set<ActivityTaskManagerService.ProcessRecord>> e : mClients.entrySet()) {
            if (e.getValue().remove(app) && e.getValue().isEmpty()) unused.add(e.getKey());
        }
        return unused;
    }
}
