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
        // Android's GrantUri removes an embedded user ID, not the URI's query or
        // fragment. Those remain part of an exact grant's identity; prefix grants
        // and revocation deliberately compare path segments instead.
        return uri.buildUpon().authority(authority).build();
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
        return holdsGrant(target, uri, access, false) ? PackageManager.PERMISSION_GRANTED
                : PackageManager.PERMISSION_DENIED;
    }

    private boolean current(ApkPackage pkg) {
        return pkg != null && mPm.get(pkg.packageName) == pkg
                && !mPm.isRemovalPending(pkg.packageName);
    }

    private boolean holdsGrant(ApkPackage target, Uri uri, int modes, boolean requirePrefix) {
        synchronized (this) {
            if (!current(target)) return false;
            for (Grant grant : mGrants) {
                if (grant.target == target && current(grant.owner)
                        && (!requirePrefix || grant.prefix)
                        && matches(grant.uri, grant.prefix, uri)
                        && (grant.modes & modes) == modes) return true;
            }
        }
        return false;
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
        if (!current(target)
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
        if (holdsGrant(target, uri, modes, false)) return true;
        return hasDirectAccess(target, info, uri, modes);
    }

    /** General provider/path access is distinct from an explicitly held URI grant. */
    private boolean hasDirectAccess(ApkPackage target, ProviderInfo info, Uri uri, int modes) {
        if (!current(target) || modes == 0) return false;
        // The legacy runtime may give unrelated packages the same numeric UID.
        // Its authenticated package identity must not become provider ownership.
        if (target.packageName.equals(info.packageName)) return true;
        if (!info.exported) return false;
        // Such providers require a dynamic provider-side decision. No general
        // permission can substitute for that missing backend; explicit grants
        // and the actual provider owner are still handled independently above.
        if (info.forceUriPermissions) return false;
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
        if (!current(target)) return false;
        for (Grant grant : mGrants) {
            if (grant.target == target && current(grant.owner)
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
            if (!current(grant.owner) || !current(grant.target)) continue;
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
        boolean system = mCallers.isSystem();
        String senderName = system ? null : mCallers.callerPackage();
        if (targetPackage == null) throw new IllegalArgumentException("Missing URI grant target");
        boolean prefix = (modes & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0;
        // Provider metadata and installed generations must come from one snapshot.
        // Package removal takes this same monitor before retiring capabilities.
        synchronized (mPm) {
            ApkPackage sender = system ? null : mPm.get(senderName);
            if (!system && !current(sender)) {
                throw new SecurityException("Retiring URI grant sender");
            }
            ProviderInfo info = ContentProviders.declaredProvider(mPm, uri.getAuthority());
            // Android makes no grant when either endpoint no longer exists.
            // Caller and argument validation has already run; existing state is
            // untouched and no missing provider/package is fabricated.
            if (info == null) return;
            ApkPackage owner = mPm.get(info.packageName);
            ApkPackage target = mPm.get(targetPackage);
            if (target == null) return;
            if (!current(owner) || !current(target)) {
                throw new SecurityException("Unknown or retiring URI grant participant");
            }
            // No capability is needed or created when the target already has
            // direct access. Android does this before checking grant patterns,
            // but advanced grants and dynamic providers must still be checked.
            if (!prefix && !info.forceUriPermissions
                    && hasDirectAccess(target, info, uri, access)) return;
            boolean grantable = info.grantUriPermissions;
            if (info.uriPermissionPatterns != null && info.uriPermissionPatterns.length != 0) {
                grantable = false;
                for (android.os.PatternMatcher pattern : info.uriPermissionPatterns) {
                    if (pattern != null && pattern.match(uri.getPath())) { grantable = true; break; }
                }
            }
            if (!grantable) throw new SecurityException("Provider does not allow URI grants");
            boolean direct = system || hasDirectAccess(sender, info, uri, access);
            synchronized (this) {
                // An exact grant cannot be promoted into a subtree grant. Keep
                // the held-grant check and publication atomic with revocation.
                if (!direct && !holdsGrant(sender, uri, access, prefix)) {
                    throw new SecurityException("Caller cannot delegate this URI with these modes");
                }
                // Modes accumulate only within one GrantUri identity. Combining
                // unrelated exact/prefix entries would amplify delegated access.
                for (Grant grant : mGrants) {
                    if (grant.owner == owner && grant.target == target
                            && grant.prefix == prefix && grant.uri.equals(uri)) {
                        grant.modes |= access;
                        return;
                    }
                }
                mGrants.add(new Grant(owner, target, uri, access, prefix));
            }
        }
    }

    void revoke(String targetPackage, Uri original, int modes, int userId) {
        Uri uri = checkedUri(original, userId);
        int access = modes & ACCESS;
        boolean system = mCallers.isSystem();
        String callerName = system ? null : mCallers.callerPackage();
        synchronized (mPm) {
            ProviderInfo info = ContentProviders.declaredProvider(mPm, uri.getAuthority());
            if (info == null) return;
            ApkPackage owner = mPm.get(info.packageName);
            ApkPackage caller = system ? null : mPm.get(callerName);
            boolean all = system || hasDirectAccess(caller, info, uri, access);
            synchronized (this) {
                for (Grant grant : mGrants) {
                    // Since Android L, recipients without general provider access
                    // may relinquish their own ownerless grants, never another
                    // recipient's. All grants in this backend are explicit.
                    if ((all || grant.target == caller)
                            && (targetPackage == null || targetPackage.equals(grant.target.packageName))
                            && grant.owner == owner && matches(uri, true, grant.uri)) {
                        grant.modes &= ~access;
                    }
                }
                mGrants.removeIf(grant -> grant.modes == 0);
            }
        }
    }
}
