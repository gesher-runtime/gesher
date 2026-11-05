/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.os.Binder;
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
    private final SettingsProvider mSettings;
    private final CallerIdentity mCallers;
    private final UriGrants mUriGrants;

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
        mCallers = new CallerIdentity(pm, atm);
        mUriGrants = new UriGrants(pm, atm);
        mSettings = new SettingsProvider(pm, atm);
    }

    UriGrants uriGrants() { return mUriGrants; }

    static boolean hasAuthority(ProviderInfo info, String authority) {
        if (info == null || info.authority == null || authority == null) return false;
        for (String declared : info.authority.split(";")) if (authority.equals(declared)) return true;
        return false;
    }

    /** Ambiguous declarations never select an attacker simply because it installed first. */
    static ProviderInfo declaredProvider(PackageManagerService pm, String authority) {
        ProviderInfo found = null;
        for (ApkPackage pkg : pm.packages()) {
            for (ProviderInfo info : pkg.providers(PackageManager.GET_META_DATA
                    | PackageManager.GET_SHARED_LIBRARY_FILES)) {
                if (!hasAuthority(info, authority)) continue;
                if (found != null && (!found.packageName.equals(info.packageName)
                        || !found.name.equals(info.name))) {
                    throw new SecurityException("Conflicting content provider authority");
                }
                found = info;
            }
        }
        return found;
    }

    private String callerPackage(ActivityTaskManagerService.ProcessRecord caller, String claimed) {
        if (mCallers.isSystem()) {
            // A server-local on-behalf-of-app operation keeps that app's permissions.
            if (claimed == null || "android".equals(claimed)) return null;
            if (mPm.get(claimed) == null) throw new SecurityException("Unknown content caller");
            return claimed;
        }
        String actual = mCallers.requirePackage(claimed);
        if (caller == null || caller.exiting || caller.pid != Binder.getCallingPid()
                || !actual.equals(caller.packageName)) {
            throw new SecurityException("Content provider acquisition requires its attached caller");
        }
        return actual;
    }

    private boolean holds(String pkg, String permission) {
        return permission == null || permission.isEmpty()
                || mPm.permissions().checkPermission(pkg, permission) == PackageManager.PERMISSION_GRANTED;
    }

    private void enforceAccess(String pkg, ProviderInfo info) {
        if (pkg == null || pkg.equals(info.packageName)) return;
        ApkPackage target = mPm.get(pkg);
        boolean uriGrant = target != null && mUriGrants.hasAny(target, info);
        if (!info.exported && !uriGrant) throw new SecurityException("Content provider is not exported");
        if (uriGrant || holds(pkg, info.readPermission) || holds(pkg, info.writePermission)) return;
        if (info.pathPermissions != null) {
            for (android.content.pm.PathPermission path : info.pathPermissions) {
                if ((path.getReadPermission() != null && holds(pkg, path.getReadPermission()))
                        || (path.getWritePermission() != null && holds(pkg, path.getWritePermission()))) return;
            }
        }
        throw new SecurityException("Content provider permissions denied");
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
        String actualPackage = callerPackage(caller, callingPackage);
        if (SettingsProvider.AUTHORITY.equals(name)) {
            return mSettings.holder();
        }
        ProviderInfo declared = null;
        long deadline = 0;
        while (true) {
            declared = declaredProvider(mPm, name);
            if (declared == null) return null;
            enforceAccess(actualPackage, declared);
            Set<ActivityTaskManagerService.ProcessRecord> live = new HashSet<>(mAtm.processes());
            synchronized (this) {
                ContentProviderHolder published = mPublished.get(name);
                if (published != null) {
                    ActivityTaskManagerService.ProcessRecord owner = mPublishedBy.get(name);
                    if (owner == null || owner.exiting || !declared.packageName.equals(owner.packageName)
                            || !declared.name.equals(published.info.name)
                            || !live.contains(owner)) {
                        mPublished.remove(name); mPublishedBy.remove(name);
                        continue;
                    }
                    if (caller != null && owner != null && caller != owner) {
                        if (mClients.computeIfAbsent(owner, k -> new HashSet<>()).add(caller)) {
                            SystemServer.log("getContentProvider " + name + " from "
                                    + callingPackage + ": " + owner + " has a client, "
                                    + caller);
                        }
                    }
                    ContentProviderHolder h = new ContentProviderHolder(new ProviderInfo(declared));
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
            // A provider's onCreate can request a sibling before bindApplication has
            // published either. ActivityThread installs a local holder itself; waiting
            // here would wait for the calling thread to finish its own onCreate.
            // Gesher currently hosts all of a package's components in one process.
            if (caller != null && declared.packageName.equals(caller.packageName)) {
                ContentProviderHolder local = new ContentProviderHolder(declared);
                local.mLocal = true;
                local.noReleaseNeeded = true;
                return local;
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
        if (app == null || app.packageName == null || app.exiting
                || mAtm.callerProcess(app.pid) != app) {
            throw new SecurityException("Unknown provider publisher");
        }
        if (!mCallers.isSystem() && (!app.packageName.equals(mCallers.callerPackage())
                || app.pid != Binder.getCallingPid())) {
            throw new SecurityException("Provider publisher identity mismatch");
        }
        ApkPackage pkg = mPm.get(app.packageName);
        if (pkg == null) throw new SecurityException("Uninstalled provider publisher");
        List<ContentProviderHolder> checked = new ArrayList<>();
        Set<String> batch = new HashSet<>();
        for (ContentProviderHolder supplied : providers) {
            if (supplied == null || supplied.info == null || supplied.provider == null
                    || supplied.provider.asBinder() == null) {
                throw new SecurityException("Incomplete provider publication");
            }
            ProviderInfo info = supplied.info;
            ProviderInfo declared = null;
            for (ProviderInfo candidate : pkg.providers(PackageManager.GET_META_DATA
                    | PackageManager.GET_SHARED_LIBRARY_FILES)) {
                if (candidate.name.equals(info.name)) { declared = candidate; break; }
            }
            if (declared == null || !pkg.packageName.equals(info.packageName)
                    || info.applicationInfo == null || info.applicationInfo.uid != pkg.uid()
                    || !pkg.packageName.equals(info.applicationInfo.packageName)
                    || !java.util.Objects.equals(declared.authority, info.authority)
                    || declared.authority == null || declared.authority.isEmpty()) {
                throw new SecurityException("Provider does not match the package declaration");
            }
            for (String authority : declared.authority.split(";")) {
                if (authority.isEmpty() || SettingsProvider.AUTHORITY.equals(authority)
                        || !batch.add(authority)) throw new SecurityException("Invalid published authority");
                ProviderInfo canonical = declaredProvider(mPm, authority);
                if (canonical == null || !pkg.packageName.equals(canonical.packageName)
                        || !declared.name.equals(canonical.name)) {
                    throw new SecurityException("Provider authority belongs to another component");
                }
            }
            ContentProviderHolder trusted = new ContentProviderHolder(new ProviderInfo(declared));
            trusted.provider = supplied.provider;
            trusted.noReleaseNeeded = true;
            checked.add(trusted);
        }
        Set<ActivityTaskManagerService.ProcessRecord> live = new HashSet<>(mAtm.processes());
        synchronized (this) {
            // Validate the entire batch before mutating any authority mapping.
            for (ContentProviderHolder h : checked) {
                for (String authority : h.info.authority.split(";")) {
                    ActivityTaskManagerService.ProcessRecord owner = mPublishedBy.get(authority);
                    if (owner != null && owner != app && !owner.exiting
                            && live.contains(owner)) {
                        throw new SecurityException("Provider already has a live publisher");
                    }
                }
            }
            for (ContentProviderHolder h : checked) {
                for (String authority : h.info.authority.split(";")) {
                    mPublished.put(authority, h);
                    mPublishedBy.put(authority, app);
                }
            }
            notifyAll();
        }
        for (ContentProviderHolder h : checked) {
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
