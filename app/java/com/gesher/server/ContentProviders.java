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
import android.os.RemoteException;
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
    private final DocumentsStorageProvider mDocumentsStorage;
    private final CallerIdentity mCallers;
    private final UriGrants mUriGrants;

    /** Real server-hosted provider registrations, independent of installed APKs. */
    static final class RegisteredProvider {
        final ProviderInfo info;
        final android.content.IContentProvider provider;
        RegisteredProvider(ProviderInfo info, android.content.IContentProvider provider) {
            this.info = new ProviderInfo(java.util.Objects.requireNonNull(info));
            this.provider = java.util.Objects.requireNonNull(provider);
        }
    }
    private volatile Map<String, RegisteredProvider> mRegisteredProviders = Map.of();

    RegisteredProvider registeredProvider(String authority) {
        return mRegisteredProviders.get(authority);
    }

    boolean isCurrentRegistration(RegisteredProvider registration) {
        if (registration == null || registration.info.authority == null) return false;
        for (String authority : registration.info.authority.split(";")) {
            if (registeredProvider(authority) != registration) return false;
        }
        return true;
    }

    /** Called only by construction of an actual server provider and its transport. */
    void registerProvider(RegisteredProvider registration) {
        if (registration.info.authority == null || registration.info.authority.isEmpty()) {
            throw new IllegalArgumentException("Provider has no authority");
        }
        synchronized (mPm) {
            Map<String, RegisteredProvider> next = new HashMap<>(mRegisteredProviders);
            for (String authority : registration.info.authority.split(";")) {
                if (next.containsKey(authority) || declaredProvider(mPm, authority) != null) {
                    throw new IllegalArgumentException("Provider authority is already registered");
                }
                next.put(authority, registration);
            }
            mRegisteredProviders = Map.copyOf(next);
        }
    }

    // Guarded by this. Published providers, by authority.
    private final Map<String, ContentProviderHolder> mPublished = new HashMap<>();
    private final Map<String, ActivityTaskManagerService.ProcessRecord> mPublishedBy =
            new HashMap<>();
    private final Map<String, ApkPackage> mPublishedCode = new HashMap<>();
    /** Actual bindApplication/scheduleInstallProvider admissions, by process incarnation. */
    private final Map<ActivityTaskManagerService.ProcessRecord, Map<String, ProviderInstall>> mInstalling = new HashMap<>();
    private final Set<ProviderRequest> mRequests = new HashSet<>();

    private static final class ProviderInstall {
        final ApkPackage owner;
        final ProviderInfo info;
        volatile boolean retired;
        ProviderInstall(ApkPackage owner, ProviderInfo info) { this.owner = owner; this.info = info; }
    }

    private static final class ProviderRequest {
        final ApkPackage owner;
        final ProviderInfo info;
        volatile boolean canceled;
        ProviderInstall installation;
        ProviderRequest(ApkPackage owner, ProviderInfo info) { this.owner = owner; this.info = info; }
    }

    private boolean current(ProviderInstall install) {
        return install != null && !install.retired && mPm.get(install.owner.packageName) == install.owner
                && !mPm.isRemovalPending(install.owner.packageName)
                && install.owner.componentEnabled(install.info.name, ApkPackage.ComponentKind.PROVIDER);
    }
    /** Keep acquisition ownership per authority, so removing one provider preserves peers. */
    private final Map<String, Set<ActivityTaskManagerService.ProcessRecord>> mClients = new HashMap<>();
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
        final ApkPackage ownerCode;
        final ApkPackage clientCode;
        final String component;
        int stable;
        int unstable;

        Connection(ActivityTaskManagerService.ProcessRecord owner,
                ActivityTaskManagerService.ProcessRecord client, IBinder provider, String authority,
                ApkPackage ownerCode, ApkPackage clientCode, String component) {
            super("ContentProviderConnection");
            this.owner = owner;
            this.client = client;
            this.provider = provider;
            this.authority = authority;
            this.ownerCode = ownerCode;
            this.clientCode = clientCode;
            this.component = component;
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
        mUriGrants = new UriGrants(pm, atm, this);
        mSettings = new SettingsProvider(pm, atm);
        mDocumentsStorage = new DocumentsStorageProvider(this, mUriGrants);
        pm.setHostedProviders(this);
    }

    UriGrants uriGrants() { return mUriGrants; }
    DocumentsStorageProvider documents() { return mDocumentsStorage; }
    CallerIdentity callers() { return mCallers; }

    SettingsProvider settingsProvider() { return mSettings; }

    static boolean hasAuthority(ProviderInfo info, String authority) {
        if (info == null || info.authority == null || authority == null) return false;
        for (String declared : info.authority.split(";")) if (authority.equals(declared)) return true;
        return false;
    }

    /** Ambiguous declarations never select an attacker simply because it installed first. */
    static ProviderInfo declaredProvider(PackageManagerService pm, String authority) {
        synchronized (pm) {
            ProviderInfo found = null;
            for (ApkPackage pkg : pm.providerPackages(authority)) {
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
        if (pkg == null) return;
        synchronized (mPm) {
            ApkPackage target = mPm.get(pkg);
            if (target == null || target.uidPending() || target.uid() < 0
                    || mPm.isRemovalPending(pkg)) throw new SecurityException("Unknown content caller");
            ApkPackage owner = mPm.get(info.packageName);
            // Android provider ownership is per UID, including admitted shared-UID
            // packages. Invalid or retired metadata cannot establish that identity.
            if (info.applicationInfo != null && info.applicationInfo.uid >= 0
                    && owner != null && !owner.uidPending() && !mPm.isRemovalPending(owner.packageName)
                    && owner.uid() == info.applicationInfo.uid && target.uid() == owner.uid()) return;
            boolean uriGrant = mUriGrants.hasAny(target, info);
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
    }

    /**
     * {@code pkg}'s registered providers, one per component. ApkPackage selects the
     * first declaration before applying installed state, as ComponentResolver does.
     * Bind and publication therefore agree with authority lookup even when duplicate
     * declarations differ in process, enabled state, permissions or authorities.
     */
    static List<ProviderInfo> declaredProviders(ApkPackage pkg, long flags) {
        return pkg.providers(flags);
    }

    /** The providers a process of {@code ai}'s package installs at bindApplication. */
    List<ProviderInfo> providersFor(ApplicationInfo ai) {
        return providersFor(null, ai);
    }

    List<ProviderInfo> providersFor(ActivityTaskManagerService.ProcessRecord app, ApplicationInfo ai) {
        ApkPackage p = mPm.get(ai.packageName);
        if (p == null) return List.of();
        List<ProviderInfo> result = new ArrayList<>();
        String processName = mAtm.componentProcess(ai.packageName, ai.processName);
        for (ProviderInfo info : declaredProviders(p, PackageManager.GET_META_DATA
                | PackageManager.GET_SHARED_LIBRARY_FILES)) {
            if (processName.equals(mAtm.componentProcess(info.packageName, info.processName))) {
                result.add(info);
            }
        }
        // AOSP ContentProviderHelper.generateApplicationProvidersLocked initializes
        // providers in descending initOrder and preserves manifest order for ties.
        result.sort((a, b) -> Integer.compare(b.initOrder, a.initOrder));
        if (app != null) synchronized (this) {
            if (app.exiting || app.dead || app.packageCode != p
                    || mPm.get(p.packageName) != p || mPm.isRemovalPending(p.packageName)) return List.of();
            Map<String, ProviderInstall> installing = mInstalling.computeIfAbsent(app, ignored -> new HashMap<>());
            result.removeIf(info -> !p.componentEnabled(info.name, ApkPackage.ComponentKind.PROVIDER));
            for (ProviderInfo info : result) {
                ProviderInstall install = new ProviderInstall(p, info);
                installing.put(info.name, install);
                for (ProviderRequest request : mRequests) {
                    if (!request.canceled && request.owner == p && request.info.name.equals(info.name)) {
                        request.installation = install;
                    }
                }
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
        if (DocumentsStorageProvider.AUTHORITY.equals(name)) {
            RegisteredProvider registration = registeredProvider(name);
            if (registration == null || declaredProvider(mPm, name) != null) {
                throw new SecurityException("Documents authority is not uniquely registered");
            }
            enforceAccess(actualPackage, registration.info);
            return mDocumentsStorage.holder();
        }
        final ProviderInfo declared;
        final ApkPackage declaredCode;
        synchronized (mPm) {
            declared = declaredProvider(mPm, name);
            if (declared == null) return null;
            declaredCode = mPm.get(declared.packageName);
        }
        if (declaredCode == null) return null;
        ProviderRequest request = new ProviderRequest(declaredCode, declared);
        long deadline = 0;
        synchronized (this) { mRequests.add(request); }
        try {
        while (true) {
            if (request.canceled || mPm.get(declared.packageName) != declaredCode
                    || mPm.isRemovalPending(declared.packageName)) return null;
            ProviderInfo current = declaredProvider(mPm, name);
            if (current == null || !declared.packageName.equals(current.packageName)
                    || !declared.name.equals(current.name)) return null;
            enforceAccess(actualPackage, declared);
            Set<ActivityTaskManagerService.ProcessRecord> live = new HashSet<>(mAtm.processes());
            synchronized (this) {
                if (request.canceled || mPm.get(declared.packageName) != declaredCode
                        || !declaredCode.componentEnabled(declared.name, ApkPackage.ComponentKind.PROVIDER)) return null;
                if (caller != null && (caller.exiting || caller.dead)) {
                    throw new SecurityException("Content provider caller retired before acquisition");
                }
                ContentProviderHolder published = mPublished.get(name);
                if (published != null) {
                    ActivityTaskManagerService.ProcessRecord owner = mPublishedBy.get(name);
                    if (owner == null || owner.exiting || owner.dead || mPublishedCode.get(name) != declaredCode
                            || !declared.packageName.equals(owner.packageName)
                            || !declared.name.equals(published.info.name)
                            || !mAtm.componentProcess(declared.packageName, declared.processName)
                                    .equals(mAtm.componentProcess(owner.packageName, owner.processName))
                            || !live.contains(owner)) {
                        mPublished.remove(name); mPublishedBy.remove(name); mPublishedCode.remove(name);
                        mClients.remove(name);
                        continue;
                    }
                    if (caller != null && owner != null && caller != owner) {
                        if (mClients.computeIfAbsent(name, k -> new HashSet<>()).add(caller)) {
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
                                published.provider.asBinder(), name, declaredCode, declared.name);
                        if (stable) c.stable++; else c.unstable++;
                        h.connection = c;
                        h.noReleaseNeeded = false;
                        owner.markActive(SystemClock.uptimeMillis());
                    }
                    return h;
                }
                if (request.installation == null) {
                    for (ActivityTaskManagerService.ProcessRecord owner : live) {
                        ProviderInstall install = mInstalling.getOrDefault(owner, Map.of()).get(declared.name);
                        if (install != null && install.owner == declaredCode && current(install)) {
                            request.installation = install;
                            break;
                        }
                    }
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
                synchronized (this) {
                    if (request.canceled || caller.exiting || caller.dead
                            || mPm.get(declared.packageName) != declaredCode
                            || !declaredCode.componentEnabled(declared.name, ApkPackage.ComponentKind.PROVIDER)) return null;
                    mInstalling.computeIfAbsent(caller, ignored -> new HashMap<>())
                            .computeIfAbsent(declared.name, ignored -> new ProviderInstall(declaredCode, declared));
                    ContentProviderHolder local = new ContentProviderHolder(declared);
                    local.mLocal = true;
                    local.noReleaseNeeded = true;
                    return local;
                }
            }
            // Its process publishes it once it is bound (bindApplication installs it).
            SystemServer.log("getContentProvider " + name + " from " + callingPackage
                    + ": declared by " + declared.packageName + ", waiting for its process");
            deadline = SystemClock.uptimeMillis() + PUBLISH_TIMEOUT_MS;
            mAtm.withProcess(declaredCode, declared.name, ApkPackage.ComponentKind.PROVIDER,
                    declared.processName, "provider " + name, app -> installProvider(app, request), () -> {
                        synchronized (ContentProviders.this) { request.canceled = true; notifyAll(); }
                    });
        }
        } finally {
            synchronized (this) { mRequests.remove(request); }
        }
    }

    /** A provider can be enabled after its process has already bound its application. */
    private void installProvider(ActivityTaskManagerService.ProcessRecord app, ProviderRequest request) {
        ProviderInstall install;
        synchronized (this) {
            if (request.canceled || app.exiting || app.dead || app.packageCode != request.owner
                    || mPm.get(request.owner.packageName) != request.owner
                    || mPm.isRemovalPending(request.owner.packageName)
                    || !request.owner.componentEnabled(request.info.name, ApkPackage.ComponentKind.PROVIDER)) {
                request.canceled = true; notifyAll(); return;
            }
            Map<String, ProviderInstall> installing = mInstalling.computeIfAbsent(app, ignored -> new HashMap<>());
            ProviderInstall existing = installing.get(request.info.name);
            if (current(existing)) { request.installation = existing; return; }
            install = new ProviderInstall(request.owner, request.info);
            installing.put(request.info.name, install);
            request.installation = install;
        }
        try {
            if (current(install) && !app.exiting && !app.dead) app.thread.scheduleInstallProvider(request.info);
        } catch (RemoteException | RuntimeException failure) {
            synchronized (this) {
                install.retired = true;
                Map<String, ProviderInstall> installing = mInstalling.get(app);
                if (installing != null) {
                    installing.remove(request.info.name, install);
                    if (installing.isEmpty()) mInstalling.remove(app);
                }
                for (ProviderRequest waiting : mRequests) {
                    if (waiting.installation == install) waiting.canceled = true;
                }
                notifyAll();
            }
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
        if (pkg == null || app.packageCode != pkg) return;
        Map<String, ProviderInstall> installing;
        synchronized (this) { installing = new HashMap<>(mInstalling.getOrDefault(app, Map.of())); }
        List<ContentProviderHolder> checked = new ArrayList<>();
        List<ProviderInstall> admissions = new ArrayList<>();
        Set<String> batch = new HashSet<>();
        publication: for (ContentProviderHolder supplied : providers) {
            if (supplied == null || supplied.info == null || supplied.provider == null
                    || supplied.provider.asBinder() == null) {
                continue;
            }
            ProviderInfo info = supplied.info;
            ProviderInstall admission = installing.get(info.name);
            // The process no longer owns a pending provider record after cleanup.
            // A late publish cannot recreate it or turn an ordinary race into a crash.
            if (!current(admission) || admission.owner != pkg) continue;
            ProviderInfo declared = null;
            for (ProviderInfo candidate : declaredProviders(pkg, PackageManager.GET_META_DATA
                    | PackageManager.GET_SHARED_LIBRARY_FILES | PackageManager.MATCH_DISABLED_COMPONENTS)) {
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
                    if (!current(admission)) continue publication;
                    throw new SecurityException("Provider authority belongs to another component");
                }
            }
            ContentProviderHolder trusted = new ContentProviderHolder(new ProviderInfo(declared));
            trusted.provider = supplied.provider;
            trusted.noReleaseNeeded = true;
            checked.add(trusted);
            admissions.add(admission);
        }
        Set<ActivityTaskManagerService.ProcessRecord> live = new HashSet<>(mAtm.processes());
        List<ContentProviderHolder> published = new ArrayList<>();
        synchronized (this) {
            if (app.exiting || app.dead) return;
            // Validate the entire batch before mutating any authority mapping.
            for (int i = 0; i < checked.size(); i++) {
                ContentProviderHolder h = checked.get(i);
                if (!current(admissions.get(i))
                        || mInstalling.getOrDefault(app, Map.of()).get(h.info.name) != admissions.get(i)) continue;
                for (String authority : h.info.authority.split(";")) {
                    ActivityTaskManagerService.ProcessRecord owner = mPublishedBy.get(authority);
                    if (owner != null && owner != app && !owner.exiting
                            && live.contains(owner)) {
                        throw new SecurityException("Provider already has a live publisher");
                    }
                }
            }
            for (int i = 0; i < checked.size(); i++) {
                ContentProviderHolder h = checked.get(i);
                if (!current(admissions.get(i))
                        || mInstalling.getOrDefault(app, Map.of()).get(h.info.name) != admissions.get(i)) continue;
                for (String authority : h.info.authority.split(";")) {
                    mPublished.put(authority, h);
                    mPublishedBy.put(authority, app);
                    mPublishedCode.put(authority, pkg);
                }
                published.add(h);
            }
            notifyAll();
        }
        for (ContentProviderHolder h : published) {
            SystemServer.log("publishContentProviders: " + h.info.authority + " ("
                    + h.info.name + ") in " + app);
        }
    }

    /** The client's connection to the provider, made on its first acquisition. Locked. */
    private Connection connectionLocked(ActivityTaskManagerService.ProcessRecord owner,
            ActivityTaskManagerService.ProcessRecord client, IBinder provider, String authority,
            ApkPackage code, String component) {
        for (Connection c : mConnections.values()) {
            if (c.client == client && c.provider == provider && c.owner == owner
                    && c.ownerCode == code && c.component.equals(component)) return c;
        }
        Connection c = new Connection(owner, client, provider, authority,
                code, mPm.get(client.packageName), component);
        mConnections.put(c, c);
        return c;
    }

    /** The connection, if it is the calling client's (or the caller is the system). Locked. */
    private Connection callerConnectionLocked(IBinder connection, boolean system, int pid, int uid) {
        Connection c = connection == null ? null : mConnections.get(connection);
        if (c == null) return null;
        if (!system && (c.client.pid != pid || c.client.uid != uid || c.client.exiting || c.client.dead)) {
            throw new SecurityException("Provider connection belongs to another process");
        }
        return c;
    }

    /**
     * ActivityThread changes its counts: a first stable or unstable reference, or the last
     * one of a kind gone (stable to unstable while the removal is pending). Returns whether
     * the connection still stands.
     */
    boolean refContentProvider(IBinder connection, int stable, int unstable) {
        // isSystem resolves the caller through ATM. Never acquire ATM under this monitor.
        int pid = Binder.getCallingPid(), uid = Binder.getCallingUid();
        boolean system = mCallers.isSystem();
        synchronized (this) {
            Connection c = callerConnectionLocked(connection, system, pid, uid);
            if (c == null) return false;
            if (mPm.get(c.ownerCode.packageName) != c.ownerCode
                    || !c.ownerCode.componentEnabled(c.component, ApkPackage.ComponentKind.PROVIDER)) return false;
            if (c.stable + stable < 0 || c.unstable + unstable < 0) {
                SystemServer.log("refContentProvider " + c + ": counts would go below zero ("
                        + stable + ", " + unstable + "); kept at zero");
            }
            c.stable = Math.max(0, c.stable + stable);
            c.unstable = Math.max(0, c.unstable + unstable);
            c.owner.markActive(SystemClock.uptimeMillis());
            return !c.owner.exiting && !c.owner.dead;
        }
    }

    /**
     * ActivityThread released its last reference of a kind (completeRemoveProvider), or a
     * duplicate holder it did not need (installProvider lost a race): once the connection
     * has no reference left, it is gone.
     */
    void removeContentProvider(IBinder connection, boolean stable) {
        int pid = Binder.getCallingPid(), uid = Binder.getCallingUid();
        boolean system = mCallers.isSystem();
        synchronized (this) {
            Connection c = callerConnectionLocked(connection, system, pid, uid);
            if (c == null) return;
            if (stable ? c.stable == 0 : c.unstable == 0) {
                SystemServer.log("removeContentProvider " + c + ": no "
                        + (stable ? "stable" : "unstable") + " reference to remove");
            }
            if (stable) c.stable = Math.max(0, c.stable - 1);
            else c.unstable = Math.max(0, c.unstable - 1);
            c.owner.markActive(SystemClock.uptimeMillis());
            if (c.stable == 0 && c.unstable == 0) mConnections.remove(c);
        }
    }

    /** Whether processes of other packages use the process's providers. */
    synchronized boolean hasClients(ActivityTaskManagerService.ProcessRecord app) {
        for (Map.Entry<String, Set<ActivityTaskManagerService.ProcessRecord>> clients : mClients.entrySet()) {
            if (mPublishedBy.get(clients.getKey()) == app && !clients.getValue().isEmpty()) return true;
        }
        return false;
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

    /**
     * Commit an idle exit under the provider lock, while the caller holds ATM and the
     * service/broadcast admission locks. An acquisition either keeps the process or sees
     * its committed exit; no temporary exiting flag can discard a still-live publisher.
     */
    synchronized boolean retireUnused(ActivityTaskManagerService.ProcessRecord app, long expectedActive) {
        if (app.exiting || app.dead || app.lastActive != expectedActive || hasActiveClients(app)) return false;
        app.reaped = true;
        app.exiting = true;
        unpublish(app);
        return true;
    }

    /** The process is exiting: its providers are not handed out any more. */
    synchronized void unpublish(ActivityTaskManagerService.ProcessRecord app) {
        Map<String, ProviderInstall> installing = mInstalling.remove(app);
        if (installing != null) for (ProviderInstall install : installing.values()) {
            install.retired = true;
            for (ProviderRequest request : mRequests) {
                if (request.installation == install) request.canceled = true;
            }
        }
        mPublishedBy.entrySet().removeIf(e -> {
            if (e.getValue() != app) return false;
            mPublished.remove(e.getKey());
            mPublishedCode.remove(e.getKey());
            mClients.remove(e.getKey());
            return true;
        });
        notifyAll();
    }

    void cleanupDisabledPackageComponents(PackageManagerService.EnabledChange change) {
        List<Connection> lost = new ArrayList<>();
        synchronized (this) {
            for (ProviderRequest request : mRequests) {
                if (change.disables(mPm, request.owner, request.info.name, ApkPackage.ComponentKind.PROVIDER)) request.canceled = true;
            }
            for (Map<String, ProviderInstall> installing : mInstalling.values()) {
                installing.values().removeIf(install -> {
                    if (!change.disables(mPm, install.owner, install.info.name, ApkPackage.ComponentKind.PROVIDER)) return false;
                    install.retired = true;
                    return true;
                });
            }
            mInstalling.values().removeIf(Map::isEmpty);
            for (String authority : new ArrayList<>(mPublished.keySet())) {
                ContentProviderHolder holder = mPublished.get(authority);
                if (!change.disables(mPm, mPublishedCode.get(authority), holder.info.name, ApkPackage.ComponentKind.PROVIDER)) continue;
                mPublished.remove(authority);
                mPublishedBy.remove(authority);
                mPublishedCode.remove(authority);
                mClients.remove(authority);
            }
            mConnections.values().removeIf(connection -> {
                if (!change.disables(mPm, connection.ownerCode, connection.component, ApkPackage.ComponentKind.PROVIDER)) return false;
                lost.add(connection);
                return true;
            });
            // A waiter must re-resolve disabled declarations instead of waiting two minutes.
            notifyAll();
        }
        for (Connection connection : lost) {
            if (connection.stable > 0) {
                mAtm.providerDependencyDied(connection.client, connection.clientCode);
            } else if (connection.unstable > 0 && !connection.client.exiting && !connection.client.dead
                    && connection.clientCode != null
                    && mPm.get(connection.client.packageName) == connection.clientCode
                    && mAtm.callerProcess(connection.client.pid) == connection.client) {
                try { connection.client.thread.unstableProviderDied(connection.provider); }
                catch (RemoteException ignored) { }
            }
        }
    }

    /**
     * The process went away: so did the providers it published, and its use of others'.
     * Returns the processes that lost their last client.
     */
    synchronized List<ActivityTaskManagerService.ProcessRecord> processDied(
            ActivityTaskManagerService.ProcessRecord app) {
        unpublish(app);
        mConnections.values().removeIf(c -> c.client == app || c.owner == app);
        List<ActivityTaskManagerService.ProcessRecord> unused = new ArrayList<>();
        for (Map.Entry<String,
                Set<ActivityTaskManagerService.ProcessRecord>> e : mClients.entrySet()) {
            if (e.getValue().remove(app) && e.getValue().isEmpty()) {
                ActivityTaskManagerService.ProcessRecord owner = mPublishedBy.get(e.getKey());
                if (owner != null && !hasClients(owner) && !unused.contains(owner)) unused.add(owner);
            }
        }
        return unused;
    }
}
