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

    /**
     * Android 16 PermissionFlags distinguishes the permission grant from its AppOps grant.
     * {@code reviewPending}: a restricted permission's legacy grant still carries
     * REVIEW_REQUIRED. AOSP keeps that grant (checkPermission); its PermissionPolicyService
     * leaves the AppOp of a permission under review untouched and gates the app through the
     * permission review screen instead. Gesher has no review screen, so it withholds the
     * AppOp as the stand-in for that gate until the review flag is cleared, whatever the
     * exemptions.
     */
    private record PermissionState(boolean granted, int flags, boolean restricted, boolean reviewPending) {
        PermissionState(boolean granted, int flags, boolean restricted) { this(granted, flags, restricted, false); }

        boolean appOpGranted() {
            return granted && !restricted && !reviewPending
                    && (flags & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) == 0;
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
        boolean restricted = restricted(info, flags, members);
        int protection = info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE;
        if (protection == PermissionInfo.PROTECTION_NORMAL) return new PermissionState(true, flags, restricted);
        if (protection == PermissionInfo.PROTECTION_DANGEROUS) {
            return dangerousState(info, flags, legacy, "granted".equals(decision), restricted);
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

    /**
     * A dangerous permission's state. LEGACY_GRANTED stays a permission grant, also for a
     * restricted permission (AOSP restorePermissionState always grants a legacy app's
     * permissions); its revocation is represented by REVOKED_COMPAT, so PermissionChecker can
     * distinguish hard and soft denial. A restriction withholds only the AppOp (AOSP
     * PermissionPolicyService); so does a restricted permission's pending review, Gesher's
     * stand-in for AOSP's review gate (see PermissionState).
     */
    private static PermissionState dangerousState(PermissionInfo info, int flags, boolean legacy, boolean explicit,
            boolean restricted) {
        boolean granted = legacy || explicit && !(restricted && info.isHardRestricted());
        boolean reviewPending = legacy && info.isRestricted()
                && (flags & PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED) != 0;
        return new PermissionState(granted, flags, restricted, reviewPending);
    }

    /**
     * Whether APPLY_RESTRICTION withholds this permission. A hard restriction withholds the
     * grant itself. Like AOSP SoftRestrictedPermissionPolicy (READ/WRITE_EXTERNAL_STORAGE),
     * a soft restriction withholds only an app whose UID's minimum target is below Q; for
     * newer targets it concerns only OP_LEGACY_STORAGE, which this policy does not derive.
     */
    private static boolean restricted(PermissionInfo info, int flags, List<ApkPackage> members) {
        if ((flags & PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION) == 0 || !info.isRestricted()) return false;
        return info.isHardRestricted() || minimumTargetSdk(members) < android.os.Build.VERSION_CODES.Q;
    }

    /** AOSP SoftRestrictedPermissionPolicy.getMinimumTargetSDK: every package of the UID. */
    private static int minimumTargetSdk(List<ApkPackage> members) {
        int target = Integer.MAX_VALUE;
        for (ApkPackage member : members) target = Math.min(target, member.parsed.getTargetSdkVersion());
        return target == Integer.MAX_VALUE ? 0 : target;
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

    /**
     * Reconcile the runtime grant flags when the requesting APK is published or admitted at
     * boot. {@code exemptions} are restriction-exemption flags the install adds (AOSP
     * onPackageInstalled's installer allowlist, or a system package's exemption).
     */
    private void reconcileRuntimeState(int uid, PermissionInfo info, RuntimeState old, List<ApkPackage> members,
            int exemptions) {
        if (info == null || !info.isRuntime()) return;
        String permission = info.name;
        int target = requestingTargetSdk(permission, members);
        if (target == Integer.MAX_VALUE) return;
        if (old == null) old = new RuntimeState(new PermissionState(false, 0, false), false, false);
        int implicit = PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED | PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED;
        int flags = old.access.flags;
        boolean legacy = legacyGranted(info, target), granted = old.explicitGrant;
        if (target < 23 && !legacy) {
            // Runtime-only permissions keep restriction exemptions, never a compatibility
            // grant or old user/runtime grant bits, during legacy package evaluation.
            flags &= RESTRICTION_EXEMPT;
            granted = false;
        } else if (legacy) {
            if (old.hasState() && !old.access.granted) flags |= PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
            if (!old.hasState()) flags |= PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
            // AOSP restorePermissionState: dropping the restriction of a legacy app's
            // permission implies a review, so the exemption this admission adds never makes
            // its AppOp effective without one.
            if ((flags & PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION) != 0
                    && (applyRestriction(info, flags | exemptions) & PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION) == 0) {
                flags |= PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
            }
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
        storeRuntimeState(uid, permission, legacy, granted, applyRestriction(info, flags | exemptions));
    }

    /**
     * AOSP restorePermissionState: a restricted permission without an exemption carries
     * APPLY_RESTRICTION, and an exemption clears it. Unrestricted permissions keep neither.
     */
    private static int applyRestriction(PermissionInfo info, int flags) {
        if (!info.isRestricted() || (flags & RESTRICTION_EXEMPT) != 0) {
            return flags & ~PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
        }
        return flags | PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
    }

    /** Exemptions a package's own admission supplies for {@code permission}. */
    private static int admissionExemptions(PermissionInfo info, ApkPackage pkg, boolean newState) {
        if (info == null || !info.isRestricted() || !pkg.parsed.getRequestedPermissions().contains(info.name)) return 0;
        int exemptions = 0;
        // A verified system addon (microG) is a preinstalled system package: AOSP exempts
        // those through the system allowlist, never by package name.
        if (pkg.isSystemAddon()) exemptions |= PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
        java.util.Set<String> allowlist = pkg.restrictedPermissionAllowlist();
        // AOSP SessionParams and pm install default to INSTALL_ALL_WHITELIST_RESTRICTED_PERMISSIONS.
        // An unrecorded request (an internal republication) adds no exemption to existing state.
        if (allowlist == null ? newState : allowlist.contains(info.name)) {
            exemptions |= PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT;
        }
        return exemptions;
    }

    /** Boot admission re-evaluates saved flags against the installed requesting packages. */
    synchronized void initializePackages() {
        Properties saved = new Properties(); saved.putAll(mDecisions);
        // Definitions do not change while the initial admitted set is reconciled.
        // Rebuilding the entire catalogue for each UID multiplies package scans.
        Map<String, PermissionInfo> permissions = mCatalog.permissions();
        // One-time migration (issue #485): Gesher used to record no restriction exemption,
        // so restricted permissions could never be granted. Every package admitted before
        // this version came through a path that now defaults to AOSP's installer allowlist
        // of all requested restricted permissions. User decisions and flags stay as they are.
        boolean migrate = !EXEMPTION_VERSION.equals(mDecisions.getProperty(EXEMPTION_VERSION_KEY));
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
                    boolean restricted = restricted(info, flags, members);
                    old = new RuntimeState(new PermissionState(legacy || granted && !(restricted && info.isHardRestricted()),
                            flags, restricted), legacy, granted);
                }
                // Only the one-time migration supplies the default installer exemption. Outside
                // it, a permission without recorded state (a publication whose ledger write was
                // lost, or a permission the catalogue gained later) stays restricted: an install
                // that opted out must never become exempt, and installs write their state
                // synchronously (Installer, flushDecisions).
                int exemptions = info.isRestricted() && migrate
                        ? PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT : 0;
                for (ApkPackage member : members) exemptions |= admissionExemptions(info, member, false)
                        & PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
                reconcileRuntimeState(pkg.uid(), info, old, members, exemptions);
            }
        }
        // Without restriction metadata (framework assets unavailable) nothing was migrated:
        // leave the version unrecorded so a boot with the real catalogue performs it.
        if (permissions.values().stream().anyMatch(PermissionInfo::isRestricted)) {
            mDecisions.setProperty(EXEMPTION_VERSION_KEY, EXEMPTION_VERSION);
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
                PermissionInfo info = mCatalog.get(permission);
                RuntimeState old = snapshot.permissions.get(permission);
                reconcileRuntimeState(snapshot.uid, info, old, members, admissionExemptions(info, pkg, old == null));
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
        // The attached process owns its installation, even when a newer APK
        // has already been published with the same package name and UID.
        ApkPackage owner = callers.packageCodeForPid(pid, uid);
        if (owner != null) {
            synchronized (this) {
                return mPm.get(owner.packageName) == owner && !mPm.isRemovalPending(owner.packageName)
                        && granted(permission, owner.packageName)
                        ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
            }
        }
        if (pid == Process.myPid() && callers.packageForPid(pid) == null
                && (uid == Process.SYSTEM_UID || uid == Process.myUid())) {
            return PackageManager.PERMISSION_GRANTED;
        }
        return PackageManager.PERMISSION_DENIED;
    }

    @Override public int getPermissionRequestState(String pkg, String permission, int deviceId) {
        if (deviceId != Context.DEVICE_ID_DEFAULT) return Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE;
        if (!mCallers.isSystem()) {
            try {
                // Android answers the calling UID's existing grant before looking
                // at the named package's requestability. A claimed name is not an
                // authority to inspect another UID's permission history.
                String caller = mCallers.callerPackage();
                if (granted(permission, caller)) return Context.PERMISSION_REQUEST_STATE_GRANTED;
                mCallers.requirePackage(pkg);
            } catch (SecurityException e) { return Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE; }
        }
        if (granted(permission, pkg)) return Context.PERMISSION_REQUEST_STATE_GRANTED;
        return requestable(pkg, permission) ? Context.PERMISSION_REQUEST_STATE_REQUESTABLE
                : Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE;
    }

    private static final int RESTRICTION_EXEMPT = PackageManager.FLAGS_PERMISSION_RESTRICTION_ANY_EXEMPT;
    // Versioned one-time state migrations of the runtime-permissions ledger.
    private static final String EXEMPTION_VERSION_KEY = "restriction-exemptions", EXEMPTION_VERSION = "1";
    private static final int FIXED_PERMISSION_FLAGS = PackageManager.FLAG_PERMISSION_USER_FIXED
            | PackageManager.FLAG_PERMISSION_POLICY_FIXED | PackageManager.FLAG_PERMISSION_SYSTEM_FIXED;

    private static boolean unrequestable(PermissionInfo info, PermissionState state) {
        // APPLY_RESTRICTION also describes soft restrictions. Only AOSP's hard
        // RESTRICTION_REVOKED flag belongs in its UNREQUESTABLE_MASK.
        return (state.flags & FIXED_PERMISSION_FLAGS) != 0 || state.restricted && info.isHardRestricted();
    }

    private synchronized boolean requestable(String pkg, String permission) {
        if (pkg == null || permission == null) return false;
        ApkPackage installed = mPm.get(pkg);
        PermissionInfo info = mCatalog.get(permission);
        return installed != null && !mPm.isRemovalPending(pkg)
                && installed.parsed.getRequestedPermissions().contains(permission)
                && info != null && info.isRuntime()
                && !unrequestable(info, permissionState(installed.uid(), permission, uidMembers(installed.uid())));
    }

    /**
     * Controller eligibility is narrower than Android's public request-state query. Like
     * PermissionController, a grant the policy would refuse (a hard restriction, or a soft
     * restriction of a pre-Q UID) is never offered to the user.
     */
    public synchronized boolean canRequest(String pkg, String permission) {
        ApkPackage installed = mPm.get(pkg);
        return installed != null && installed.parsed.getTargetSdkVersion() >= 23 && requestable(pkg, permission)
                && !permissionState(installed.uid(), permission, uidMembers(installed.uid())).restricted;
    }

    /**
     * Whether the user can change this permission on the app's Settings page (AOSP
     * AppPermissionFragment). Unlike a request, Settings still offers a USER_FIXED
     * permission; policy and system fixed permissions and restrictions stay unchangeable.
     */
    private synchronized boolean changeableInSettings(String pkg, String permission) {
        ApkPackage installed = mPm.get(pkg);
        PermissionInfo info = mCatalog.get(permission);
        if (installed == null || mPm.isRemovalPending(pkg) || installed.parsed.getTargetSdkVersion() < 23
                || !installed.parsed.getRequestedPermissions().contains(permission)
                || info == null || !info.isRuntime()) return false;
        PermissionState state = permissionState(installed.uid(), permission, uidMembers(installed.uid()));
        return !state.restricted && (state.flags & (PackageManager.FLAG_PERMISSION_POLICY_FIXED
                | PackageManager.FLAG_PERMISSION_SYSTEM_FIXED)) == 0;
    }

    private boolean changeable(String pkg, String permission, boolean settings) {
        return settings ? changeableInSettings(pkg, permission) : canRequest(pkg, permission);
    }

    /** Android R: the release from which a split background permission is requested alone. */
    private static final int BACKGROUND_SPLIT_SDK = android.os.Build.VERSION_CODES.R;

    /** What one requestPermissions call asks the user, per PermissionController's grant behaviors. */
    record RequestPlan(List<String> foreground, List<String> background, java.util.Set<String> foregroundAlreadyGranted) {
        static final RequestPlan NONE = new RequestPlan(List.of(), List.of(), java.util.Set.of());
    }

    /** The requested runtime permissions whose background permission is {@code background}. */
    private List<String> foregroundsOf(ApkPackage pkg, String background) {
        List<String> foregrounds = new ArrayList<>();
        PermissionInfo bg = mCatalog.get(background);
        for (String requested : pkg.parsed.getRequestedPermissions()) {
            PermissionInfo info = mCatalog.get(requested);
            if (info != null && bg != null && info.isRuntime() && background.equals(info.backgroundPermission)
                    && java.util.Objects.equals(info.packageName, bg.packageName)) foregrounds.add(requested);
        }
        return foregrounds;
    }

    private boolean anyGranted(ApkPackage pkg, List<String> permissions) {
        for (String permission : permissions) if (granted(permission, pkg.packageName)) return true;
        return false;
    }

    /**
     * Android 16 BackgroundGrantBehavior: a background permission is never granted by the
     * foreground Allow. A background request without its foreground (requested now or
     * granted) is rejected without UI. From Android R targets, foreground and background
     * requested together reject the whole request without UI, and a background request
     * alone goes to its separate choice (Settings on a phone); older targets get the
     * foreground dialog and then the background choice (SETTINGS_LINK_FOR_BG,
     * UPGRADE_SETTINGS_LINK).
     */
    synchronized RequestPlan planRequest(ApkPackage expected, String[] names) {
        java.util.Set<String> backgrounds = RuntimePermissionGroups.backgroundPermissions(mCatalog.permissions());
        List<String> foreground = new ArrayList<>(), background = new ArrayList<>();
        java.util.Set<String> foregroundGranted = new java.util.HashSet<>();
        List<String> requested = new ArrayList<>();
        for (String name : names) if (name != null) requested.add(name);
        for (String name : requested) {
            if (!canRequest(expected.packageName, name) || granted(name, expected.packageName)) continue;
            if (!backgrounds.contains(name)) {
                if (!foreground.contains(name)) foreground.add(name);
                continue;
            }
            if (background.contains(name)) continue;
            List<String> foregrounds = foregroundsOf(expected, name);
            boolean requestsForeground = requested.stream().anyMatch(foregrounds::contains);
            boolean grantedForeground = anyGranted(expected, foregrounds);
            if (!requestsForeground && !grantedForeground) continue; // NO_UI_REJECT_THIS_GROUP
            if (expected.parsed.getTargetSdkVersion() >= BACKGROUND_SPLIT_SDK && requestsForeground) {
                return RequestPlan.NONE; // NO_UI_REJECT_ALL_GROUPS
            }
            background.add(name);
            if (grantedForeground) foregroundGranted.add(name);
        }
        return new RequestPlan(List.copyOf(foreground), List.copyOf(background), java.util.Set.copyOf(foregroundGranted));
    }

    /**
     * The background choice is offered only while its foreground is granted. {@code settings}:
     * the choice is the app's Settings page (App info, or a target R+ request sent there),
     * which still offers a USER_FIXED background permission; otherwise it is the request
     * dialog of a pre-R target, which does not.
     */
    synchronized boolean backgroundChoiceAvailable(ApkPackage expected, String background, boolean settings) {
        return mPm.get(expected.packageName) == expected && !mPm.isRemovalPending(expected.packageName)
                && changeable(expected.packageName, background, settings) && !granted(background, expected.packageName)
                && anyGranted(expected, foregroundsOf(expected, background));
    }

    /**
     * The background permission an app's own Settings page would let the user choose now
     * (AOSP AppPermissionFragment): requested, not policy or system fixed (a user's fixed
     * refusal can still be changed there), not granted, foreground granted.
     */
    synchronized String pendingBackgroundChoice(ApkPackage expected) {
        java.util.Set<String> backgrounds = RuntimePermissionGroups.backgroundPermissions(mCatalog.permissions());
        for (String requested : expected.parsed.getRequestedPermissions()) {
            if (backgrounds.contains(requested) && backgroundChoiceAvailable(expected, requested, true)) return requested;
        }
        return null;
    }

    /**
     * Commit the background dialog's explicit choice (PermissionRequests.ALWAYS,
     * FOREGROUND_ONLY or DENIED). "Don't allow" also denies the foreground, and revoking a
     * grant that existed before this request stops the app as Android does.
     * <p>The request dialog of a pre-R target follows PermissionController's upgrade
     * dialog: each subgroup's repeated denial is fixed. On the Settings page
     * ({@code settings}, AOSP AppPermissionViewModel.requestChange) "Allow only while using
     * the app" leaves the ungranted background permission as it is unless the user had fixed
     * it, in which case it is revoked again with USER_SET only (userFixed=false), clearing
     * USER_FIXED; "Don't allow" revokes the same way.
     */
    void recordBackgroundChoice(ApkPackage expected, String background, int choice, boolean killOnRevoke,
            boolean settings) {
        requireSystem();
        if (choice != PermissionRequests.ALWAYS && choice != PermissionRequests.FOREGROUND_ONLY
                && choice != PermissionRequests.DENIED) return;
        List<String> revoked = new ArrayList<>();
        boolean changed;
        // One commit: the foreground cannot change between this check and the grant.
        synchronized (mPm) { synchronized (this) {
            if (!backgroundChoiceAvailable(expected, background, settings)) return;
            if (settings && choice == PermissionRequests.FOREGROUND_ONLY
                    && (permissionState(expected.uid(), background, uidMembers(expected.uid())).flags
                            & PackageManager.FLAG_PERMISSION_USER_FIXED) == 0) return;
            changed = commitUserDecision(expected, List.of(background), choice == PermissionRequests.ALWAYS,
                    false, settings, revoked);
            if (choice == PermissionRequests.DENIED) {
                changed |= commitUserDecision(expected, foregroundsOf(expected, background), false, true,
                        settings, revoked);
            }
        } }
        // Process retirement and notifications run outside the PM and policy monitors.
        if (changed) permissionsChanged(expected, () -> killOnRevoke && revoked.stream()
                .anyMatch(name -> !granted(name, expected.packageName)),
                ExternalStorageAccess.isStoragePermission(background));
    }

    /**
     * A target R+ background request sent to the Settings page returned without a choice.
     * Like AOSP GrantPermissionsViewModel.sendDirectlyToSettings, that counts against the
     * app: the background permission becomes USER_SET, or USER_FIXED when it already was,
     * so an app cannot keep reopening the choice.
     */
    void recordBackgroundDismissal(ApkPackage expected, String background) {
        requireSystem();
        synchronized (mPm) { synchronized (this) {
            if (!backgroundChoiceAvailable(expected, background, false)) return;
            List<ApkPackage> members = uidMembers(expected.uid());
            PermissionInfo info = mCatalog.get(background);
            int flags = permissionState(expected.uid(), background, members, info).flags;
            flags |= (flags & PackageManager.FLAG_PERMISSION_USER_SET) != 0
                    ? PackageManager.FLAG_PERMISSION_USER_FIXED : PackageManager.FLAG_PERMISSION_USER_SET;
            Map<Integer, Integer> before = runtimeModes(expected.uid());
            // The background permission is not granted here (backgroundChoiceAvailable).
            storeRuntimeDecision(expected, background, info, members, flags, "denied");
            scheduleWrite();
            publishRuntimeModes(expected.uid(), before);
        } }
        permissionsChanged(expected, () -> false, ExternalStorageAccess.isStoragePermission(background));
    }

    /**
     * Explicit durability boundary for an install: the admission's restriction exemptions
     * (or their absence) reach the ledger before the install reports success. Never call
     * while holding a package or operation monitor.
     */
    boolean flushDecisions() { return mWriter.flush(); }

    public boolean isProtectedBroadcast(String action) { return mCatalog.isProtectedBroadcast(action); }

    boolean isKnownProtectedBroadcast(String action) { return mCatalog.isKnownProtectedBroadcast(action); }

    boolean isDeclaredProtectedBroadcast(String action) { return mCatalog.isDeclaredProtectedBroadcast(action); }

    public int checkPermission(String pkg, String permission) {
        return checkPermission(pkg, permission, "default:0", 0);
    }

    public void grantRuntime(String pkg, String permission, int userId) {
        grantRuntimePermission(pkg, permission, "default:0", userId);
    }

    /** Commit an explicit host-dialog choice, never a dismissal or an unavailable dialog. */
    void recordUserDecision(ApkPackage expected, List<String> permissions, boolean allowed) {
        requireSystem();
        if (!commitUserDecision(expected, permissions, allowed, false, false, new ArrayList<>())) return;
        // Every changed permission was still denied at commit time. In particular,
        // an old dialog cannot revoke a grant that another controller just made.
        permissionsChanged(expected, () -> false,
                permissions.stream().anyMatch(ExternalStorageAccess::isStoragePermission));
    }

    /**
     * Store an explicit user choice; false when nothing changed. {@code revokeGranted}: a
     * denial also revokes the listed permissions' existing grants, added to {@code revoked}.
     * {@code settings}: a Settings page change, which may change a USER_FIXED permission and
     * whose denial never fixes one (AOSP revokes there with userFixed=false).
     */
    private boolean commitUserDecision(ApkPackage expected, List<String> permissions, boolean allowed,
            boolean revokeGranted, boolean settings, List<String> revoked) {
        synchronized (mPm) { synchronized (this) {
            if (mPm.get(expected.packageName) != expected || mPm.isRemovalPending(expected.packageName)) return false;
            List<ApkPackage> members = uidMembers(expected.uid());
            Map<String, PermissionState> pending = new LinkedHashMap<>();
            Map<String, Boolean> previouslySet = new LinkedHashMap<>();
            java.util.Set<String> background = allowed ? java.util.Set.of()
                    : RuntimePermissionGroups.backgroundPermissions(mCatalog.permissions());
            for (String name : permissions) {
                if (!changeable(expected.packageName, name, settings)) continue;
                if (granted(name, expected.packageName)) {
                    if (allowed || !revokeGranted) continue;
                    if (permissionState(expected.uid(), name, members).granted) revoked.add(name);
                }
                PermissionInfo info = mCatalog.get(name);
                pending.put(name, permissionState(expected.uid(), name, members));
                if (allowed) continue;
                // Revoking access the user already has is the Settings page's change,
                // which AOSP records as USER_SET without USER_FIXED.
                if (revokeGranted || settings) { previouslySet.put(name, false); continue; }
                // Snapshot this foreground/background subgroup's history before mutating it.
                // Two names in a first request must not count as two user denials.
                boolean userSet = false;
                String group = RuntimePermissionGroups.group(info);
                for (String requested : expected.parsed.getRequestedPermissions()) {
                    PermissionInfo other = mCatalog.get(requested);
                    boolean sameGroup = other != null && other.isRuntime() && (name.equals(requested)
                            || group != null && group.equals(RuntimePermissionGroups.group(other))
                            && java.util.Objects.equals(info.packageName, other.packageName)
                            && background.contains(name) == background.contains(requested));
                    if (sameGroup && (permissionState(expected.uid(), requested, members).flags
                            & PackageManager.FLAG_PERMISSION_USER_SET) != 0) userSet = true;
                }
                previouslySet.put(name, userSet);
            }
            if (pending.isEmpty()) return false;
            Map<Integer, Integer> before = runtimeModes(expected.uid());
            for (Map.Entry<String, PermissionState> entry : pending.entrySet()) {
                String name = entry.getKey();
                PermissionInfo info = mCatalog.get(name);
                int flags = (entry.getValue().flags | PackageManager.FLAG_PERMISSION_USER_SET)
                        & ~(PackageManager.FLAG_PERMISSION_USER_FIXED | PackageManager.FLAG_PERMISSION_ONE_TIME
                            | PackageManager.FLAG_PERMISSION_AUTO_REVOKED | PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED);
                if (allowed) flags &= ~(PackageManager.FLAG_PERMISSION_REVOKED_COMPAT
                        | PackageManager.FLAG_PERMISSION_REVOKE_WHEN_REQUESTED);
                else if (previouslySet.get(name)) flags |= PackageManager.FLAG_PERMISSION_USER_FIXED;
                storeRuntimeDecision(expected, name, info, members, flags, allowed ? "granted" : "denied");
            }
            scheduleWrite();
            publishRuntimeModes(expected.uid(), before);
            return true;
        } }
    }

    private void storeRuntimeDecision(ApkPackage installed, String permission, PermissionInfo info,
            List<ApkPackage> members, int flags, String decision) {
        mDecisions.setProperty(legacyGrantKey(installed.uid(), permission), Boolean.toString(
                storedLegacyGrant(installed.uid(), permission, info, requestingTargetSdk(permission, members))));
        mDecisions.setProperty(uidKey(installed.uid(), permission), decision);
        mDecisions.setProperty(flagsKey(installed.uid(), permission), Integer.toString(flags));
        for (ApkPackage member : members) mDecisions.remove(legacyKey(member, permission));
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
            storeRuntimeDecision(installed, permission, info, members, old.flags, decision);
            scheduleWrite();
            publishRuntimeModes(installed.uid(), before);
        } }
        if (killOnRevoke) permissionChanged(installed, permission, wasGranted);
        else permissionsChanged(installed, () -> false, ExternalStorageAccess.isStoragePermission(permission));
    }

    private void permissionChanged(ApkPackage installed, String permission, boolean wasGranted) {
        permissionsChanged(installed, () -> wasGranted && !granted(permission, installed.packageName),
                ExternalStorageAccess.isStoragePermission(permission));
    }

    private void permissionsChanged(ApkPackage installed, java.util.function.BooleanSupplier stillRevoked) {
        permissionsChanged(installed, stillRevoked, true);
    }

    private void permissionsChanged(ApkPackage installed, java.util.function.BooleanSupplier stillRevoked,
            boolean storageChanged) {
        invalidateCaches();
        try {
            if (storageChanged) ExternalStorageAccess.syncUid(mPm, installed.uid());
        } finally {
            NotificationManagerService notifications = NotificationManagerService.sLast;
            if (notifications != null) for (ApkPackage member : uidMembers(installed.uid())) {
                notifications.permissionChanged(member.packageName);
            }
            Services.Created services = Services.sLast;
            if (services != null && services.pm == mPm && stillRevoked.getAsBoolean()) {
                services.atm.killUidForPermissionChange(installed, stillRevoked);
            }
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
    @Override public boolean shouldShowRequestPermissionRationale(String pkg, String permission,
            int deviceId, int userId) {
        if (pkg == null || permission == null || userId != 0 || deviceId != Context.DEVICE_ID_DEFAULT) return false;
        ApkPackage installed = mPm.get(pkg);
        if (installed == null || mPm.isRemovalPending(pkg) || mCallers.callerUid() != installed.uid()) return false;
        try { mCallers.requirePackage(pkg); }
        catch (SecurityException e) { return false; }
        // Caller authentication can acquire ATM's monitor; do it before the policy
        // monitor, whose existing readers also run from ATM lifecycle operations.
        synchronized (this) {
            if (mPm.get(pkg) != installed || mPm.isRemovalPending(pkg)) return false;
            PermissionInfo info = mCatalog.get(permission);
            if (info == null || !info.isRuntime() || granted(permission, pkg)) return false;
            PermissionState state = permissionState(installed.uid(), permission, uidMembers(installed.uid()));
            if (unrequestable(info, state)) return false;
            // AOSP BACKGROUND_RATIONALE_CHANGE_ID (147316723), enabled after target Q.
            if (Manifest.permission.ACCESS_BACKGROUND_LOCATION.equals(permission)
                    && installed.parsed.getTargetSdkVersion() >= 30) return true;
            return (state.flags & PackageManager.FLAG_PERMISSION_USER_SET) != 0;
        }
    }
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
        if (ExternalStorageAccess.isStorageOp(switchOp)) ExternalStorageAccess.syncUid(mPm, uid);
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
    private static final int ALLOWLIST_FLAGS = PackageManager.FLAG_PERMISSION_WHITELIST_SYSTEM
            | PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER | PackageManager.FLAG_PERMISSION_WHITELIST_UPGRADE;

    /** The exemption flag each AOSP allowlist records. */
    private static int exemptionFlags(int allowlists) {
        int exemptions = 0;
        if ((allowlists & PackageManager.FLAG_PERMISSION_WHITELIST_SYSTEM) != 0) {
            exemptions |= PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
        }
        if ((allowlists & PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER) != 0) {
            exemptions |= PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT;
        }
        if ((allowlists & PackageManager.FLAG_PERMISSION_WHITELIST_UPGRADE) != 0) {
            exemptions |= PackageManager.FLAG_PERMISSION_RESTRICTION_UPGRADE_EXEMPT;
        }
        return exemptions;
    }

    private record AllowlistIdentity(boolean system, ApkPackage code, int uid) { }
    private record AllowlistCaller(boolean privileged, boolean installer) { }

    /** ATM can enter PM; authenticate attached code before acquiring either state monitor. */
    private AllowlistIdentity allowlistIdentity() {
        CallerIdentity callers = mCallers;
        if (callers == null) throw new SecurityException("Unrecognized allowlist caller");
        int uid = callers.callerUid();
        boolean system = callers.isSystem();
        ApkPackage code = system ? null : callers.requireCurrentPackage(callers.callerPackage());
        return new AllowlistIdentity(system, code, uid);
    }

    /** PM and policy are locked: revalidate installation, attribution and permission together. */
    private AllowlistCaller allowlistCaller(ApkPackage target, AllowlistIdentity identity) {
        if (identity.system) return new AllowlistCaller(true, false);
        ApkPackage code = identity.code;
        if (code == null || mPm.get(code.packageName) != code || code.uid() != identity.uid
                || mPm.isRemovalPending(code.packageName)) {
            throw new SecurityException("Unknown or retired allowlist caller");
        }
        boolean privileged = granted(Manifest.permission.WHITELIST_RESTRICTED_PERMISSIONS, code.packageName);
        PackageInstallSource source = target.installSource();
        ApkPackage recorded = mPm.get(source.installer());
        boolean installer = source.isInstaller(recorded) && !mPm.isRemovalPending(recorded.packageName)
                && PackageManagerService.sameInstallSourceIdentity(code, recorded);
        // The current installer (including its admitted shared UID) can see its installation.
        // Other invisible packages answer as absent, before category-specific authorization.
        if (!privileged && !installer && !code.packageName.equals(target.packageName)
                && !mPm.canSeeInstalledPackage(code.packageName, target.packageName)) return null;
        return new AllowlistCaller(privileged, installer);
    }

    private static void checkAllowlistFlags(int flags) {
        if ((flags & ~ALLOWLIST_FLAGS) != 0) throw new IllegalArgumentException("Invalid allowlist flags: " + flags);
    }

    private static void requireAllowlistAuthority(AllowlistCaller caller, int flags) {
        if ((flags & PackageManager.FLAG_PERMISSION_WHITELIST_SYSTEM) != 0 && !caller.privileged) {
            throw new SecurityException("System allowlist requires " + Manifest.permission.WHITELIST_RESTRICTED_PERMISSIONS);
        }
        if ((flags & (PackageManager.FLAG_PERMISSION_WHITELIST_UPGRADE | PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER)) != 0
                && !caller.privileged && !caller.installer) {
            throw new SecurityException("Upgrade or installer allowlist requires being installer on record or "
                    + Manifest.permission.WHITELIST_RESTRICTED_PERMISSIONS);
        }
    }

    /** AOSP PermissionManagerServiceImpl.getAllowlistedRestrictedPermissions, including its caller checks. */
    @Override public List<String> getAllowlistedRestrictedPermissions(String pkg, int flags, int userId) {
        java.util.Objects.requireNonNull(pkg);
        checkAllowlistFlags(flags);
        if (userId < 0) throw new IllegalArgumentException("Invalid user " + userId);
        if (userId != 0) return null;
        AllowlistIdentity identity = allowlistIdentity();
        synchronized (mPm) { synchronized (this) {
            ApkPackage target = mPm.get(pkg);
            if (target == null || mPm.isRemovalPending(pkg)) return null;
            AllowlistCaller caller = allowlistCaller(target, identity);
            if (caller == null) return null;
            requireAllowlistAuthority(caller, flags);
            return allowlisted(target, flags);
        } }
    }

    /** Called with PM and policy locked. */
    private List<String> allowlisted(ApkPackage target, int allowlists) {
        int query = exemptionFlags(allowlists);
        List<ApkPackage> members = uidMembers(target.uid());
        List<String> result = new ArrayList<>();
        for (String permission : target.parsed.getRequestedPermissions()) {
            PermissionInfo info = mCatalog.get(permission);
            if (info != null && (permissionState(target.uid(), permission, members, info).flags & query) != 0) {
                result.add(permission);
            }
        }
        return result.isEmpty() ? null : result;
    }

    @Override public boolean addAllowlistedRestrictedPermission(String pkg, String permission, int flags, int userId) {
        return updateAllowlistedRestrictedPermission(pkg, permission, flags, userId, true);
    }

    @Override public boolean removeAllowlistedRestrictedPermission(String pkg, String permission, int flags, int userId) {
        return updateAllowlistedRestrictedPermission(pkg, permission, flags, userId, false);
    }

    /** Authorize and change one entry against one current installation and state snapshot. */
    private boolean updateAllowlistedRestrictedPermission(String pkg, String permission, int flags, int userId,
            boolean add) {
        java.util.Objects.requireNonNull(pkg);
        java.util.Objects.requireNonNull(permission);
        checkAllowlistFlags(flags);
        if (Integer.bitCount(flags) != 1) throw new IllegalArgumentException("Exactly one allowlist is changed");
        if (userId < 0) throw new IllegalArgumentException("Invalid user " + userId);
        if (userId != 0) return false;
        AllowlistIdentity identity = allowlistIdentity();
        ApkPackage target;
        List<String> lostGrants;
        synchronized (mPm) { synchronized (this) {
            target = mPm.get(pkg);
            if (target == null || mPm.isRemovalPending(pkg)) return false;
            AllowlistCaller caller = allowlistCaller(target, identity);
            if (caller == null) return false;
            requireAllowlistAuthority(caller, flags);
            PermissionInfo info = mCatalog.get(permission);
            if (info == null) return false;
            if (info.isRestricted() && (info.flags & PermissionInfo.FLAG_IMMUTABLY_RESTRICTED) != 0
                    && !caller.privileged) {
                throw new SecurityException("Cannot modify allowlisting of an immutably restricted permission: " + permission);
            }
            List<String> current = allowlisted(target, flags);
            List<String> permissions = current == null ? new ArrayList<>() : new ArrayList<>(current);
            if (add) {
                if (permissions.contains(permission)) return false;
                if (flags == PackageManager.FLAG_PERMISSION_WHITELIST_UPGRADE && !caller.privileged) {
                    throw new SecurityException("Adding to upgrade allowlist requires "
                            + Manifest.permission.WHITELIST_RESTRICTED_PERMISSIONS);
                }
                permissions.add(permission);
            } else if (!permissions.remove(permission)) return false;
            // Both the read and write stay under the same locks. Independent add/remove
            // operations cannot overwrite each other's changes or adopt a replacement APK.
            lostGrants = setAllowlistLocked(target, permissions, flags, false);
        } }
        if (lostGrants == null) return false;
        notifyAllowlistChanged(target, lostGrants);
        return true;
    }

    /** AOSP onPackageInstalled: an install request adds its installer allowlist to existing state. */
    void addInstallerAllowlist(ApkPackage target, java.util.Collection<String> permissions) {
        List<String> added = new ArrayList<>(permissions == null ? target.parsed.getRequestedPermissions() : permissions);
        setAllowlist(target, added, PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, true);
    }

    /** AOSP setAllowlistedRestrictedPermissionsInternal followed by restorePermissionState. */
    private void setAllowlist(ApkPackage target, java.util.Collection<String> permissions, int allowlist, boolean addOnly) {
        List<String> lostGrants;
        synchronized (mPm) { synchronized (this) {
            if (mPm.get(target.packageName) != target || mPm.isRemovalPending(target.packageName)) return;
            lostGrants = setAllowlistLocked(target, permissions, allowlist, addOnly);
        } }
        if (lostGrants != null) notifyAllowlistChanged(target, lostGrants);
    }

    /** Called with PM then policy locked; null means no state changed. */
    private List<String> setAllowlistLocked(ApkPackage target, java.util.Collection<String> permissions,
            int allowlist, boolean addOnly) {
        int exemption = exemptionFlags(allowlist);
        List<String> lostGrants = new ArrayList<>();
        List<ApkPackage> members = uidMembers(target.uid());
        Map<Integer, Integer> before = runtimeModes(target.uid());
        boolean changed = false;
        for (String permission : target.parsed.getRequestedPermissions()) {
            PermissionInfo info = mCatalog.get(permission);
            if (info == null || !info.isRestricted()) continue;
            PermissionState old = permissionState(target.uid(), permission, members, info);
            int flags = old.flags;
            if (permissions.contains(permission)) flags |= exemption;
            else if (!addOnly) flags &= ~exemption;
            boolean wasAllowlisted = (old.flags & RESTRICTION_EXEMPT) != 0;
            boolean isAllowlisted = (flags & RESTRICTION_EXEMPT) != 0;
            // Allowlisting trumps policy: a policy-fixed grant that loses every allowlist
            // is no longer fixed.
            if ((flags & PackageManager.FLAG_PERMISSION_POLICY_FIXED) != 0 && !isAllowlisted && old.granted) {
                flags &= ~PackageManager.FLAG_PERMISSION_POLICY_FIXED;
            }
            int target23 = requestingTargetSdk(permission, members);
            if (target23 < 23 && !wasAllowlisted && isAllowlisted) flags |= PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
            boolean legacy = storedLegacyGrant(target.uid(), permission, info, target23);
            flags = implicitPermissionFlags(applyRestriction(info, flags), legacy);
            if (flags == old.flags) continue;
            changed = true;
            mDecisions.setProperty(legacyGrantKey(target.uid(), permission), Boolean.toString(legacy));
            mDecisions.setProperty(flagsKey(target.uid(), permission), Integer.toString(flags));
            if (old.granted && !permissionState(target.uid(), permission, members, info).granted) lostGrants.add(permission);
        }
        if (!changed) return null;
        scheduleWrite();
        publishRuntimeModes(target.uid(), before);
        return lostGrants;
    }

    private void notifyAllowlistChanged(ApkPackage target, List<String> lostGrants) {
        // Cache invalidation and application callbacks run after both state monitors are released.
        permissionsChanged(target, () -> lostGrants.stream().anyMatch(name -> !granted(name, target.packageName)));
    }
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
