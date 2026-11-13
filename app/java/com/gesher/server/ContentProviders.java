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
 * <li>The processes of other packages that got a process's provider are its clients:
 *     while it has any, nothing ends it; when the last one is gone, it may exit
 *     ({@link ActivityTaskManagerService#maybeExit}).
 * <li>Each client process also holds a reference-counted connection to each provider it
 *     acquired (ContentProviderConnection, {@link Connection}): ActivityThread counts its
 *     stable and unstable references, tells {@code refContentProvider} when either kind
 *     appears or goes, and {@code removeContentProvider} once it released the last one
 *     (one second later, CONTENT_PROVIDER_RETAIN_TIME). A provider process whose
 *     connections all have no references left is in no client's use
 *     ({@link #hasActiveClients}): the idle reaper may end it (ADR 0082), and the next
 *     acquisition starts it again.
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
    /** The reference-counted connections, by their Binder (ContentProviderHolder.connection). */
    private final Map<IBinder, Connection> mConnections = new HashMap<>();

    /**
     * ContentProviderConnection: one client process's references to one published
     * provider. The client gets it as {@code ContentProviderHolder.connection} and names it
     * in refContentProvider and removeContentProvider.
     */
    static final class Connection extends Binder {
        final ActivityTaskManagerService.ProcessRecord owner;
        final ActivityTaskManagerService.ProcessRecord client;
        final IBinder provider;
        final String authority;
        int stable;
        int unstable;

        Connection(ActivityTaskManagerService.ProcessRecord owner,
                ActivityTaskManagerService.ProcessRecord client, IBinder provider, String authority) {
            super("ContentProviderConnection");
            this.owner = owner;
            this.client = client;
            this.provider = provider;
            this.authority = authority;
        }

        @Override
        public String toString() {
            return authority + " for " + client + " (stable " + stable + ", unstable "
                    + unstable + ")";
        }
    }

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
            if (pm.isRemovalPending(pkg.packageName)) continue;
            for (ProviderInfo info : pkg.providers(PackageManager.GET_META_DATA
                    | PackageManager.GET_URI_PERMISSION_PATTERNS
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
        if (p == null) return List.of();
        List<ProviderInfo> result = new ArrayList<>();
        String processName = mAtm.componentProcess(ai.packageName, ai.processName);
        for (ProviderInfo info : p.providers(PackageManager.GET_META_DATA
                | PackageManager.GET_SHARED_LIBRARY_FILES)) {
            if (processName.equals(mAtm.componentProcess(info.packageName, info.processName))) {
                result.add(info);
            }
        }
        return result;
    }

    /**
     * The provider of authority {@code name} for {@code caller} (its process, null for a
     * caller that is not an app process); starts the provider's process if needed.
     */
    ContentProviderHolder getContentProvider(ActivityTaskManagerService.ProcessRecord caller,
            String callingPackage, String name) {
        return getContentProvider(caller, callingPackage, name, false, false);
    }

    /**
     * {@link #getContentProvider(ActivityTaskManagerService.ProcessRecord, String, String)}
     * for an app's ActivityThread: with {@code counted}, a holder of another process's
     * provider carries a reference-counted {@link Connection} (one more stable or unstable
     * reference) that the client releases; otherwise it needs no release.
     */
    ContentProviderHolder getContentProvider(ActivityTaskManagerService.ProcessRecord caller,
            String callingPackage, String name, boolean stable, boolean counted) {
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
                            || !mAtm.componentProcess(declared.packageName, declared.processName)
                                    .equals(mAtm.componentProcess(owner.packageName, owner.processName))
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
                    if (counted && caller != null && owner != null && caller != owner) {
                        Connection c = connectionLocked(owner, caller,
                                published.provider.asBinder(), name);
                        if (stable) c.stable++; else c.unstable++;
                        h.connection = c;
                        h.noReleaseNeeded = false;
                        owner.lastActive = SystemClock.uptimeMillis();
                    }
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
            if (caller != null && declared.packageName.equals(caller.packageName)
                    && mAtm.componentProcess(declared.packageName, declared.processName)
                            .equals(mAtm.componentProcess(caller.packageName, caller.processName))) {
                ContentProviderHolder local = new ContentProviderHolder(declared);
                local.mLocal = true;
                local.noReleaseNeeded = true;
                return local;
            }
            // Its process publishes it once it is bound (bindApplication installs it).
            SystemServer.log("getContentProvider " + name + " from " + callingPackage
                    + ": declared by " + declared.packageName + ", waiting for its process");
            deadline = SystemClock.uptimeMillis() + PUBLISH_TIMEOUT_MS;
            mAtm.withProcess(declared.packageName, declared.processName, "provider " + name, app -> { });
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
                    || declared.authority == null || declared.authority.isEmpty()
                    || !mAtm.componentProcess(declared.packageName, declared.processName)
                            .equals(mAtm.componentProcess(app.packageName, app.processName))) {
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

    /** The client's connection to the provider, made on its first acquisition. Locked. */
    private Connection connectionLocked(ActivityTaskManagerService.ProcessRecord owner,
            ActivityTaskManagerService.ProcessRecord client, IBinder provider, String authority) {
        for (Connection c : mConnections.values()) {
            if (c.client == client && c.provider == provider && c.owner == owner) return c;
        }
        Connection c = new Connection(owner, client, provider, authority);
        mConnections.put(c, c);
        return c;
    }

    /** The connection, if it is the calling client's (or the caller is the system). Locked. */
    private Connection callerConnectionLocked(IBinder connection) {
        Connection c = connection == null ? null : mConnections.get(connection);
        if (c == null) return null;
        if (!mCallers.isSystem() && c.client.pid != Binder.getCallingPid()) {
            throw new SecurityException("Provider connection belongs to another process");
        }
        return c;
    }

    /**
     * ActivityThread changes its counts: a first stable or unstable reference, or the last
     * one of a kind gone (stable to unstable while the removal is pending). Returns whether
     * the connection still stands.
     */
    synchronized boolean refContentProvider(IBinder connection, int stable, int unstable) {
        Connection c = callerConnectionLocked(connection);
        if (c == null) return false;
        c.stable = Math.max(0, c.stable + stable);
        c.unstable = Math.max(0, c.unstable + unstable);
        c.owner.lastActive = SystemClock.uptimeMillis();
        return !c.owner.exiting;
    }

    /**
     * ActivityThread released its last reference of a kind (completeRemoveProvider), or a
     * duplicate holder it did not need (installProvider lost a race): once the connection
     * has no reference left, it is gone.
     */
    synchronized void removeContentProvider(IBinder connection, boolean stable) {
        Connection c = callerConnectionLocked(connection);
        if (c == null) return;
        if (stable) c.stable = Math.max(0, c.stable - 1);
        else c.unstable = Math.max(0, c.unstable - 1);
        c.owner.lastActive = SystemClock.uptimeMillis();
        if (c.stable == 0 && c.unstable == 0) mConnections.remove(c);
    }

    /** Whether processes of other packages use the process's providers. */
    synchronized boolean hasClients(ActivityTaskManagerService.ProcessRecord app) {
        Set<ActivityTaskManagerService.ProcessRecord> clients = mClients.get(app);
        return clients != null && !clients.isEmpty();
    }

    /**
     * Whether a live client still holds a stable or unstable reference to one of the
     * process's providers: then ending the process would break a client (a stable
     * reference's DeadObjectException).
     */
    synchronized boolean hasActiveClients(ActivityTaskManagerService.ProcessRecord app) {
        for (Connection c : mConnections.values()) {
            if (c.owner == app && !c.client.exiting && (c.stable > 0 || c.unstable > 0)) return true;
        }
        return false;
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
        mConnections.values().removeIf(c -> c.client == app || c.owner == app);
        List<ActivityTaskManagerService.ProcessRecord> unused = new ArrayList<>();
        for (Map.Entry<ActivityTaskManagerService.ProcessRecord,
                Set<ActivityTaskManagerService.ProcessRecord>> e : mClients.entrySet()) {
            if (e.getValue().remove(app) && e.getValue().isEmpty()) unused.add(e.getKey());
        }
        return unused;
    }
}
