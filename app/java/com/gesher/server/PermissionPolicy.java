/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
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
import android.util.AtomicFile;
import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.WeakHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import static android.app.AppOpsManager.*;

/** UID permission state shared by package/activity/permission services. */
public final class PermissionPolicy extends IPermissionManager.Default {
    private final PackageManagerService mPm;
    private final PermissionCatalog mCatalog;
    private final File mFile = new File(Device.dataDir(), "system/runtime-permissions");
    private final Properties mDecisions;
    private final AtomicFile mPermissionFile;
    private final PolicyStateWriter<Properties> mWriter;
    interface RuntimeModeListener {
        void changed(int uid, Map<Integer, Integer> before, Map<Integer, Integer> after, Object publicationLock);
    }
    private final CopyOnWriteArrayList<RuntimeModeListener> mRuntimeModeListeners = new CopyOnWriteArrayList<>();
    // An uninstall revokes permission ownership before committing the package registry.
    // Keep that intermediate view tied to the exact APK generation, without retaining it
    // after removal. A replacement APK must never inherit this retirement marker.
    private final Map<ApkPackage, Boolean> mForgottenPackages = new WeakHashMap<>();
    // Keyed by PM's immutable published map, never by a name, UID or timestamp.
    // Retirement remains a separate live filter: an uninstall may be in progress
    // before the package map is replaced, and a failed removal may restore it.
    private Map<String, ApkPackage> mIndexedPackages;
    private Map<Integer, List<ApkPackage>> mUidMembers = Map.of();
    private record AttributionOwner(String name, int uid, ApkPackage code) { }
    private record RegisteredAttribution(AttributionSourceState source, List<AttributionOwner> owners) { }
    private final Map<IBinder, RegisteredAttribution> mAttributions = new WeakHashMap<>();
    private volatile CallerIdentity mCallers;

    PermissionPolicy(PackageManagerService pm) {
        mPm = pm;
        mCatalog = new PermissionCatalog(pm);
        mCallers = new CallerIdentity(pm, null);
        mPermissionFile = new AtomicFile(mFile);
        mDecisions = new Properties();
        try (var input = mPermissionFile.openRead()) {
            mDecisions.load(input);
            for (String key : mDecisions.stringPropertyNames()) {
                if (key.startsWith("flags:")) Integer.parseInt(mDecisions.getProperty(key));
                if (key.startsWith("legacy-grant:") && !List.of("true", "false").contains(mDecisions.getProperty(key))) {
                    throw new IllegalArgumentException("Invalid legacy permission grant");
                }
            }
        }
        catch (java.io.FileNotFoundException absent) { }
        catch (java.io.IOException | IllegalArgumentException failure) {
            throw new IllegalStateException("Cannot read permission decisions", failure);
        }
        mWriter = new PolicyStateWriter<>("Permissions", this::writeDecisions);
    }

    void attach(CallerIdentity callers) { mCallers = callers; }
    void addRuntimeModeListener(RuntimeModeListener listener) { mRuntimeModeListeners.add(listener); }

    private void writeDecisions(Properties snapshot) throws java.io.IOException {
        java.io.FileOutputStream output = null;
        try {
            output = mPermissionFile.startWrite();
            snapshot.store(output, "Gesher UID permission state");
            mPermissionFile.finishWrite(output);
        } catch (java.io.IOException | RuntimeException failure) {
            if (output != null) mPermissionFile.failWrite(output);
            throw failure;
        }
    }
    private void scheduleWrite() {
        Properties snapshot = new Properties();
        snapshot.putAll(mDecisions);
        // PermissionFlags.ONE_TIME and its user decisions are session state. Persisting the
        // runtime grant here would turn a temporary grant into a grant after policy restart.
        for (String key : snapshot.stringPropertyNames()) if (key.startsWith("flags:")) {
            int flags = Integer.parseInt(snapshot.getProperty(key));
            if ((flags & PackageManager.FLAG_PERMISSION_ONE_TIME) == 0) continue;
            int persistent = flags & ~(PackageManager.FLAG_PERMISSION_ONE_TIME
                    | PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED
                    | PackageManager.FLAG_PERMISSION_REVOKED_COMPAT);
            snapshot.setProperty(key, Integer.toString(persistent));
            snapshot.setProperty("uid:" + key.substring("flags:".length()), "denied");
        }
        mWriter.schedule(snapshot);
    }

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

    synchronized boolean granted(String name, String packageName) {
        ApkPackage pkg = mPm.get(packageName);
        if (pkg == null || name == null) return false;
        if (GmsSignatureSpoofing.PERMISSION_FAKE_PACKAGE_SIGNATURE.equals(name)) {
            return pkg.parsed.getRequestedPermissions().contains(name)
                    && GmsSignatureSpoofing.isTrustedPackage(pkg);
        }
        List<ApkPackage> members = uidMembers(pkg.uid());
        PermissionInfo info = mCatalog.get(name);
        // A normal grant depends on the installed UID's manifest requests, not
        // runtime decisions or flags. Flag queries still take the full path below.
        if (info != null && (info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE)
                == PermissionInfo.PROTECTION_NORMAL) {
            return requestingTargetSdk(name, members) != Integer.MAX_VALUE;
        }
        if (permissionState(pkg.uid(), name, members, info).granted) return true;
        String fuller = fullerPermission(name);
        return fuller != null && permissionState(pkg.uid(), fuller, members).granted;
    }

    /** Android 16 PermissionFlags distinguishes the permission grant from its AppOps grant. */
    private record PermissionState(boolean granted, int flags, boolean restricted) {
        boolean appOpGranted() {
            return granted && !restricted && (flags & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) == 0;
        }
    }

    private synchronized PermissionState permissionState(int uid, String name, List<ApkPackage> members) {
        return permissionState(uid, name, members, mCatalog.get(name));
    }

    private PermissionState permissionState(int uid, String name, List<ApkPackage> members,
            PermissionInfo info) {
        if (info == null) return new PermissionState(false, 0, false);
        int targetSdk = requestingTargetSdk(name, members);
        if (targetSdk == Integer.MAX_VALUE) return new PermissionState(false, 0, false);
        String decision = runtimeDecisionForUid(uid, name, members);
        boolean legacy = storedLegacyGrant(uid, name, info, targetSdk);
        int flags = permissionFlags(uid, name, info, legacy, decision);
        boolean restricted = (flags & PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION) != 0
                && info.isRestricted();
        int protection = info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE;
        if (protection == PermissionInfo.PROTECTION_NORMAL) return new PermissionState(true, flags, restricted);
        if (protection == PermissionInfo.PROTECTION_DANGEROUS) {
            // LEGACY_GRANTED stays a permission grant. Its revocation is represented by
            // REVOKED_COMPAT, so PermissionChecker can distinguish hard and soft denial.
            boolean granted = legacy || "granted".equals(decision)
                    && !(restricted && info.isHardRestricted());
            return new PermissionState(granted, flags, restricted);
        }
        if ((info.protectionLevel & (PermissionInfo.PROTECTION_FLAG_ROLE
                | PermissionInfo.PROTECTION_FLAG_DEVELOPMENT)) != 0 && "granted".equals(decision)) {
            return new PermissionState(true, flags, restricted);
        }
        ApkPackage owner = mPm.get(info.packageName);
        for (ApkPackage member : members) {
            if (!member.parsed.getRequestedPermissions().contains(name)) continue;
            if (!"android".equals(info.packageName) && member.packageName.equals(info.packageName)) return new PermissionState(true, flags, restricted);
            if (owner != null && member.signaturesMatch(owner)) return new PermissionState(true, flags, restricted);
            if ((info.protectionLevel & PermissionInfo.PROTECTION_FLAG_PRIVILEGED) != 0
                    && member.isSystemAddon() && member.hasVerifiedMicrogSigner()
                    && privilegedAllowlist(member.packageName, name)) return new PermissionState(true, flags, restricted);
        }
        return new PermissionState(false, flags, restricted);
    }

    /** AOSP evaluates legacy eligibility from requestingPackageStates for each permission. */
    private static int requestingTargetSdk(String permission, List<ApkPackage> members) {
        int targetSdk = Integer.MAX_VALUE;
        for (ApkPackage member : members) {
            if (member.parsed.getRequestedPermissions().contains(permission)) {
                targetSdk = Math.min(targetSdk, member.parsed.getTargetSdkVersion());
            }
        }
        return targetSdk;
    }

    private static boolean legacyGranted(PermissionInfo info, int targetSdk) {
        // Runtime-only protection excludes automatic compatibility grants; an explicit
        // grant accepted through a modern requester remains independent UID state.
        return info.isRuntime() && targetSdk < 23
                && (info.protectionLevel & PermissionInfo.PROTECTION_FLAG_RUNTIME_ONLY) == 0;
    }

    private boolean storedLegacyGrant(int uid, String permission, PermissionInfo info, int targetSdk) {
        if (!info.isRuntime() || (info.protectionLevel & PermissionInfo.PROTECTION_FLAG_RUNTIME_ONLY) != 0) return false;
        String saved = mDecisions.getProperty(legacyGrantKey(uid, permission));
        return saved == null ? legacyGranted(info, targetSdk) : Boolean.parseBoolean(saved);
    }

    private static int implicitPermissionFlags(int flags, boolean legacyGranted) {
        // Both API flags represent IMPLICIT; only LEGACY_GRANTED selects REVIEW_REQUIRED.
        int implicit = PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED | PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED;
        return (flags & implicit) == 0 ? flags : (flags & ~implicit) | (legacyGranted
                ? PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED : PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED);
    }

    private static String fullerPermission(String permission) {
        if (Manifest.permission.ACCESS_COARSE_LOCATION.equals(permission)) return Manifest.permission.ACCESS_FINE_LOCATION;
        if (Manifest.permission.INTERACT_ACROSS_USERS.equals(permission)) return Manifest.permission.INTERACT_ACROSS_USERS_FULL;
        return null;
    }

    private int permissionFlags(int uid, String permission, PermissionInfo info, boolean legacy, String decision) {
        String saved = mDecisions.getProperty(flagsKey(uid, permission));
        if (saved != null) return implicitPermissionFlags(Integer.parseInt(saved), legacy);
        int flags = decision == null ? 0 : PackageManager.FLAG_PERMISSION_USER_SET;
        if (info.isRestricted()) flags |= PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
        if (legacy) {
            flags |= PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
            if ("denied".equals(decision)) flags |= PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
        }
        return flags;
    }

    String runtimePermissionForOp(int op) {
        String permission = AppOpsManager.opToPermission(op);
        if (permission == null || AppOpsManager.permissionToOpCode(permission) != op) return null;
        PermissionInfo info = mCatalog.get(permission);
        return info != null && info.isRuntime() ? permission : null;
    }

    /** Null means this operation has independently stored AppOps policy. */
    synchronized Integer runtimeOpMode(int op, int uid) {
        String permission = runtimePermissionForOp(op);
        return permission == null ? null : permissionMode(uid, permission, uidMembers(uid));
    }

    private int permissionMode(int uid, String permission, List<ApkPackage> members) {
        PermissionInfo info = mCatalog.get(permission);
        if (info != null && permissionState(uid, permission, members).appOpGranted()) {
            return info.backgroundPermission == null
                    || permissionState(uid, info.backgroundPermission, members).appOpGranted()
                    ? MODE_ALLOWED : MODE_FOREGROUND;
        }
        String fuller = fullerPermission(permission);
        return fuller == null ? MODE_IGNORED : permissionMode(uid, fuller, members);
    }

    private Map<Integer, Integer> runtimeModes(int uid) {
        Map<Integer, Integer> modes = new LinkedHashMap<>();
        for (int op = 0; op < _NUM_OP; op++) if (AppOpsManager.opToSwitch(op) == op) {
            Integer mode = runtimeOpMode(op, uid);
            if (mode != null) modes.put(op, mode);
        }
        return Map.copyOf(modes);
    }

    private void publishRuntimeModes(int uid, Map<Integer, Integer> before) {
        publishRuntimeModes(uid, before, null);
    }

    private void publishRuntimeModes(int uid, Map<Integer, Integer> before, Object publicationLock) {
        Map<Integer, Integer> after = runtimeModes(uid);
        if (!before.equals(after)) for (RuntimeModeListener listener : mRuntimeModeListeners) {
            // Internal listeners enqueue work only; no Binder callback or AppOps monitor here.
            listener.changed(uid, before, after, publicationLock);
        }
    }

    synchronized void invalidatePackageIndex() {
        mIndexedPackages = null;
        mUidMembers = Map.of();
        mCatalog.invalidateDeclarations();
    }

    private synchronized List<ApkPackage> uidMembers(int uid) {
        Map<String, ApkPackage> packages = mPm.packageSnapshot();
        if (mIndexedPackages != packages) {
            Map<Integer, List<ApkPackage>> indexed = new LinkedHashMap<>();
            for (ApkPackage pkg : packages.values()) {
                indexed.computeIfAbsent(pkg.uid(), unused -> new ArrayList<>()).add(pkg);
            }
            indexed.replaceAll((key, value) -> List.copyOf(value));
            mUidMembers = indexed;
            mIndexedPackages = packages;
        }
        List<ApkPackage> indexed = mUidMembers.getOrDefault(uid, List.of());
        if (mForgottenPackages.isEmpty()) return indexed;
        List<ApkPackage> members = new ArrayList<>();
        for (ApkPackage pkg : indexed) {
            if (!mForgottenPackages.containsKey(pkg)) members.add(pkg);
        }
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
    private static String flagsKey(int uid, String permission) { return "flags:" + uid + ":" + permission; }
    private static String legacyGrantKey(int uid, String permission) { return "legacy-grant:" + uid + ":" + permission; }

    private record RuntimeState(PermissionState access, boolean legacy, boolean explicitGrant) {
        boolean hasState() { return access.flags != 0 || legacy || explicitGrant; }
    }
    private record UidSnapshot(int uid, Map<String, RuntimeState> permissions, Map<Integer, Integer> modes) { }

    private UidSnapshot snapshotUid(int uid) {
        List<ApkPackage> members = uidMembers(uid);
        Map<String, RuntimeState> states = new LinkedHashMap<>();
        for (PermissionInfo info : mCatalog.permissions().values()) {
            int target = requestingTargetSdk(info.name, members);
            if (!info.isRuntime() || target == Integer.MAX_VALUE) continue;
            states.put(info.name, new RuntimeState(permissionState(uid, info.name, members, info),
                    storedLegacyGrant(uid, info.name, info, target),
                    "granted".equals(runtimeDecisionForUid(uid, info.name, members))));
        }
        return new UidSnapshot(uid, states, runtimeModes(uid));
    }

    private void storeRuntimeState(int uid, String permission, boolean legacy, boolean granted, int flags) {
        mDecisions.setProperty(legacyGrantKey(uid, permission), Boolean.toString(legacy));
        mDecisions.setProperty(uidKey(uid, permission), granted ? "granted" : "denied");
        mDecisions.setProperty(flagsKey(uid, permission), Integer.toString(implicitPermissionFlags(flags, legacy)));
    }

    private void retainRuntimeStates(UidSnapshot snapshot) {
        for (var entry : snapshot.permissions.entrySet()) {
            RuntimeState state = entry.getValue();
            storeRuntimeState(snapshot.uid, entry.getKey(), state.legacy, state.explicitGrant, state.access.flags);
        }
    }

    private void trimUidState(int uid, List<ApkPackage> members) {
        String decisions = "uid:" + uid + ":", flags = "flags:" + uid + ":", legacy = "legacy-grant:" + uid + ":";
        mDecisions.keySet().removeIf(key -> {
            String saved = key.toString();
            String permission = saved.startsWith(decisions) ? saved.substring(decisions.length())
                    : saved.startsWith(flags) ? saved.substring(flags.length())
                    : saved.startsWith(legacy) ? saved.substring(legacy.length()) : null;
            return permission != null && requestingTargetSdk(permission, members) == Integer.MAX_VALUE;
        });
    }

    /** Reconcile the runtime grant flags, once, when the requesting APK is published. */
    private void reconcileRuntimeState(int uid, PermissionInfo info, RuntimeState old, List<ApkPackage> members) {
        if (info == null || !info.isRuntime()) return;
        String permission = info.name;
        int target = requestingTargetSdk(permission, members);
        if (target == Integer.MAX_VALUE) return;
        if (old == null) old = new RuntimeState(new PermissionState(false, 0, false), false, false);
        int implicit = PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED | PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED;
        int flags = old.access.flags;
        boolean legacy = legacyGranted(info, target), granted = old.explicitGrant;
        if (!old.hasState() && info.isRestricted()) flags |= PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
        if (target < 23 && !legacy) {
            // Runtime-only permissions keep restriction exemptions, never a compatibility
            // grant or old user/runtime grant bits, during legacy package evaluation.
            int exempt = PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT
                    | PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT
                    | PackageManager.FLAG_PERMISSION_RESTRICTION_UPGRADE_EXEMPT;
            flags &= exempt;
            if (info.isRestricted() && flags == 0) flags |= PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
            granted = false;
        } else if (legacy) {
            if (old.hasState() && !old.access.granted) flags |= PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
            if (!old.hasState()) flags |= PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
        } else {
            boolean wasImplicit = (flags & implicit) != 0;
            if (old.legacy && !wasImplicit) granted = true;
            if (old.legacy && (flags & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) != 0) {
                granted = false;
                flags &= ~PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
            }
            boolean remainsImplicit = members.stream().anyMatch(member -> member.parsed.getImplicitPermissions().contains(permission));
            if (wasImplicit && !remainsImplicit) {
                flags &= ~implicit;
                if ((flags & (PackageManager.FLAG_PERMISSION_SYSTEM_FIXED | PackageManager.FLAG_PERMISSION_POLICY_FIXED)) == 0) {
                    granted = false;
                    flags &= ~(PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_USER_FIXED);
                }
            }
        }
        storeRuntimeState(uid, permission, legacy, granted, flags);
    }

    /** Boot admission re-evaluates saved flags against the installed requesting packages. */
    synchronized void initializePackages() {
        Properties saved = new Properties(); saved.putAll(mDecisions);
        // Definitions do not change while the initial admitted set is reconciled.
        // Rebuilding the entire catalogue for each UID multiplies package scans.
        Map<String, PermissionInfo> permissions = mCatalog.permissions();
        java.util.Set<Integer> seen = new java.util.HashSet<>();
        for (ApkPackage pkg : mPm.packages()) {
            if (!seen.add(pkg.uid())) continue;
            List<ApkPackage> members = uidMembers(pkg.uid());
            trimUidState(pkg.uid(), members);
            java.util.Set<String> requested = new java.util.LinkedHashSet<>();
            for (ApkPackage member : members) requested.addAll(member.parsed.getRequestedPermissions());
            for (String permission : requested) {
                PermissionInfo info = permissions.get(permission);
                if (info == null || !info.isRuntime()
                        || requestingTargetSdk(info.name, members) == Integer.MAX_VALUE) continue;
                String decision = runtimeDecisionForUid(pkg.uid(), info.name, members);
                String storedFlags = mDecisions.getProperty(flagsKey(pkg.uid(), info.name));
                String storedLegacy = mDecisions.getProperty(legacyGrantKey(pkg.uid(), info.name));
                RuntimeState old = null;
                if (decision != null || storedFlags != null || storedLegacy != null) {
                    int flags = storedFlags == null ? (decision == null ? 0 : PackageManager.FLAG_PERMISSION_USER_SET)
                            : Integer.parseInt(storedFlags);
                    // The old format exposed legacy review through the API flag. New snapshots
                    // retain the grant basis even after that review or compat flag is cleared.
                    boolean legacy = storedLegacy == null ? (flags & PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED) != 0
                            : Boolean.parseBoolean(storedLegacy);
                    boolean granted = "granted".equals(decision);
                    boolean restricted = info.isRestricted() && (flags & PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION) != 0;
                    old = new RuntimeState(new PermissionState(legacy || granted && !(restricted && info.isHardRestricted()),
                            flags, restricted), legacy, granted);
                }
                reconcileRuntimeState(pkg.uid(), info, old, members);
            }
        }
        if (!saved.equals(mDecisions)) scheduleWrite();
    }

    /** PM holds its monitor. Permission readers cannot observe metadata with old flags. */
    synchronized Runnable publishPackage(ApkPackage pkg, Runnable publish, Object publicationLock) {
        java.util.Set<Integer> uids = new java.util.LinkedHashSet<>();
        uids.add(pkg.uid());
        ApkPackage previous = mPm.get(pkg.packageName);
        if (previous != null) uids.add(previous.uid());
        List<UidSnapshot> before = new ArrayList<>();
        Properties saved = new Properties(); saved.putAll(mDecisions);
        for (int uid : uids) {
            UidSnapshot snapshot = snapshotUid(uid);
            before.add(snapshot);
            retainRuntimeStates(snapshot);
        }
        publish.run();
        List<Runnable> notifications = new ArrayList<>();
        for (UidSnapshot snapshot : before) {
            List<ApkPackage> members = uidMembers(snapshot.uid);
            trimUidState(snapshot.uid, members);
            if (snapshot.uid == pkg.uid()) for (String permission : pkg.parsed.getRequestedPermissions()) {
                reconcileRuntimeState(snapshot.uid, mCatalog.get(permission), snapshot.permissions.get(permission), members);
            }
            List<String> revoked = new ArrayList<>();
            for (var entry : snapshot.permissions.entrySet()) {
                if (entry.getValue().access.granted && !permissionState(snapshot.uid, entry.getKey(), members).granted) revoked.add(entry.getKey());
            }
            publishRuntimeModes(snapshot.uid, snapshot.modes, publicationLock);
            ApkPackage current = members.stream().filter(member -> !mPm.isRemovalPending(member.packageName)).findFirst().orElse(null);
            if (current != null) notifications.add(() -> publishPermissionRevocation(current, revoked));
        }
        if (!saved.equals(mDecisions)) scheduleWrite();
        return () -> {
            if (mPm.get(pkg.packageName) != pkg || mPm.isRemovalPending(pkg.packageName)) return;
            for (Runnable notification : notifications) notification.run();
        };
    }

    private void publishPermissionRevocation(ApkPackage current, List<String> revoked) {
        permissionsChanged(current, () -> {
            synchronized (this) {
                List<ApkPackage> members = uidMembers(current.uid());
                return revoked.stream().anyMatch(permission -> !permissionState(current.uid(), permission, members).granted);
            }
        });
    }

    /** Uninstall must revoke decisions even if a later install reuses a package UID. */
    public void forgetPackage(String packageName) {
        synchronized (mAttributions) {
            mAttributions.values().removeIf(saved -> AppOpsAttribution.chain(saved.source).stream()
                    .anyMatch(source -> packageName.equals(source.packageName)));
        }
        final ApkPackage surviving;
        final List<String> revoked = new ArrayList<>();
        synchronized (mPm) { synchronized (this) {
            ApkPackage removed = mPm.get(packageName);
            List<ApkPackage> beforeMembers = removed == null ? List.of() : uidMembers(removed.uid());
            Map<Integer, Integer> beforeModes = removed == null ? Map.of() : runtimeModes(removed.uid());
            List<String> grantedRuntime = new ArrayList<>();
            if (removed != null) {
                // Removal trims UID ownership, retaining the existing grant basis while
                // another member still requests the permission, like AOSP trimPermissionStates.
                retainRuntimeStates(snapshotUid(removed.uid()));
                for (PermissionInfo info : mCatalog.permissions().values()) {
                    if (info.isRuntime() && permissionState(removed.uid(), info.name, beforeMembers).granted) {
                        grantedRuntime.add(info.name);
                    }
                }
                mForgottenPackages.put(removed, true);
            }
            List<ApkPackage> remaining = removed == null ? List.of() : uidMembers(removed.uid());
            surviving = remaining.stream().filter(member -> !mPm.isRemovalPending(member.packageName))
                    .findFirst().orElse(null);
            String prefix = removed == null ? null : "uid:" + removed.uid() + ":";
            String flagsPrefix = removed == null ? null : "flags:" + removed.uid() + ":";
            String legacyPrefix = removed == null ? null : "legacy-grant:" + removed.uid() + ":";
            mDecisions.keySet().removeIf(key -> {
                String saved = key.toString();
                if (saved.startsWith(packageName + ":")) return true;
                if (prefix == null) return false;
                String permission;
                if (saved.startsWith(prefix)) permission = saved.substring(prefix.length());
                else if (saved.startsWith(flagsPrefix)) permission = saved.substring(flagsPrefix.length());
                else if (saved.startsWith(legacyPrefix)) permission = saved.substring(legacyPrefix.length());
                else return false;
                for (ApkPackage member : remaining) {
                    if (member.parsed.getRequestedPermissions().contains(permission)) return false;
                }
                return true;
            });
            scheduleWrite();
            if (removed != null) {
                for (String permission : grantedRuntime) {
                    if (!permissionState(removed.uid(), permission, remaining).granted) revoked.add(permission);
                }
                publishRuntimeModes(removed.uid(), beforeModes);
            }
        } }
        // Publish and retire surviving UID processes before any fallible/blocking disk work.
        // The leaving generation is already unavailable; killUid must be anchored to a
        // surviving generation and retain its ordinary activity/service recovery behavior.
        if (surviving != null) publishPermissionRevocation(surviving, revoked);
        else invalidateCaches();
        if (NotificationManagerService.sLast != null) NotificationManagerService.sLast.permissionChanged(packageName);
        // Retirement is an explicit durable boundary, unlike an ordinary permission change.
        // The installer must not publish a successful uninstall with stale grants on disk.
        if (!mWriter.flush()) throw new IllegalStateException("Cannot clear permission decisions");
    }

    /** Called under the PM monitor after commit or rollback of this exact removal. */
    synchronized boolean finishPackageRemoval(ApkPackage original) {
        if (!mForgottenPackages.containsKey(original)) return false;
        Map<Integer, Integer> before = runtimeModes(original.uid());
        mForgottenPackages.remove(original);
        if (mPm.get(original.packageName) != original) return false;
        // A failed uninstall keeps the manifest installed. Do not restore deleted user
        // grants, but do restore permissions that the installed manifest grants itself.
        publishRuntimeModes(original.uid(), before);
        return true;
    }

    void packageRemovalRestored(ApkPackage original) { permissionsChanged(original, () -> false); }

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

    /** ActivityManager.checkComponentPermission, with this runtime's UID permission store. */
    int checkComponentPermission(String permission, int uid, int owningUid, boolean exported) {
        if (uid < 0) return PackageManager.PERMISSION_DENIED;
        if (android.app.ActivityManager.canAccessUnexportedComponents(uid)) return PackageManager.PERMISSION_GRANTED;
        if (android.os.UserHandle.isIsolated(uid)) return PackageManager.PERMISSION_DENIED;
        if (owningUid >= 0 && android.os.UserHandle.isSameApp(uid, owningUid)) return PackageManager.PERMISSION_GRANTED;
        if (!exported) return PackageManager.PERMISSION_DENIED;
        return permission == null ? PackageManager.PERMISSION_GRANTED : checkUidPermission(uid, permission, 0);
    }

    int checkProcessPermission(String permission, int pid, int uid) {
        CallerIdentity callers = mCallers;
        if (callers == null) return PackageManager.PERMISSION_DENIED;
        // A package's effective UID comes from its authenticated registered PID.
        String pkg = callers.packageForPid(pid);
        if (pid == Process.myPid()) {
            // Standalone Main binds ActivityThread without an ATM attachment.
            // Its app must not inherit the services that share this process.
            String bound = android.app.ActivityThread.currentPackageName();
            if (bound != null) {
                if (uid != Process.myUid() || pkg != null && !pkg.equals(bound)) {
                    return PackageManager.PERMISSION_DENIED;
                }
                pkg = bound;
            }
        }
        if (pkg != null) {
            ApkPackage installed = mPm.get(pkg);
            return installed != null && !mPm.isRemovalPending(pkg)
                    && installed.uid() == uid && granted(permission, pkg)
                    ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
        }
        // Only a genuine service-local context with no bound APK reaches here.
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
        runtimeDecision(pkg, permission, userId, decision, true);
    }

    private void runtimeDecision(String pkg, String permission, int userId, String decision,
            boolean killOnRevoke) {
        requireSystem(); user(userId);
        ApkPackage installed = mPm.get(pkg);
        PermissionInfo info = mCatalog.get(permission);
        if (installed == null || !installed.parsed.getRequestedPermissions().contains(permission)
                || info == null || !info.isRuntime() && (info.protectionLevel
                        & (PermissionInfo.PROTECTION_FLAG_ROLE | PermissionInfo.PROTECTION_FLAG_DEVELOPMENT)) == 0) {
            throw new IllegalArgumentException("Not a requested runtime permission: " + permission);
        }
        final boolean wasGranted;
        synchronized (mPm) { synchronized (this) {
            if (mPm.get(pkg) != installed || mPm.isRemovalPending(pkg)) {
                throw new SecurityException("Runtime permission owner changed or is retiring");
            }
            if (info.isRuntime() && installed.parsed.getTargetSdkVersion() < 23) return;
            PermissionState old = permissionState(installed.uid(), permission, uidMembers(installed.uid()));
            if ((old.flags & PackageManager.FLAG_PERMISSION_SYSTEM_FIXED) != 0) return;
            if ("granted".equals(decision) && old.restricted) return;
            wasGranted = granted(permission, pkg);
            Map<Integer, Integer> before = runtimeModes(installed.uid());
            List<ApkPackage> members = uidMembers(installed.uid());
            mDecisions.setProperty(legacyGrantKey(installed.uid(), permission), Boolean.toString(
                    storedLegacyGrant(installed.uid(), permission, info, requestingTargetSdk(permission, members))));
            mDecisions.setProperty(uidKey(installed.uid(), permission), decision);
            mDecisions.setProperty(flagsKey(installed.uid(), permission), Integer.toString(old.flags));
            for (ApkPackage member : members) mDecisions.remove(legacyKey(member, permission));
            scheduleWrite();
            publishRuntimeModes(installed.uid(), before);
        } }
        if (killOnRevoke) permissionChanged(installed, permission, wasGranted);
        else permissionsChanged(installed, () -> false);
    }

    private void permissionChanged(ApkPackage installed, String permission, boolean wasGranted) {
        permissionsChanged(installed, () -> wasGranted && !granted(permission, installed.packageName));
    }

    private void permissionsChanged(ApkPackage installed, java.util.function.BooleanSupplier stillRevoked) {
        invalidateCaches();
        NotificationManagerService notifications = NotificationManagerService.sLast;
        if (notifications != null) for (ApkPackage member : uidMembers(installed.uid())) {
            notifications.permissionChanged(member.packageName);
        }
        Services.Created services = Services.sLast;
        if (services != null && services.pm == mPm && stillRevoked.getAsBoolean()) {
            services.atm.killUidForPermissionChange(installed, stillRevoked);
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
        requireSystem();
        requireDefaultDevice(deviceId);
        runtimeDecision(pkg, permission, userId, "granted");
    }
    @Override public void revokeRuntimePermission(String pkg, String permission, String deviceId, int userId, String reason) {
        requireSystem();
        requireDefaultDevice(deviceId);
        runtimeDecision(pkg, permission, userId, "denied");
    }
    /**
     * Android's test-only POST_NOTIFICATIONS revoke: the same runtime-permission revoke
     * (state, flags, AppOps modes, cache invalidation and notification cleanup) but with
     * AOSP's SKIP_KILL_APP_REASON_NOTIFICATION_TEST, so the still-running process can
     * observe the denial. Ordinary revokes keep killing the UID.
     */
    @Override public void revokePostNotificationPermissionWithoutKillForTest(String pkg, int userId) {
        requireSystem();
        runtimeDecision(pkg, Manifest.permission.POST_NOTIFICATIONS, userId, "denied", false);
    }
    @Override public boolean shouldShowRequestPermissionRationale(String pkg, String permission, int deviceId, int userId) { return false; }
    @Override public synchronized int getPermissionFlags(String pkg, String permission, String deviceId, int userId) {
        ApkPackage installed = mPm.get(pkg);
        if (installed == null || userId != 0 || !"default:0".equals(deviceId)) return 0;
        return permissionState(installed.uid(), permission, uidMembers(installed.uid())).flags;
    }

    private static void requireDefaultDevice(String device) {
        if (!"default:0".equals(device)) throw new IllegalArgumentException("Unavailable permission device");
    }

    @Override public void updatePermissionFlags(String pkg, String permission, int mask, int values,
            boolean enforceAdjustPolicyPermission, String device, int userId) {
        requireSystem(); user(userId); requireDefaultDevice(device);
        ApkPackage installed = mPm.get(pkg);
        PermissionInfo info = mCatalog.get(permission);
        if (info == null) throw new IllegalArgumentException("Unknown permission " + permission);
        if (installed == null) return;
        final boolean wasGranted;
        synchronized (mPm) { synchronized (this) {
            if (mPm.get(pkg) != installed || mPm.isRemovalPending(pkg)) {
                throw new SecurityException("Permission owner changed or is retiring");
            }
            List<ApkPackage> members = uidMembers(installed.uid());
            if (members.stream().noneMatch(member -> member.parsed.getRequestedPermissions().contains(permission))) return;
            PermissionState old = permissionState(installed.uid(), permission, members);
            int flags = (old.flags & ~mask) | (values & mask & PackageManager.MASK_PERMISSION_FLAGS_ALL);
            if (!info.isRestricted()) flags &= ~PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
            flags = implicitPermissionFlags(flags, storedLegacyGrant(installed.uid(), permission, info,
                    requestingTargetSdk(permission, members)));
            if (old.flags == flags) return;
            wasGranted = granted(permission, pkg);
            Map<Integer, Integer> before = runtimeModes(installed.uid());
            mDecisions.setProperty(legacyGrantKey(installed.uid(), permission), Boolean.toString(
                    storedLegacyGrant(installed.uid(), permission, info, requestingTargetSdk(permission, members))));
            mDecisions.setProperty(flagsKey(installed.uid(), permission), Integer.toString(flags));
            scheduleWrite();
            publishRuntimeModes(installed.uid(), before);
        } }
        permissionChanged(installed, permission, wasGranted);
    }

    @Override public void updatePermissionFlagsForAllApps(int mask, int values, int userId) {
        requireSystem(); user(userId);
        java.util.Set<String> seen = new java.util.HashSet<>();
        for (ApkPackage pkg : mPm.packages()) for (String permission : pkg.parsed.getRequestedPermissions()) {
            if (mCatalog.get(permission) != null && seen.add(uidKey(pkg.uid(), permission))) {
                updatePermissionFlags(pkg.packageName, permission, mask, values, true, "default:0", userId);
            }
        }
    }

    /** Called only by the already-authorized AppOps UID setter, never a permission grant. */
    boolean setRuntimeAppOpMode(int uid, ApkPackage expected, int switchOp, int mode) {
        synchronized (mPm) { synchronized (this) {
            if (expected != null && (mPm.get(expected.packageName) != expected
                    || mPm.isRemovalPending(expected.packageName))) return false;
            List<ApkPackage> members = new ArrayList<>(uidMembers(uid));
            members.removeIf(member -> mPm.isRemovalPending(member.packageName));
            if (members.isEmpty()) return false;
            Map<Integer, Integer> before = runtimeModes(uid);
            boolean changed = false;
            // The outer Android AppOpsService updates all switched runtime permissions before
            // its checking service ignores the direct runtime-op setter.
            for (int op = 0; op < _NUM_OP; op++) {
                if (AppOpsManager.opToSwitch(op) != switchOp) continue;
                String permission = AppOpsManager.opToPermission(op);
                PermissionInfo info = permission == null ? null : mCatalog.get(permission);
                if (info == null || !info.isRuntime()) continue;
                PermissionState foreground = permissionState(uid, permission, members);
                if (!foreground.granted) continue;
                if (info.backgroundPermission != null) {
                    PermissionState background = permissionState(uid, info.backgroundPermission, members);
                    if (background.granted) changed |= setCompatFlag(uid, info.backgroundPermission,
                            background.flags, mode != MODE_ALLOWED);
                }
                changed |= setCompatFlag(uid, permission, foreground.flags,
                        mode != MODE_ALLOWED && (info.backgroundPermission == null || mode != MODE_FOREGROUND));
            }
            if (changed) { scheduleWrite(); publishRuntimeModes(uid, before); }
        } }
        invalidateCaches();
        return true;
    }

    private boolean setCompatFlag(int uid, String permission, int flags, boolean revoked) {
        int next = revoked ? flags | PackageManager.FLAG_PERMISSION_REVOKED_COMPAT
                : flags & ~PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
        if (flags == next) return false;
        PermissionInfo info = mCatalog.get(permission);
        mDecisions.setProperty(legacyGrantKey(uid, permission), Boolean.toString(
                storedLegacyGrant(uid, permission, info, requestingTargetSdk(permission, uidMembers(uid)))));
        mDecisions.setProperty(flagsKey(uid, permission), Integer.toString(next));
        return true;
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
        List<AttributionSourceState> chain = AppOpsAttribution.chain(source);
        boolean trusted = callers.isSystem() || checkProcessPermission(
                android.Manifest.permission.UPDATE_APP_OPS_STATS, callers.callerPid(), callers.callerUid())
                == PackageManager.PERMISSION_GRANTED;
        if (!trusted) {
            String name = callers.callerPackage();
            ApkPackage caller = mPm.get(name);
            if (caller == null || caller.uid() != source.uid || callers.callerUid() != source.uid
                    || source.pid != -1 && source.pid != callers.callerPid()) {
                throw new SecurityException("Attribution source does not belong to the caller");
            }
        }
        List<AttributionOwner> owners = new ArrayList<>();
        for (AttributionSourceState node : chain) {
            ApkPackage pkg = node.packageName == null ? null : mPm.get(node.packageName);
            boolean platform = "android".equals(node.packageName) && node.uid == Process.SYSTEM_UID;
            if (!platform && (pkg == null || pkg.uid() != node.uid || mPm.isRemovalPending(pkg.packageName))) {
                throw new SecurityException("Unknown attribution package/UID");
            }
            owners.add(new AttributionOwner(node.packageName, node.uid, pkg));
        }
        if (chain.size() > 1 && !isRegisteredAttributionSource(chain.get(1))) {
            throw new SecurityException("Attribution next link is not registered");
        }
        IBinder token = source.token == null ? new Binder() : source.token;
        AttributionSourceState saved = AppOpsAttribution.copy(source, false);
        synchronized (mAttributions) { mAttributions.put(token, new RegisteredAttribution(saved, List.copyOf(owners))); }
        return token;
    }
    @Override public boolean isRegisteredAttributionSource(AttributionSourceState source) {
        if (source == null || source.token == null) return false;
        synchronized (mAttributions) {
            RegisteredAttribution saved = mAttributions.get(source.token);
            if (saved == null || !attributionOwnersCurrent(saved)) return false;
            try { return AppOpsAttribution.equalExceptRootToken(saved.source, source); }
            catch (IllegalArgumentException malformed) { return false; }
        }
    }
    private boolean attributionOwnersCurrent(RegisteredAttribution saved) {
        for (AttributionOwner owner : saved.owners) {
            if (mPm.get(owner.name) != owner.code || mPm.isRemovalPending(owner.name)) return false;
            if (owner.code != null && owner.code.uid() != owner.uid) return false;
        }
        return true;
    }
    @Override public int getRegisteredAttributionSourceCount(int uid) {
        if (mCallers == null || !mCallers.isSystem()) throw new SecurityException("Attribution count requires system authority");
        synchronized (mAttributions) {
            mAttributions.values().removeIf(saved -> !attributionOwnersCurrent(saved));
            return (int)mAttributions.values().stream().filter(saved -> saved.source.uid == uid).count();
        }
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
