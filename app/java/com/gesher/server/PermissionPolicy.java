/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.AttributionSourceState;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.PermissionGroupInfo;
import android.content.pm.PermissionInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.permission.IPermissionManager;
import android.permission.PermissionManager;
import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.WeakHashMap;

/** UID permission state shared by package/activity/permission services. */
public final class PermissionPolicy extends IPermissionManager.Default {
    private final PackageManagerService mPm;
    private final PermissionCatalog mCatalog;
    private final File mFile = new File(Device.dataDir(), "system/runtime-permissions");
    private final Properties mDecisions;
    private final Map<IBinder, AttributionSourceState> mAttributions = new WeakHashMap<>();
    private volatile CallerIdentity mCallers;

    PermissionPolicy(PackageManagerService pm) {
        mPm = pm;
        mCatalog = new PermissionCatalog(pm);
        mCallers = new CallerIdentity(pm, null);
        Properties saved = Device.read(mFile);
        mDecisions = saved == null ? new Properties() : saved;
    }

    void attach(CallerIdentity callers) { mCallers = callers; }

    private void requireSystem() {
        if (mCallers == null || !mCallers.isSystem()) {
            throw new SecurityException("Only the Gesher host can change permission grants");
        }
    }

    private static void user(int userId) {
        if (userId != 0) throw new IllegalArgumentException("Only user 0 is available");
    }

    @Override public PermissionInfo getPermissionInfo(String name, String opPackageName, int flags) {
        return mCatalog.get(name);
    }
    @Override public PermissionGroupInfo getPermissionGroupInfo(String name, int flags) {
        PermissionGroupInfo info = mCatalog.groups().get(name);
        return info == null ? null : new PermissionGroupInfo(info);
    }
    @Override public ParceledListSlice getAllPermissionGroups(int flags) {
        return new ParceledListSlice<>(new ArrayList<>(mCatalog.groups().values()));
    }
    @Override public ParceledListSlice queryPermissionsByGroup(String group, int flags) {
        List<PermissionInfo> result = new ArrayList<>();
        for (PermissionInfo info : mCatalog.permissions().values()) {
            if (java.util.Objects.equals(group, info.group)) result.add(new PermissionInfo(info));
        }
        return new ParceledListSlice<>(result);
    }

    boolean granted(String name, String packageName) {
        ApkPackage pkg = mPm.get(packageName);
        if (pkg == null || name == null) return false;
        if (GmsSignatureSpoofing.PERMISSION_FAKE_PACKAGE_SIGNATURE.equals(name)) {
            return pkg.parsed.getRequestedPermissions().contains(name)
                    && GmsSignatureSpoofing.isTrustedPackage(pkg);
        }
        PermissionInfo info = mCatalog.get(name);
        if (info == null) return false;
        List<ApkPackage> members = uidMembers(pkg.uid());
        boolean requested = false;
        int targetSdk = Integer.MAX_VALUE;
        for (ApkPackage member : members) {
            requested |= member.parsed.getRequestedPermissions().contains(name);
            targetSdk = Math.min(targetSdk, member.parsed.getTargetSdkVersion());
        }
        if (!requested) return false;
        int protection = info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE;
        if (protection == PermissionInfo.PROTECTION_NORMAL) return true;
        if (protection == PermissionInfo.PROTECTION_DANGEROUS) {
            String decision = runtimeDecisionForUid(pkg.uid(), name, members);
            // Runtime permissions are UID state. New modern applications do not
            // receive consent merely because an optional compatibility mode is off.
            return decision != null ? "granted".equals(decision) : targetSdk < 23;
        }
        ApkPackage owner = mPm.get(info.packageName);
        for (ApkPackage member : members) {
            if (!member.parsed.getRequestedPermissions().contains(name)) continue;
            if (!"android".equals(info.packageName) && member.packageName.equals(info.packageName)) return true;
            if (owner != null && member.signaturesMatch(owner)) return true;
            if ((info.protectionLevel & PermissionInfo.PROTECTION_FLAG_PRIVILEGED) != 0
                    && member.isSystemAddon() && member.hasVerifiedMicrogSigner()
                    && privilegedAllowlist(member.packageName, name)) return true;
        }
        return false;
    }

    private List<ApkPackage> uidMembers(int uid) {
        List<ApkPackage> members = new ArrayList<>();
        for (ApkPackage pkg : mPm.packages()) if (pkg.uid() == uid) members.add(pkg);
        return members;
    }

    private synchronized String runtimeDecisionForUid(int uid, String permission, List<ApkPackage> members) {
        String decision = mDecisions.getProperty(uidKey(uid, permission));
        if (decision != null) return decision;
        boolean granted = false;
        // Migrate old package records deterministically. Conflicting old decisions
        // must never depend on whichever package happened to be queried first.
        for (ApkPackage member : members) {
            String old = mDecisions.getProperty(legacyKey(member, permission));
            if ("denied".equals(old)) return "denied";
            granted |= "granted".equals(old);
        }
        return granted ? "granted" : null;
    }

    /** Explicit allowlist, never all signature permissions simply because an APK is trusted. */
    private boolean privilegedAllowlist(String pkg, String permission) {
        String root = System.getenv("ANDROID_ROOT");
        File file = new File(root == null ? "/system" : root,
                "etc/permissions/privapp-permissions-microg.xml");
        try (java.io.FileInputStream input = new java.io.FileInputStream(file)) {
            org.xmlpull.v1.XmlPullParser xml = android.util.Xml.newPullParser();
            xml.setInput(input, "UTF-8");
            boolean selected = false;
            for (int event; (event = xml.next()) != org.xmlpull.v1.XmlPullParser.END_DOCUMENT;) {
                if (event == org.xmlpull.v1.XmlPullParser.START_TAG) {
                    if ("privapp-permissions".equals(xml.getName())) {
                        selected = pkg.equals(xml.getAttributeValue(null, "package"));
                    } else if (selected && "permission".equals(xml.getName())
                            && permission.equals(xml.getAttributeValue(null, "name"))) return true;
                } else if (event == org.xmlpull.v1.XmlPullParser.END_TAG
                        && "privapp-permissions".equals(xml.getName())) selected = false;
            }
        } catch (java.io.FileNotFoundException ignored) {
            // No allowlist, no privileged permissions.
        } catch (Exception e) { SystemServer.log("permissions: invalid privileged allowlist: " + e); }
        return false;
    }

    private String legacyKey(ApkPackage pkg, String permission) {
        String signer = pkg.verifiedSignerSha256();
        // Grants never survive replacement by a different signing identity.
        return pkg.packageName + ":" + (signer == null ? "unsigned" : signer) + ":" + permission;
    }

    private static String uidKey(int uid, String permission) {
        return "uid:" + uid + ":" + permission;
    }

    /** Uninstall must revoke decisions even if a later install reuses a package UID. */
    public void forgetPackage(String packageName) {
        synchronized (this) {
            Properties before = new Properties();
            before.putAll(mDecisions);
            ApkPackage removed = mPm.get(packageName);
            List<ApkPackage> remaining = removed == null ? List.of() : uidMembers(removed.uid());
            String prefix = removed == null ? null : "uid:" + removed.uid() + ":";
            mDecisions.keySet().removeIf(key -> {
                String saved = key.toString();
                if (saved.startsWith(packageName + ":")) return true;
                if (prefix == null || !saved.startsWith(prefix)) return false;
                String permission = saved.substring(prefix.length());
                for (ApkPackage member : remaining) {
                    if (!member.packageName.equals(packageName)
                            && member.parsed.getRequestedPermissions().contains(permission)) return false;
                }
                return true;
            });
            try { Device.write(mFile, mDecisions); }
            catch (java.io.IOException e) {
                mDecisions.clear(); mDecisions.putAll(before);
                throw new IllegalStateException("Cannot clear permission decisions", e);
            }
        }
        invalidateCaches();
        if (NotificationManagerService.sLast != null) NotificationManagerService.sLast.permissionChanged(packageName);
    }

    @Override public int checkPermission(String pkg, String permission, String deviceId, int userId) {
        return userId == 0 && granted(permission, pkg)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }
    @Override public int checkUidPermission(int uid, String permission, int deviceId) {
        if (uid == Process.SYSTEM_UID || uid == Process.ROOT_UID || uid == Process.SHELL_UID) {
            return PackageManager.PERMISSION_GRANTED;
        }
        String[] packages = mPm.getPackagesForUid(uid);
        if (packages != null) for (String pkg : packages) if (granted(permission, pkg)) return PackageManager.PERMISSION_GRANTED;
        return PackageManager.PERMISSION_DENIED;
    }

    int checkProcessPermission(String permission, int pid, int uid) {
        CallerIdentity callers = mCallers;
        if (callers == null) return PackageManager.PERMISSION_DENIED;
        // A package's effective UID comes from its authenticated registered PID.
        String pkg = callers.packageForPid(pid);
        if (pkg != null) {
            ApkPackage installed = mPm.get(pkg);
            return installed != null && installed.uid() == uid && granted(permission, pkg)
                    ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
        }
        if (pid == Process.myPid() && (uid == Process.SYSTEM_UID || uid == Process.myUid())) {
            return PackageManager.PERMISSION_GRANTED;
        }
        return PackageManager.PERMISSION_DENIED;
    }

    @Override public int getPermissionRequestState(String pkg, String permission, int deviceId) {
        if (granted(permission, pkg)) return Context.PERMISSION_REQUEST_STATE_GRANTED;
        return canRequest(pkg, permission) ? Context.PERMISSION_REQUEST_STATE_REQUESTABLE
                : Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE;
    }

    public boolean canRequest(String pkg, String permission) {
        ApkPackage installed = mPm.get(pkg);
        PermissionInfo info = mCatalog.get(permission);
        return installed != null && installed.parsed.getRequestedPermissions().contains(permission)
                && info != null && (info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE)
                        == PermissionInfo.PROTECTION_DANGEROUS;
    }

    public boolean isProtectedBroadcast(String action) { return mCatalog.isProtectedBroadcast(action); }

    boolean isKnownProtectedBroadcast(String action) { return mCatalog.isKnownProtectedBroadcast(action); }

    public int checkPermission(String pkg, String permission) {
        return checkPermission(pkg, permission, "default:0", 0);
    }

    public void grantRuntime(String pkg, String permission, int userId) {
        grantRuntimePermission(pkg, permission, "default:0", userId);
    }

    private void runtimeDecision(String pkg, String permission, int userId, String decision) {
        requireSystem(); user(userId);
        ApkPackage installed = mPm.get(pkg);
        PermissionInfo info = mCatalog.get(permission);
        if (installed == null || !installed.parsed.getRequestedPermissions().contains(permission)
                || info == null || (info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE)
                        != PermissionInfo.PROTECTION_DANGEROUS) {
            throw new IllegalArgumentException("Not a requested runtime permission: " + permission);
        }
        final boolean wasGranted;
        synchronized (this) {
            if (mPm.get(pkg) != installed || mPm.isRemovalPending(pkg)) {
                throw new SecurityException("Runtime permission owner changed or is retiring");
            }
            wasGranted = granted(permission, pkg);
            Properties before = new Properties();
            before.putAll(mDecisions);
            List<ApkPackage> members = uidMembers(installed.uid());
            mDecisions.setProperty(uidKey(installed.uid(), permission), decision);
            for (ApkPackage member : members) mDecisions.remove(legacyKey(member, permission));
            try { Device.write(mFile, mDecisions); }
            catch (java.io.IOException e) {
                mDecisions.clear(); mDecisions.putAll(before);
                throw new IllegalStateException("Cannot persist permission decision", e);
            }
        }
        invalidateCaches();
        NotificationManagerService notifications = NotificationManagerService.sLast;
        if (notifications != null) for (ApkPackage member : uidMembers(installed.uid())) {
            notifications.permissionChanged(member.packageName);
        }
        Services.Created services = Services.sLast;
        if (wasGranted && "denied".equals(decision) && services != null && services.pm == mPm) {
            services.atm.killUidForPermissionChange(installed, () -> !granted(permission, pkg));
        }
    }

    private void invalidateCaches() {
        // These caches share the platform package-info nonce. Invalidation reaches
        // existing client processes through ApplicationSharedMemory, not just this JVM.
        if (Services.sLast != null) Services.sLast.am.invalidatePermissionCaches();
        android.permission.PermissionManager.disablePermissionCache();
        android.permission.PermissionManager.disablePackageNamePermissionCache();
    }
    @Override public void grantRuntimePermission(String pkg, String permission, String deviceId, int userId) {
        runtimeDecision(pkg, permission, userId, "granted");
    }
    @Override public void revokeRuntimePermission(String pkg, String permission, String deviceId, int userId, String reason) {
        runtimeDecision(pkg, permission, userId, "denied");
    }
    @Override public boolean shouldShowRequestPermissionRationale(String pkg, String permission, int deviceId, int userId) { return false; }
    @Override public int getPermissionFlags(String pkg, String permission, String deviceId, int userId) {
        ApkPackage installed = mPm.get(pkg);
        if (installed == null) return 0;
        synchronized (this) {
            return runtimeDecisionForUid(installed.uid(), permission, uidMembers(installed.uid())) != null
                    ? PackageManager.FLAG_PERMISSION_USER_SET : 0;
        }
    }
    @Override public Map<String, PermissionManager.PermissionState> getAllPermissionStates(String pkg, String deviceId, int userId) {
        Map<String, PermissionManager.PermissionState> result = new LinkedHashMap<>();
        ApkPackage installed = mPm.get(pkg);
        if (installed != null && userId == 0) for (String permission : installed.parsed.getRequestedPermissions()) {
            result.put(permission, new PermissionManager.PermissionState(granted(permission, pkg),
                    getPermissionFlags(pkg, permission, deviceId, userId)));
        }
        return result;
    }
    @Override public List<String> getAllowlistedRestrictedPermissions(String pkg, int flags, int userId) { return Collections.emptyList(); }
    @Override public List<android.content.pm.permission.SplitPermissionInfoParcelable> getSplitPermissions() { return Collections.emptyList(); }

    @Override public IBinder registerAttributionSource(AttributionSourceState source) {
        CallerIdentity callers = mCallers;
        if (source == null || callers == null) throw new SecurityException("Unregistered attribution caller");
        callers.requirePackage(source.packageName);
        ApkPackage pkg = mPm.get(source.packageName);
        if (pkg == null || pkg.uid() != source.uid
                || (source.pid != -1 && source.pid != callers.callerPid())) throw new SecurityException("Forged attribution identity");
        IBinder token = source.token == null ? new Binder() : source.token;
        AttributionSourceState saved = new AttributionSourceState();
        saved.uid = source.uid; saved.pid = callers.callerPid(); saved.packageName = source.packageName;
        synchronized (mAttributions) { mAttributions.put(token, saved); }
        return token;
    }
    @Override public boolean isRegisteredAttributionSource(AttributionSourceState source) {
        if (source == null || source.token == null) return false;
        synchronized (mAttributions) {
            AttributionSourceState saved = mAttributions.get(source.token);
            return saved != null && saved.uid == source.uid && java.util.Objects.equals(saved.packageName, source.packageName)
                    && (source.pid == -1 || source.pid == saved.pid);
        }
    }
    @Override public int getRegisteredAttributionSourceCount(int uid) {
        synchronized (mAttributions) { return (int)mAttributions.values().stream().filter(s -> s.uid == uid).count(); }
    }

    int checkAttribution(String permission, AttributionSourceState source, boolean fromDatasource) {
        CallerIdentity callers = mCallers;
        if (callers == null || source == null) return android.content.PermissionChecker.PERMISSION_HARD_DENIED;
        String caller = callers.callerPackage();
        if (!callers.isSystem() && !(java.util.Objects.equals(caller, source.packageName)
                && callers.callerUid() == source.uid) && !isRegisteredAttributionSource(source)) {
            return android.content.PermissionChecker.PERMISSION_HARD_DENIED;
        }
        AttributionSourceState current = source;
        for (int depth = 0; current != null; depth++) {
            if (depth > 16) return android.content.PermissionChecker.PERMISSION_HARD_DENIED;
            if (!(depth == 0 && fromDatasource)) {
                ApkPackage pkg = mPm.get(current.packageName);
                if (pkg == null || pkg.uid() != current.uid || (permission != null && !granted(permission, current.packageName))) {
                    return android.content.PermissionChecker.PERMISSION_HARD_DENIED;
                }
            }
            if (current.renouncedPermissions != null && java.util.Arrays.asList(current.renouncedPermissions).contains(permission)) {
                return android.content.PermissionChecker.PERMISSION_HARD_DENIED;
            }
            current = current.next == null || current.next.length == 0 ? null : current.next[0];
        }
        return android.content.PermissionChecker.PERMISSION_GRANTED;
    }

    PackageInfo annotate(PackageInfo info) {
        if (info != null && info.requestedPermissions != null) {
            if (info.requestedPermissionsFlags == null) info.requestedPermissionsFlags = new int[info.requestedPermissions.length];
            for (int i=0;i<info.requestedPermissions.length;i++) {
                if (granted(info.requestedPermissions[i], info.packageName)) info.requestedPermissionsFlags[i] |= PackageInfo.REQUESTED_PERMISSION_GRANTED;
                else info.requestedPermissionsFlags[i] &= ~PackageInfo.REQUESTED_PERMISSION_GRANTED;
            }
        }
        return info;
    }
}
