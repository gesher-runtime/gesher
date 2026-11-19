/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.os.IBinder;
import android.os.Process;
import android.util.AtomicFile;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserFactory;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * Android URI grants, keyed by recipient UID and GrantUri, for the supported user 0.
 * Installed metadata is locked before grant state. File IO and owner retirement
 * never call the package manager while holding the grant lock.
 */
public final class UriGrants {
    private static final int ACCESS = Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private static final int ADVANCED = Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;
    private static final int MAX_PERSISTED_URI_GRANTS = 512;
    private static final ScheduledExecutorService IO = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "uri-grants-persistence");
        thread.setDaemon(true);
        return thread;
    });

    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final CallerIdentity mCallers;
    private final ContentProviders mProviders;
    private final File mStateFile;
    private final Object mFileLock = new Object();

    // These tables and modes are all guarded by this. No package/provider IO is
    // performed from an owner callback, a state transition, or a file snapshot.
    private final Map<Integer, Map<GrantIdentity, Permission>> mGrants = new HashMap<>();
    private final Map<IBinder, Set<Permission>> mOwned = new HashMap<>();
    private final Set<IBinder> mTrackedOwners = Collections.newSetFromMap(new WeakHashMap<>());
    private ScheduledFuture<?> mPendingWrite;
    private long mWriteGeneration;
    private boolean mLoaded;

    private record GrantIdentity(Uri uri, boolean prefix) {}

    private static final class Source {
        final ProviderInfo info;
        final ApkPackage installed;
        final ContentProviders.RegisteredProvider registered;
        Source(ProviderInfo info, ApkPackage installed, ContentProviders.RegisteredProvider registered) {
            this.info = info; this.installed = installed; this.registered = registered;
        }
        String durableOwner() { return installed == null ? null : installed.enabledStateOwner(); }
        int uid() { return installed != null ? installed.uid() : info.applicationInfo.uid; }
    }

    /** Prepared grants retain installation/registration generations until publication. */
    static final class Grant {
        final Source source;
        final ApkPackage target;
        final Uri uri;
        final int flags;
        final int fromUid;
        final boolean fromProvider;
        Grant(Source source, ApkPackage target, Uri uri, int flags, int fromUid, boolean fromProvider) {
            this.source = source; this.target = target; this.uri = uri;
            this.flags = flags; this.fromUid = fromUid; this.fromProvider = fromProvider;
        }
    }

    private static final class Permission {
        final String sourcePackage, targetPackage;
        final String sourceOwner, targetOwner;
        final int targetUid;
        final GrantIdentity identity;
        final UriPermissionModes<IBinder> modes = new UriPermissionModes<>();
        Source transientSource;
        ApkPackage transientTarget;

        Permission(Source source, ApkPackage target, GrantIdentity identity) {
            sourcePackage = source.info.packageName; targetPackage = target.packageName;
            sourceOwner = source.durableOwner(); targetOwner = target.enabledStateOwner();
            targetUid = target.uid(); this.identity = identity;
        }
    }

    UriGrants(PackageManagerService pm, ActivityTaskManagerService atm) {
        this(pm, atm, null, new File(Device.dataDir(), "system/urigrants.xml"));
    }

    UriGrants(PackageManagerService pm, ActivityTaskManagerService atm, ContentProviders providers) {
        this(pm, atm, providers, new File(Device.dataDir(), "system/urigrants.xml"));
    }

    public UriGrants(PackageManagerService pm, ActivityTaskManagerService atm, File stateFile) {
        this(pm, atm, null, stateFile);
    }

    UriGrants(PackageManagerService pm, ActivityTaskManagerService atm,
            ContentProviders providers, File stateFile) {
        mPm = pm; mAtm = atm; mProviders = providers;
        mCallers = new CallerIdentity(pm, atm); mStateFile = stateFile;
        pm.onPackageRemoval(this::clear);
    }

    static Uri checkedUri(Uri uri, int userId) {
        if (userId != 0 || uri == null || !"content".equals(uri.getScheme())
                || uri.getAuthority() == null || uri.getAuthority().isEmpty()) {
            throw new SecurityException("Only content URIs for user 0 are available");
        }
        String authority = uri.getAuthority();
        if (authority.indexOf('@') >= 0) {
            if (!authority.startsWith("0@") || authority.indexOf('@', 2) >= 0) {
                throw new SecurityException("Cross-user content URI");
            }
            authority = authority.substring(2);
            if (authority.isEmpty()) throw new SecurityException("Missing content authority");
        }
        for (String segment : uri.getPathSegments()) {
            if (".".equals(segment) || "..".equals(segment)) {
                throw new SecurityException("Ambiguous URI path");
            }
        }
        return uri.buildUpon().authority(authority).build();
    }

    private static boolean matches(Uri granted, boolean prefix, Uri requested) {
        if (!Objects.equals(granted.getScheme(), requested.getScheme())
                || !Objects.equals(granted.getAuthority(), requested.getAuthority())) return false;
        if (!prefix) return granted.equals(requested);
        List<String> a = granted.getPathSegments(), b = requested.getPathSegments();
        return b.size() >= a.size() && b.subList(0, a.size()).equals(a);
    }

    private boolean current(ApkPackage pkg) {
        return pkg != null && mPm.get(pkg.packageName) == pkg
                && !mPm.isRemovalPending(pkg.packageName);
    }

    /** Called with the package manager monitor, before the grant monitor. */
    private Source source(String authority) {
        ContentProviders.RegisteredProvider registered = mProviders == null ? null
                : mProviders.registeredProvider(authority);
        ProviderInfo installed = ContentProviders.declaredProvider(mPm, authority);
        if (registered != null) {
            if (installed != null) throw new SecurityException("Conflicting content provider authority");
            if (registered.info.applicationInfo == null
                    || !mProviders.isCurrentRegistration(registered)) return null;
            return new Source(registered.info, null, registered);
        }
        if (installed == null) return null;
        ApkPackage owner = mPm.get(installed.packageName);
        return current(owner) ? new Source(installed, owner, null) : null;
    }

    private boolean current(Source source, Uri uri) {
        if (source == null) return false;
        if (source.registered != null) {
            return mProviders != null && mProviders.isCurrentRegistration(source.registered)
                    && mProviders.registeredProvider(uri.getAuthority()) == source.registered;
        }
        if (!current(source.installed)) return false;
        ProviderInfo info = ContentProviders.declaredProvider(mPm, uri.getAuthority());
        return info != null && source.info.packageName.equals(info.packageName)
                && Objects.equals(source.info.name, info.name);
    }

    private boolean current(Permission permission) {
        return current(permission, source(permission.identity.uri.getAuthority()));
    }

    private boolean current(Permission permission, Source source) {
        ApkPackage target = mPm.get(permission.targetPackage);
        if (!current(target) || target.uid() != permission.targetUid
                || !Objects.equals(permission.targetOwner, target.enabledStateOwner())) return false;
        return source != null && permission.sourcePackage.equals(source.info.packageName)
                && Objects.equals(permission.sourceOwner, source.durableOwner());
    }

    /** Retained permission state survives disabled providers, but not installation retirement. */
    private boolean installedParticipants(Permission permission) {
        ApkPackage target = mPm.get(permission.targetPackage);
        if (!current(target) || target.uid() != permission.targetUid
                || !Objects.equals(permission.targetOwner, target.enabledStateOwner())) return false;
        ApkPackage source = mPm.get(permission.sourcePackage);
        if (source != null) {
            return current(source) && Objects.equals(permission.sourceOwner, source.enabledStateOwner());
        }
        ContentProviders.RegisteredProvider registered = mProviders == null ? null
                : mProviders.registeredProvider(permission.identity.uri.getAuthority());
        return registered != null && mProviders.isCurrentRegistration(registered)
                && permission.sourceOwner == null
                && permission.sourcePackage.equals(registered.info.packageName);
    }

    private boolean holdsLocked(int uid, Source source, Uri uri, int flags, boolean requirePrefix) {
        return holdsLocked(uid, source, uri, flags, requirePrefix, false);
    }

    private boolean holdsRetainedLocked(int uid, Uri uri, int flags, boolean requirePrefix) {
        return holdsLocked(uid, null, uri, flags, requirePrefix, true);
    }

    /** Access and delegation match a resolved provider; state queries validate retained owners. */
    private boolean holdsLocked(int uid, Source source, Uri uri, int flags, boolean requirePrefix,
            boolean retained) {
        Map<GrantIdentity, Permission> grants = mGrants.get(uid);
        if (grants == null) return false;
        retireStaleTransientsLocked(grants);
        if (grants.isEmpty()) { mGrants.remove(uid); return false; }
        int strength = (flags & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) != 0
                ? UriPermissionModes.PERSISTABLE : UriPermissionModes.OWNED;
        Permission exact = grants.get(new GrantIdentity(uri, requirePrefix));
        if (exact != null && (retained ? installedParticipants(exact) : current(exact, source))
                && exact.modes.strength(flags) >= strength) return true;
        for (Permission permission : grants.values()) {
            if (permission.identity.prefix && matches(permission.identity.uri, true, uri)
                    && (retained ? installedParticipants(permission) : current(permission, source))
                    && permission.modes.strength(flags) >= strength) return true;
        }
        return false;
    }

    /** Runtime object generations retire transient state; durable owner epochs survive updates. */
    private void retireStaleTransientsLocked(Map<GrantIdentity, Permission> grants) {
        for (Iterator<Permission> it = grants.values().iterator(); it.hasNext();) {
            Permission permission = it.next();
            Source source = permission.transientSource;
            if (source == null) continue;
            boolean sourceCurrent = source.installed != null
                    ? mPm.get(source.installed.packageName) == source.installed
                    : mProviders != null && mProviders.isCurrentRegistration(source.registered);
            boolean targetCurrent = permission.transientTarget != null
                    && mPm.get(permission.targetPackage) == permission.transientTarget;
            if (!sourceCurrent || !targetCurrent) {
                revokeLocked(permission, ACCESS, true);
                permission.transientSource = null;
                permission.transientTarget = null;
                if (permission.modes.active() == 0) it.remove();
            }
        }
    }

    int check(Uri original, int pid, int uid, int modes, int userId) {
        Uri uri;
        try { uri = checkedUri(original, userId); }
        catch (SecurityException invalid) { return PackageManager.PERMISSION_DENIED; }
        if ((modes & ACCESS) == 0 || (modes & ~(ACCESS | ADVANCED)) != 0) {
            return PackageManager.PERMISSION_DENIED;
        }
        if (uid == Process.SYSTEM_UID || uid == Process.ROOT_UID || uid == Process.SHELL_UID) {
            return pid == Process.myPid() || (mCallers.isSystem() && pid == mCallers.callerPid()
                    && uid == mCallers.callerUid()) ? PackageManager.PERMISSION_GRANTED
                    : PackageManager.PERMISSION_DENIED;
        }
        String name = mAtm.authenticatedPackageForPid(pid);
        synchronized (mPm) {
            ApkPackage target = name == null ? null : mPm.get(name);
            if (!current(target) || target.uid() != uid) return PackageManager.PERMISSION_DENIED;
            synchronized (this) {
                if (holdsRetainedLocked(uid, uri, modes,
                        (modes & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0)) {
                    return PackageManager.PERMISSION_GRANTED;
                }
            }
            Source source = source(uri.getAuthority());
            return source != null && source.uid() == uid ? PackageManager.PERMISSION_GRANTED
                    : PackageManager.PERMISSION_DENIED;
        }
    }

    int checkFull(Uri original, int pid, int uid, int modes, int userId) {
        if (check(original, pid, uid, modes, userId) == PackageManager.PERMISSION_GRANTED) {
            return PackageManager.PERMISSION_GRANTED;
        }
        String name = mAtm.authenticatedPackageForPid(pid);
        ApkPackage target = name == null ? null : mPm.get(name);
        return target != null && target.uid() == uid && canAccess(target, original, modes, userId)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    public boolean canAccess(ApkPackage target, Uri original, int modes, int userId) {
        Uri uri;
        try { uri = checkedUri(original, userId); }
        catch (SecurityException invalid) { return false; }
        if ((modes & ACCESS) == 0 || (modes & ~ACCESS) != 0) return false;
        synchronized (mPm) {
            if (!current(target)) return false;
            if ("settings".equals(uri.getAuthority())) {
                if ((modes & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) == 0) return true;
                String table = uri.getPathSegments().isEmpty() ? "" : uri.getPathSegments().get(0);
                return ("system".equals(table) && granted(target.uid(), "android.permission.WRITE_SETTINGS"))
                        || (("secure".equals(table) || "global".equals(table))
                                && granted(target.uid(), "android.permission.WRITE_SECURE_SETTINGS"));
            }
            Source source = source(uri.getAuthority());
            if (source == null) return false;
            if (hasDirectAccess(target.uid(), source, uri, modes)) return true;
            synchronized (this) { return holdsLocked(target.uid(), source, uri, modes, false); }
        }
    }

    boolean canAccessForCaller(ApkPackage target, Uri original, int modes, int userId) {
        Uri uri;
        try { uri = checkedUri(original, userId); }
        catch (SecurityException invalid) { return false; }
        int access = modes & ACCESS;
        synchronized (mPm) {
            if (!current(target) || access == 0) return false;
            if ("settings".equals(uri.getAuthority())) return canAccess(target, uri, access, userId);
            Source source = source(uri.getAuthority());
            if (source == null) return false;
            if (hasDirectAccess(target.uid(), source, uri, access)) return true;
            synchronized (this) {
                return holdsLocked(target.uid(), source, uri, modes,
                        (modes & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0);
            }
        }
    }

    private boolean hasDirectAccess(int uid, Source source, Uri uri, int modes) {
        if (uid < 0 || (modes & ACCESS) == 0) return false;
        if (uid == source.uid()) return true;
        ProviderInfo info = source.info;
        if (!info.exported || info.forceUriPermissions) return false;
        if ((modes & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0
                && !directAllowed(uid, info, uri, true)) return false;
        if ((modes & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0
                && !directAllowed(uid, info, uri, false)) return false;
        return true;
    }

    private boolean granted(int uid, String permission) {
        return mPm.permissions().checkUidPermission(uid, permission, 0) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean directAllowed(int uid, ProviderInfo info, Uri uri, boolean read) {
        String base = read ? info.readPermission : info.writePermission;
        if (base != null && granted(uid, base)) return true;
        boolean defaultAllowed = base == null;
        if (info.pathPermissions != null) {
            for (android.content.pm.PathPermission path : info.pathPermissions) {
                String permission = read ? path.getReadPermission() : path.getWritePermission();
                if (permission != null && path.match(uri.getPath())) {
                    if (granted(uid, permission)) return true;
                    defaultAllowed = false;
                }
            }
        }
        return defaultAllowed;
    }

    boolean hasAny(ApkPackage target, ProviderInfo info) {
        synchronized (mPm) {
            if (!current(target)) return false;
            synchronized (this) {
                Map<GrantIdentity, Permission> grants = mGrants.get(target.uid());
                if (grants == null) return false;
                retireStaleTransientsLocked(grants);
                if (grants.isEmpty()) { mGrants.remove(target.uid()); return false; }
                for (Permission permission : grants.values()) {
                    if (permission.sourcePackage.equals(info.packageName)
                            && ContentProviders.hasAuthority(info, permission.identity.uri.getAuthority())
                            && permission.modes.active() != 0 && current(permission)) return true;
                }
                return false;
            }
        }
    }

    public List<android.content.UriPermission> list(String packageName, boolean incoming,
            boolean persistedOnly) {
        List<android.content.UriPermission> result = new ArrayList<>();
        synchronized (mPm) {
            synchronized (this) {
                Iterable<Map<GrantIdentity, Permission>> tables = mGrants.values();
                if (incoming) {
                    ApkPackage target = mPm.get(packageName);
                    Map<GrantIdentity, Permission> grants = target == null ? null : mGrants.get(target.uid());
                    if (grants == null) return result;
                    tables = List.of(grants);
                }
                for (Map<GrantIdentity, Permission> grants : tables) {
                    retireStaleTransientsLocked(grants);
                    for (Permission permission : grants.values()) {
                        if (packageName.equals(incoming ? permission.targetPackage : permission.sourcePackage)
                                && (!persistedOnly || permission.modes.persisted != 0) && installedParticipants(permission)) {
                            result.add(new android.content.UriPermission(permission.identity.uri,
                                    permission.modes.persisted, permission.modes.persistedTime));
                        }
                    }
                }
                mGrants.values().removeIf(Map::isEmpty);
            }
        }
        return result;
    }

    List<android.app.GrantedUriPermission> granted(String packageName) {
        List<android.app.GrantedUriPermission> result = new ArrayList<>();
        synchronized (mPm) {
            synchronized (this) {
                for (Map<GrantIdentity, Permission> grants : mGrants.values()) {
                    retireStaleTransientsLocked(grants);
                    for (Permission permission : grants.values()) {
                        if ((packageName == null || packageName.equals(permission.targetPackage))
                                && permission.modes.persisted != 0 && installedParticipants(permission)) {
                            result.add(new android.app.GrantedUriPermission(permission.identity.uri,
                                    permission.targetPackage));
                        }
                    }
                }
                mGrants.values().removeIf(Map::isEmpty);
            }
        }
        return result;
    }

    void clearGranted(String packageName) { removePackage(packageName, true, true); }
    void clearTransient(String packageName) { removePackage(packageName, false, false); }
    void clear(String packageName) { removePackage(packageName, true, false); }

    private synchronized void removePackage(String packageName, boolean persistable, boolean targetOnly) {
        boolean changed = false;
        for (Map<GrantIdentity, Permission> grants : mGrants.values()) {
            for (Iterator<Permission> it = grants.values().iterator(); it.hasNext();) {
                Permission permission = it.next();
                if (packageName == null || packageName.equals(permission.targetPackage)
                        || (!targetOnly && packageName.equals(permission.sourcePackage))) {
                    changed |= revokeLocked(permission, ACCESS
                            | (persistable ? Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION : 0), true);
                    if (permission.modes.active() == 0) it.remove();
                }
            }
        }
        mGrants.values().removeIf(Map::isEmpty);
        if (changed) scheduleWriteLocked();
    }

    void grant(String targetPackage, Uri original, int modes, int userId) {
        if (!mCallers.isSystem()) mCallers.callerPackage();
        if (targetPackage == null) throw new IllegalArgumentException("null target");
        if ((modes & ~(ACCESS | ADVANCED)) != 0) throw new IllegalArgumentException("Invalid URI grant flags");
        // ActivityManager builds an Intent: absent data or access modes add no URI.
        if (original == null || (modes & ACCESS) == 0) return;
        Grant grant = prepare(mCallers.callerUid(), targetPackage, original, modes, userId, userId);
        if (grant != null) publishChecked(null, List.of(grant));
    }

    private static void grantable(ProviderInfo info, Uri uri) {
        boolean allowed = info.grantUriPermissions;
        if (info.uriPermissionPatterns != null && info.uriPermissionPatterns.length != 0) {
            allowed = false;
            for (android.os.PatternMatcher pattern : info.uriPermissionPatterns) {
                if (pattern != null && pattern.match(uri.getPath())) { allowed = true; break; }
            }
        }
        if (!allowed) throw new SecurityException("Provider does not allow URI grants");
    }

    private Grant prepare(int uid, String targetName, Uri original, int flags,
            int sourceUser, int targetUser) {
        if (sourceUser != 0 || targetUser != 0) throw new SecurityException("Only user 0 is available");
        Objects.requireNonNull(original, "uri");
        int access = flags & ACCESS;
        if (access == 0 || !"content".equals(original.getScheme())) return null;
        Uri uri = checkedUri(original, sourceUser);
        if ((uid == Process.SYSTEM_UID || uid == Process.ROOT_UID)
                && !"com.android.settings.files".equals(uri.getAuthority())
                && !"com.android.settings.module_licenses".equals(uri.getAuthority())) return null;
        synchronized (mPm) {
            Source source = source(uri.getAuthority());
            ApkPackage target = targetName == null ? null : mPm.get(targetName);
            if (source == null || (targetName != null && target == null)) return null;
            if (target != null && !current(target)) throw new SecurityException("Retiring URI grant target");
            boolean targetHolds = target == null ? publiclyAccessible(source.info, uri, access)
                    : hasDirectAccess(target.uid(), source, uri, access);
            if (!source.info.forceUriPermissions && (flags & ADVANCED) == 0 && targetHolds) return null;
            grantable(source.info, uri);
            boolean direct = hasDirectAccess(uid, source, uri, flags);
            synchronized (this) {
                if (!direct && !holdsLocked(uid, source, uri, flags,
                        (flags & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0)) {
                    throw new SecurityException("Caller cannot delegate this URI with these modes");
                }
            }
            // A privileged null-target query checks delegatability but has no recipient.
            return target == null ? null : new Grant(source, target, uri, flags, uid, false);
        }
    }

    private static boolean publiclyAccessible(ProviderInfo info, Uri uri, int access) {
        boolean allowed = info.exported;
        if ((access & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0 && info.readPermission != null) allowed = false;
        if ((access & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0 && info.writePermission != null) allowed = false;
        if (info.pathPermissions != null) {
            for (android.content.pm.PathPermission path : info.pathPermissions) {
                if (path != null && path.match(uri.getPath())) {
                    if ((access & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0 && path.getReadPermission() != null) allowed = false;
                    if ((access & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0 && path.getWritePermission() != null) allowed = false;
                    break;
                }
            }
        }
        return allowed;
    }

    /** Only a real server provider registration can issue this internal grant. */
    void grantFromProvider(ContentProviders.RegisteredProvider registration, String targetPackage,
            Uri original, int flags) {
        grantFromProviderChecked(registration, null, targetPackage, original, flags);
    }

    void grantFromProvider(ContentProviders.RegisteredProvider registration, IBinder owner,
            String targetPackage, Uri original, int flags) {
        if (owner == null) throw new IllegalArgumentException("Missing URI permission owner");
        grantFromProviderChecked(registration, owner, targetPackage, original, flags);
    }

    private void grantFromProviderChecked(ContentProviders.RegisteredProvider registration, IBinder owner,
            String targetPackage, Uri original, int flags) {
        publishChecked(owner, List.of(prepareFromProvider(registration, targetPackage, original, flags)));
    }

    /** Prepare a complete picker result before atomically publishing its owner memberships. */
    Grant prepareFromProvider(ContentProviders.RegisteredProvider registration,
            String targetPackage, Uri original, int flags) {
        Uri uri = checkedUri(original, 0);
        if ((flags & ACCESS) == 0 || (flags & ~(ACCESS | ADVANCED)) != 0) {
            throw new IllegalArgumentException("Invalid URI grant flags");
        }
        synchronized (mPm) {
            if (mProviders == null || !mProviders.isCurrentRegistration(registration)
                    || mProviders.registeredProvider(uri.getAuthority()) != registration) {
                throw new SecurityException("Provider registration is no longer current");
            }
            Source source = source(uri.getAuthority());
            ApkPackage target = mPm.get(Objects.requireNonNull(targetPackage, "targetPackage"));
            if (source == null || source.registered != registration || !current(target)) {
                throw new SecurityException("Unknown URI grant participant");
            }
            grantable(source.info, uri);
            return new Grant(source, target, uri, flags, source.uid(), true);
        }
    }

    void revoke(String targetPackage, Uri original, int flags, int userId) {
        int uid = mCallers.callerUid();
        if (!mCallers.isSystem()) mCallers.callerPackage();
        if (original == null || (flags & ACCESS) == 0) return;
        Uri uri = checkedUri(original, userId);
        synchronized (mPm) {
            Source source = source(uri.getAuthority());
            if (source == null) return;
            boolean direct = hasDirectAccess(uid, source, uri, flags);
            synchronized (this) {
                revokePermissionsLocked(targetPackage, source.info.packageName, uid, uri, flags, direct);
            }
        }
    }

    /** A hosted provider's normal Context.revokeUriPermission, bound to its real registration. */
    void revokeFromProvider(ContentProviders.RegisteredProvider registration, Uri original, int flags) {
        revokeFromProvider(registration, null, original, flags);
    }

    void revokeFromProvider(ContentProviders.RegisteredProvider registration,
            String targetPackage, Uri original, int flags) {
        if (original == null || (flags & ACCESS) == 0) return;
        Uri uri = checkedUri(original, 0);
        synchronized (mPm) {
            if (mProviders == null || !mProviders.isCurrentRegistration(registration)
                    || mProviders.registeredProvider(uri.getAuthority()) != registration) {
                throw new SecurityException("Provider registration is no longer current");
            }
            Source source = source(uri.getAuthority());
            if (source == null || source.registered != registration) {
                throw new SecurityException("Unknown URI permission source");
            }
            synchronized (this) {
                revokePermissionsLocked(targetPackage, source.info.packageName, source.uid(), uri, flags, true);
            }
        }
    }

    private void revokePermissionsLocked(String targetPackage, String sourcePackage, int uid,
            Uri uri, int flags, boolean direct) {
        boolean changed = false;
        for (Map.Entry<Integer, Map<GrantIdentity, Permission>> entry : mGrants.entrySet()) {
            if (!direct && entry.getKey() != uid) continue;
            for (Iterator<Permission> it = entry.getValue().values().iterator(); it.hasNext();) {
                Permission permission = it.next();
                if ((targetPackage == null || targetPackage.equals(permission.targetPackage))
                        && permission.sourcePackage.equals(sourcePackage)
                        && matches(uri, true, permission.identity.uri)) {
                    changed |= revokeLocked(permission, flags | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION,
                            direct && targetPackage == null);
                    if (permission.modes.active() == 0) it.remove();
                }
            }
        }
        mGrants.values().removeIf(Map::isEmpty);
        if (changed) scheduleWriteLocked();
    }

    private boolean revokeLocked(Permission permission, int flags, boolean includingOwners) {
        Set<IBinder> before = includingOwners ? permission.modes.owners() : Collections.emptySet();
        boolean changed = permission.modes.revoke(flags, includingOwners);
        for (IBinder owner : before) {
            if (!permission.modes.ownedBy(owner)) unindexOwnerLocked(owner, permission);
        }
        if ((permission.modes.global | permission.modes.owned()) == 0) {
            permission.transientSource = null;
            permission.transientTarget = null;
        }
        return changed;
    }

    private void unindexOwnerLocked(IBinder owner, Permission permission) {
        Set<Permission> owned = mOwned.get(owner);
        if (owned != null) {
            owned.remove(permission);
            if (owned.isEmpty()) mOwned.remove(owner);
        }
    }

    private void removeLocked(Permission permission) {
        for (IBinder owner : permission.modes.owners()) unindexOwnerLocked(owner, permission);
        Map<GrantIdentity, Permission> grants = mGrants.get(permission.targetUid);
        if (grants != null) {
            grants.remove(permission.identity, permission);
            if (grants.isEmpty()) mGrants.remove(permission.targetUid);
        }
    }

    void takePersistable(int uid, Uri original, int flags, int userId) {
        checkPersistableFlags(flags);
        Uri uri = checkedUri(original, userId);
        synchronized (mPm) {
            synchronized (this) {
                Map<GrantIdentity, Permission> grants = mGrants.get(uid);
                if (grants != null) retireStaleTransientsLocked(grants);
                Permission exact = grants == null ? null : grants.get(new GrantIdentity(uri, false));
                Permission prefix = grants == null ? null : grants.get(new GrantIdentity(uri, true));
                boolean exactValid = exact != null && installedParticipants(exact)
                        && (exact.modes.persistable & flags) == flags;
                boolean prefixValid = prefix != null && installedParticipants(prefix)
                        && (prefix.modes.persistable & flags) == flags;
                if (!exactValid && !prefixValid) throw new SecurityException("No persistable URI grant for UID " + uid);
                boolean changed = false;
                long now = System.currentTimeMillis();
                if (exactValid) changed |= exact.modes.take(flags, now);
                if (prefixValid) changed |= prefix.modes.take(flags, now);
                changed |= pruneLocked(uid);
                if (changed) scheduleWriteLocked();
            }
        }
    }

    void releasePersistable(int uid, Uri original, int flags, int userId, boolean requireGrant) {
        checkPersistableFlags(flags);
        Uri uri = checkedUri(original, userId);
        synchronized (mPm) {
            synchronized (this) {
                Map<GrantIdentity, Permission> grants = mGrants.get(uid);
                if (grants != null) retireStaleTransientsLocked(grants);
                Permission exact = grants == null ? null : grants.get(new GrantIdentity(uri, false));
                Permission prefix = grants == null ? null : grants.get(new GrantIdentity(uri, true));
                if (exact == null && prefix == null && requireGrant) {
                    throw new SecurityException("No URI grant for UID " + uid);
                }
                boolean changed = false;
                if (exact != null) {
                    changed |= exact.modes.release(flags);
                    if (exact.modes.active() == 0) removeLocked(exact);
                }
                if (prefix != null) {
                    changed |= prefix.modes.release(flags);
                    if (prefix.modes.active() == 0) removeLocked(prefix);
                }
                if (changed) scheduleWriteLocked();
            }
        }
    }

    private static void checkPersistableFlags(int flags) {
        if ((flags & ~ACCESS) != 0) throw new IllegalArgumentException("Invalid URI grant flags: " + flags);
    }

    private boolean pruneLocked(int uid) {
        Map<GrantIdentity, Permission> grants = mGrants.get(uid);
        if (grants == null || grants.size() <= MAX_PERSISTED_URI_GRANTS) return false;
        List<Permission> persisted = new ArrayList<>();
        for (Permission permission : grants.values()) {
            if (permission.modes.persisted != 0) persisted.add(permission);
        }
        int excess = persisted.size() - MAX_PERSISTED_URI_GRANTS;
        if (excess <= 0) return false;
        persisted.sort(java.util.Comparator.comparingLong(permission -> permission.modes.persistedTime));
        for (int i = 0; i < excess; i++) {
            Permission permission = persisted.get(i);
            permission.modes.release(ACCESS);
            if (permission.modes.active() == 0) removeLocked(permission);
        }
        return true;
    }

    synchronized IBinder newOwner() {
        IBinder token = new android.os.Binder();
        mTrackedOwners.add(token);
        return token;
    }

    void grantFromOwner(IBinder owner, int fromUid, String targetPackage, Uri uri,
            int modes, int sourceUserId, int targetUserId) {
        synchronized (this) {
            if (owner == null || !mTrackedOwners.contains(owner)) {
                throw new IllegalArgumentException("Unknown URI permission owner");
            }
        }
        if (fromUid != mCallers.callerUid() && !mCallers.isSystem()) {
            throw new SecurityException("Cannot grant on behalf of another UID");
        }
        if (!mCallers.isSystem()) mCallers.callerPackage();
        if (targetPackage == null) throw new IllegalArgumentException("null target");
        if (uri == null) throw new IllegalArgumentException("null uri");
        Grant grant = prepare(fromUid, targetPackage, uri, modes, sourceUserId, targetUserId);
        if (grant != null) publish(owner, List.of(grant));
    }

    List<Grant> prepareIntent(int uid, String target, Intent root) {
        List<Grant> result = new ArrayList<>();
        if (root == null) return result;
        int modes = root.getFlags() & (ACCESS | ADVANCED);
        if ((modes & ACCESS) == 0) return result;
        Set<Intent> seen = Collections.newSetFromMap(new java.util.IdentityHashMap<>());
        java.util.ArrayDeque<Intent> pending = new java.util.ArrayDeque<>();
        pending.add(root);
        while (!pending.isEmpty()) {
            Intent intent = pending.removeFirst();
            if (!seen.add(intent)) continue;
            if (intent.getData() != null) {
                Grant grant = prepare(uid, target, intent.getData(), modes, 0, 0);
                if (grant != null) result.add(grant);
            }
            android.content.ClipData clip = intent.getClipData();
            if (clip == null) continue;
            for (int i = 0; i < clip.getItemCount(); i++) {
                android.content.ClipData.Item item = clip.getItemAt(i);
                if (item.getUri() != null) {
                    Grant grant = prepare(uid, target, item.getUri(), modes, 0, 0);
                    if (grant != null) result.add(grant);
                } else if (item.getIntent() != null) pending.add(item.getIntent());
            }
        }
        return result;
    }

    void publish(IBinder owner, List<Grant> grants) {
        if (owner == null) throw new IllegalArgumentException("Missing URI permission owner");
        publishChecked(owner, grants);
    }

    private void publishChecked(IBinder owner, List<Grant> grants) {
        synchronized (mPm) {
            // Permission-policy lookups precede the grant monitor. The table
            // validation below still rechecks every delegated owner/global mode.
            boolean[] direct = new boolean[grants.size()];
            for (int i = 0; i < grants.size(); i++) {
                Grant grant = grants.get(i);
                direct[i] = grant.fromProvider
                        || hasDirectAccess(grant.fromUid, grant.source, grant.uri, grant.flags);
            }
            synchronized (this) {
                // Validate the complete prepared set before adding any membership.
                for (int i = 0; i < grants.size(); i++) {
                    Grant grant = grants.get(i);
                    if (!current(grant.target) || !current(grant.source, grant.uri)) {
                        throw new SecurityException("Retired URI grant participant");
                    }
                    if (!direct[i] && !holdsLocked(grant.fromUid, grant.source, grant.uri, grant.flags,
                            (grant.flags & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0)) {
                        throw new SecurityException("URI grant source no longer has permission");
                    }
                }
                for (Grant grant : grants) {
                    GrantIdentity identity = new GrantIdentity(grant.uri,
                            (grant.flags & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0);
                    Map<GrantIdentity, Permission> target = mGrants.computeIfAbsent(grant.target.uid(),
                            unused -> new LinkedHashMap<>());
                    retireStaleTransientsLocked(target);
                    Permission permission = target.get(identity);
                    if (permission != null && !current(permission, grant.source)) {
                        boolean changed = revokeLocked(permission,
                                ACCESS | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION, true);
                        target.remove(identity);
                        if (changed) scheduleWriteLocked();
                        permission = null;
                    }
                    if (permission == null) {
                        permission = new Permission(grant.source, grant.target, identity);
                        target.put(identity, permission);
                    }
                    permission.transientSource = grant.source;
                    permission.transientTarget = mPm.get(permission.targetPackage);
                    permission.modes.grant(grant.flags, owner);
                    if (owner != null) mOwned.computeIfAbsent(owner, unused -> new HashSet<>()).add(permission);
                }
            }
        }
    }

    synchronized void revokeByOwner(IBinder owner) {
        if (owner == null) return;
        Set<Permission> permissions = mOwned.remove(owner);
        if (permissions == null) return;
        for (Permission permission : permissions) {
            permission.modes.removeOwner(owner, ACCESS);
            if ((permission.modes.global | permission.modes.owned()) == 0) {
                permission.transientSource = null;
                permission.transientTarget = null;
            }
            if (permission.modes.active() == 0) removeLocked(permission);
        }
    }

    int checkGrantUriPermission_ignoreNonSystem(int sourceUid, String targetPackage, Uri original,
            int modes, int userId) {
        Grant grant = prepare(sourceUid, targetPackage, original, modes, userId, userId);
        return grant == null ? -1 : grant.target.uid();
    }

    private record StoredGrant(String sourcePackage, String targetPackage, String sourceOwner,
            String targetOwner, Uri uri, boolean prefix, int modes, long createdTime) {}

    private void scheduleWriteLocked() {
        if (mStateFile == null || mPendingWrite != null) return;
        long generation = ++mWriteGeneration;
        mPendingWrite = IO.schedule(() -> writeState(generation), 10, TimeUnit.SECONDS);
    }

    private boolean writeState(long generation) {
        List<StoredGrant> snapshot = new ArrayList<>();
        synchronized (this) {
            if (generation == mWriteGeneration) mPendingWrite = null;
            for (Map<GrantIdentity, Permission> grants : mGrants.values()) {
                for (Permission permission : grants.values()) {
                    if (permission.modes.persisted != 0) {
                        snapshot.add(new StoredGrant(permission.sourcePackage, permission.targetPackage,
                                permission.sourceOwner, permission.targetOwner, permission.identity.uri,
                                permission.identity.prefix, permission.modes.persisted,
                                permission.modes.persistedTime));
                    }
                }
            }
        }
        if (mStateFile == null) return true;
        synchronized (mFileLock) {
            AtomicFile file = new AtomicFile(mStateFile);
            FileOutputStream stream = null;
            try {
                stream = file.startWrite();
                BufferedWriter writer = new BufferedWriter(new OutputStreamWriter(stream, StandardCharsets.UTF_8));
                writer.write("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<uri-grants>\n");
                for (StoredGrant grant : snapshot) {
                    writer.write("  <uri-grant");
                    attribute(writer, "sourcePkg", grant.sourcePackage);
                    attribute(writer, "targetPkg", grant.targetPackage);
                    attribute(writer, "sourceOwner", grant.sourceOwner);
                    attribute(writer, "targetOwner", grant.targetOwner);
                    attribute(writer, "uri", grant.uri.toString());
                    attribute(writer, "prefix", Boolean.toString(grant.prefix));
                    attribute(writer, "mode", Integer.toString(grant.modes));
                    attribute(writer, "createdTime", Long.toString(grant.createdTime));
                    writer.write(" />\n");
                }
                writer.write("</uri-grants>\n");
                writer.flush();
                file.finishWrite(stream);
                return true;
            } catch (IOException failure) {
                file.failWrite(stream);
                SystemServer.log("uri-grants: cannot save " + mStateFile + ": " + failure);
                return false;
            }
        }
    }

    private static void attribute(BufferedWriter writer, String name, String value) throws IOException {
        if (value == null) return;
        writer.write(" "); writer.write(name); writer.write("=\"");
        for (int i = 0; i < value.length(); i++) {
            switch (value.charAt(i)) {
                case '&': writer.write("&amp;"); break;
                case '<': writer.write("&lt;"); break;
                case '>': writer.write("&gt;"); break;
                case '"': writer.write("&quot;"); break;
                case '\'': writer.write("&apos;"); break;
                default: writer.write(value.charAt(i));
            }
        }
        writer.write("\"");
    }

    /** Orderly service/test shutdown waits for real IO, outside every state lock. */
    public void flush() throws IOException {
        if (mStateFile == null) return;
        if (Thread.holdsLock(this) || Thread.holdsLock(mPm)) {
            throw new IllegalStateException("Cannot wait for URI persistence while holding state locks");
        }
        Future<Boolean> written;
        synchronized (this) {
            if (mPendingWrite != null) {
                mPendingWrite.cancel(false);
                mPendingWrite = null;
            }
            ++mWriteGeneration;
            written = IO.submit(() -> writeState(0));
        }
        try {
            if (!written.get()) throw new IOException("URI permission persistence failed");
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IOException("Interrupted while persisting URI permissions", interrupted);
        } catch (ExecutionException failure) {
            throw new IOException("URI permission persistence failed", failure.getCause());
        }
    }

    public void load() {
        synchronized (this) {
            if (mLoaded) return;
            mLoaded = true;
        }
        if (mStateFile == null) return;
        List<StoredGrant> restored = new ArrayList<>();
        synchronized (mFileLock) {
            try (InputStream in = new AtomicFile(mStateFile).openRead()) {
                XmlPullParser parser = XmlPullParserFactory.newInstance().newPullParser();
                parser.setInput(in, "UTF-8");
                int event;
                do { event = parser.next(); } while (event != XmlPullParser.START_TAG
                        && event != XmlPullParser.END_DOCUMENT);
                if (event != XmlPullParser.START_TAG || !"uri-grants".equals(parser.getName())) return;
                while ((event = parser.next()) != XmlPullParser.END_DOCUMENT) {
                    if (event != XmlPullParser.START_TAG || !"uri-grant".equals(parser.getName())) continue;
                    String source = parser.getAttributeValue(null, "sourcePkg");
                    String target = parser.getAttributeValue(null, "targetPkg");
                    String text = parser.getAttributeValue(null, "uri");
                    String modes = parser.getAttributeValue(null, "mode");
                    String timestamp = parser.getAttributeValue(null, "createdTime");
                    if (source == null || target == null || text == null || modes == null) continue;
                    int access = Integer.parseInt(modes) & ACCESS;
                    if (access == 0) continue;
                    Uri uri;
                    try { uri = checkedUri(Uri.parse(text), 0); }
                    catch (SecurityException invalid) { continue; }
                    restored.add(new StoredGrant(source, target,
                            parser.getAttributeValue(null, "sourceOwner"),
                            parser.getAttributeValue(null, "targetOwner"), uri,
                            Boolean.parseBoolean(parser.getAttributeValue(null, "prefix")), access,
                            timestamp == null ? System.currentTimeMillis() : Long.parseLong(timestamp)));
                }
            } catch (FileNotFoundException absent) {
                return;
            } catch (Exception failure) {
                SystemServer.log("uri-grants: cannot load " + mStateFile + ": " + failure);
                return;
            }
        }
        synchronized (mPm) {
            synchronized (this) {
                for (StoredGrant saved : restored) {
                    Source source = source(saved.uri.getAuthority());
                    ApkPackage target = mPm.get(saved.targetPackage);
                    if (source == null || !saved.sourcePackage.equals(source.info.packageName)
                            || !current(target)
                            || (saved.sourceOwner != null
                                    && !saved.sourceOwner.equals(source.durableOwner()))
                            || (saved.targetOwner != null
                                    && !saved.targetOwner.equals(target.enabledStateOwner()))) continue;
                    GrantIdentity identity = new GrantIdentity(saved.uri, saved.prefix);
                    Map<GrantIdentity, Permission> targetGrants = mGrants.computeIfAbsent(target.uid(),
                            unused -> new LinkedHashMap<>());
                    Permission permission = targetGrants.get(identity);
                    if (permission == null) {
                        permission = new Permission(source, target, identity);
                        targetGrants.put(identity, permission);
                    }
                    permission.modes.restore(saved.modes, saved.createdTime);
                }
            }
        }
    }
}
