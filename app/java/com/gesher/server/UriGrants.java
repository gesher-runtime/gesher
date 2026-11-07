/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.os.Process;

import java.util.ArrayList;
import java.util.List;

/** Explicit, process-lifetime URI grants; unknown and persistable grants fail closed. */
final class UriGrants {
    private static final int ACCESS = Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final CallerIdentity mCallers;
    private final List<Grant> mGrants = new ArrayList<>();

    private static final class Grant {
        final ApkPackage owner, target;
        final Uri uri;
        final boolean prefix;
        int modes;
        Grant(ApkPackage owner, ApkPackage target, Uri uri, int modes, boolean prefix) {
            this.owner = owner; this.target = target; this.uri = uri;
            this.modes = modes; this.prefix = prefix;
        }
    }

    UriGrants(PackageManagerService pm, ActivityTaskManagerService atm) {
        mPm = pm; mAtm = atm; mCallers = new CallerIdentity(pm, atm);
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
        }
        for (String segment : uri.getPathSegments()) {
            if (".".equals(segment) || "..".equals(segment)) {
                throw new SecurityException("Ambiguous URI path");
            }
        }
        return uri.buildUpon().authority(authority).clearQuery().fragment(null).build();
    }

    private static boolean matches(Uri granted, boolean prefix, Uri requested) {
        if (!granted.getAuthority().equals(requested.getAuthority())) return false;
        if (!prefix) return granted.equals(requested);
        List<String> a = granted.getPathSegments(), b = requested.getPathSegments();
        return b.size() >= a.size() && b.subList(0, a.size()).equals(a);
    }

    int check(Uri original, int pid, int uid, int modes, int userId) {
        Uri uri;
        try { uri = checkedUri(original, userId); }
        catch (SecurityException invalid) { return PackageManager.PERMISSION_DENIED; }
        int access = modes & ACCESS;
        if (access == 0 || (modes & ~ACCESS) != 0) return PackageManager.PERMISSION_DENIED;
        if (uid == Process.SYSTEM_UID || uid == Process.ROOT_UID || uid == Process.SHELL_UID) {
            return pid == Process.myPid() || (mCallers.isSystem() && pid == mCallers.callerPid()
                    && uid == mCallers.callerUid()) ? PackageManager.PERMISSION_GRANTED
                    : PackageManager.PERMISSION_DENIED;
        }
        // A PID supplied by another app is only a query, not an authorization. Require
        // the server's attached process mapping to agree with the supplied UID.
        String packageName = mAtm.authenticatedPackageForPid(pid);
        ApkPackage target = packageName == null ? null : mPm.get(packageName);
        if (target == null || target.uid() != uid) return PackageManager.PERMISSION_DENIED;
        ProviderInfo info = ContentProviders.declaredProvider(mPm, uri.getAuthority());
        if (info == null) return PackageManager.PERMISSION_DENIED;
        if (target.packageName.equals(info.packageName)) return PackageManager.PERMISSION_GRANTED;
        return (heldModes(target, uri) & access) == access ? PackageManager.PERMISSION_GRANTED
                : PackageManager.PERMISSION_DENIED;
    }

    private int heldModes(ApkPackage target, Uri uri) {
        int held = 0;
        synchronized (this) {
            for (Grant grant : mGrants) {
                if (grant.target == target && mPm.get(grant.owner.packageName) == grant.owner
                        && matches(grant.uri, grant.prefix, uri)) held |= grant.modes;
            }
        }
        return held;
    }

    int checkFull(Uri original, int pid, int uid, int modes, int userId) {
        if (check(original, pid, uid, modes, userId) == PackageManager.PERMISSION_GRANTED) {
            return PackageManager.PERMISSION_GRANTED;
        }
        Uri uri;
        try { uri = checkedUri(original, userId); }
        catch (SecurityException invalid) { return PackageManager.PERMISSION_DENIED; }
        if ((modes & ACCESS) == 0 || (modes & ~ACCESS) != 0) return PackageManager.PERMISSION_DENIED;
        String name = mAtm.authenticatedPackageForPid(pid);
        ApkPackage target = name == null ? null : mPm.get(name);
        if (target == null || target.uid() != uid) return PackageManager.PERMISSION_DENIED;
        return canAccess(target, uri, modes, userId) ? PackageManager.PERMISSION_GRANTED
                : PackageManager.PERMISSION_DENIED;
    }

    /** Shared URI-level policy for authenticated/stored installed identities. */
    boolean canAccess(ApkPackage target, Uri original, int modes, int userId) {
        Uri uri;
        try { uri = checkedUri(original, userId); }
        catch (SecurityException invalid) { return false; }
        if (target == null || mPm.get(target.packageName) != target
                || (modes & ACCESS) == 0 || (modes & ~ACCESS) != 0) return false;
        if ("settings".equals(uri.getAuthority())) {
            if ((modes & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) == 0) return true;
            String table = uri.getPathSegments().isEmpty() ? "" : uri.getPathSegments().get(0);
            return ("system".equals(table) && granted(target.packageName, "android.permission.WRITE_SETTINGS"))
                    || (("secure".equals(table) || "global".equals(table))
                            && granted(target.packageName, "android.permission.WRITE_SECURE_SETTINGS"));
        }
        ProviderInfo info = ContentProviders.declaredProvider(mPm, uri.getAuthority());
        if (info == null) return false;
        if (target.packageName.equals(info.packageName) || (heldModes(target, uri) & modes) == modes) return true;
        if (!info.exported) return false;
        String name = target.packageName;
        if ((modes & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0
                && !directAllowed(name, info, uri, true)) return false;
        if ((modes & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0
                && !directAllowed(name, info, uri, false)) return false;
        return true;
    }

    private boolean granted(String name, String permission) {
        return mPm.permissions().checkPermission(name, permission) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean directAllowed(String name, ProviderInfo info, Uri uri, boolean read) {
        String base = read ? info.readPermission : info.writePermission;
        if (base != null && granted(name, base)) return true;
        boolean defaultAllowed = base == null;
        if (info.pathPermissions != null) {
            for (android.content.pm.PathPermission path : info.pathPermissions) {
                String permission = read ? path.getReadPermission() : path.getWritePermission();
                if (permission != null && path.match(uri.getPath())) {
                    if (granted(name, permission)) return true;
                    defaultAllowed = false;
                }
            }
        }
        return defaultAllowed;
    }

    synchronized boolean hasAny(ApkPackage target, ProviderInfo info) {
        for (Grant grant : mGrants) {
            if (grant.target == target && mPm.get(grant.owner.packageName) == grant.owner
                    && info.packageName.equals(grant.owner.packageName)
                    && ContentProviders.hasAuthority(info, grant.uri.getAuthority())
                    && grant.modes != 0) return true;
        }
        return false;
    }

    synchronized List<android.content.UriPermission> list(String packageName, boolean incoming,
            boolean persistedOnly) {
        List<android.content.UriPermission> result = new ArrayList<>();
        if (persistedOnly) return result;
        for (Grant grant : mGrants) {
            if (mPm.get(grant.owner.packageName) != grant.owner
                    || mPm.get(grant.target.packageName) != grant.target) continue;
            if (packageName.equals(incoming ? grant.target.packageName : grant.owner.packageName)) {
                result.add(new android.content.UriPermission(grant.uri, grant.modes,
                        android.content.UriPermission.INVALID_TIME));
            }
        }
        return result;
    }

    synchronized void clear(String packageName) {
        mGrants.removeIf(grant -> packageName.equals(grant.owner.packageName)
                || packageName.equals(grant.target.packageName));
    }

    void grant(String targetPackage, Uri original, int modes, int userId) {
        Uri uri = checkedUri(original, userId);
        if ((modes & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) != 0) {
            throw new UnsupportedOperationException("Persistable URI grants are not implemented");
        }
        int access = modes & ACCESS;
        if (access == 0 || (modes & ~(ACCESS | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)) != 0) {
            throw new IllegalArgumentException("Invalid URI grant flags");
        }
        ProviderInfo info = ContentProviders.declaredProvider(mPm, uri.getAuthority());
        ApkPackage owner = info == null ? null : mPm.get(info.packageName);
        ApkPackage target = mPm.get(targetPackage);
        if (owner == null || target == null) throw new SecurityException("Unknown URI grant participant");
        boolean grantable = info.grantUriPermissions;
        if (!grantable && info.uriPermissionPatterns != null) {
            for (android.os.PatternMatcher pattern : info.uriPermissionPatterns) {
                if (pattern.match(uri.getPath())) { grantable = true; break; }
            }
        }
        if (!grantable) throw new SecurityException("Provider does not allow URI grants");
        if (!mCallers.isSystem()) {
            String sender = mCallers.callerPackage();
            boolean own = owner.packageName.equals(sender);
            if (!own && check(uri, mCallers.callerPid(), mCallers.callerUid(), access, 0)
                    != PackageManager.PERMISSION_GRANTED) {
                throw new SecurityException("Caller cannot delegate this URI");
            }
            if (!own && (modes & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0) {
                int prefixAccess = 0;
                synchronized (this) {
                    for (Grant grant : mGrants) {
                        if (grant.prefix && grant.target == mPm.get(sender)
                                && mPm.get(grant.owner.packageName) == grant.owner
                                && matches(grant.uri, true, uri)) prefixAccess |= grant.modes;
                    }
                }
                if ((prefixAccess & access) != access) {
                    throw new SecurityException("An exact URI grant cannot become a prefix grant");
                }
            }
        }
        synchronized (this) {
            mGrants.add(new Grant(owner, target, uri, access,
                    (modes & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0));
        }
    }

    void revoke(String targetPackage, Uri original, int modes, int userId) {
        Uri uri = checkedUri(original, userId);
        ProviderInfo info = ContentProviders.declaredProvider(mPm, uri.getAuthority());
        if (info == null) throw new SecurityException("Unknown URI authority");
        if (!mCallers.isSystem() && !info.packageName.equals(mCallers.callerPackage())) {
            throw new SecurityException("Only the provider owner can revoke delegated access");
        }
        int access = modes & ACCESS;
        synchronized (this) {
            for (Grant grant : mGrants) {
                if ((targetPackage == null || targetPackage.equals(grant.target.packageName))
                        && grant.owner.packageName.equals(info.packageName)
                        && matches(uri, true, grant.uri)) grant.modes &= ~access;
            }
            mGrants.removeIf(grant -> grant.modes == 0);
        }
    }
}
