/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.admin.DeviceAdminInfo;
import android.app.admin.DeviceAdminReceiver;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.*;
import android.os.*;
import android.util.DisplayMetrics;
import org.xmlpull.v1.XmlPullParser;
import java.nio.file.Path;
import java.util.*;
import java.util.function.*;

/** Consented legacy device administrators; supported policy: lock the real Windows session. */
final class DeviceAdminPolicyService extends UnmanagedDevicePolicyService {
    private static final int FORCE_LOCK = 1 << DeviceAdminInfo.USES_POLICY_FORCE_LOCK;
    static final class Request {
        final ApkPackage code;
        final ComponentName admin;
        final String label, description;
        final boolean supported;
        final int policies;
        Request(ApkPackage code, ComponentName admin, String label, String description, boolean supported, int policies) {
            this.code = code; this.admin = admin; this.label = label; this.description = description;
            this.supported = supported; this.policies = policies;
        }
    }
    private final PackageManagerService pm;
    private DeviceAdminStore store = new DeviceAdminStore(null);
    private BiFunction<Integer, Integer, ApkPackage> callers = (pid, uid) -> null;
    private Consumer<Intent> broadcasts = intent -> { };
    private final BooleanSupplier hostAvailable, lock;
    DeviceAdminPolicyService(PackageManagerService pm) {
        this(pm, WindowsHost::deviceAdminAvailable, WindowsHost::lockWorkstation);
    }
    DeviceAdminPolicyService(PackageManagerService pm, BooleanSupplier hostAvailable, BooleanSupplier lock) {
        this.pm = pm; this.hostAvailable = hostAvailable; this.lock = lock;
    }
    void configure(BiFunction<Integer, Integer, ApkPackage> callers, boolean persistent, Consumer<Intent> broadcasts) {
        this.callers = callers; this.broadcasts = broadcasts;
        store = new DeviceAdminStore(persistent ? Path.of(Device.dataDir(), "system", "device-admin-grants") : null);
        // Removing an install invalidates its persisted owner even if publication later fails.
        pm.onPackageRemoval(name -> {
            synchronized (pm) {
                for (String key : store.snapshot().keySet()) {
                    ComponentName admin = ComponentName.unflattenFromString(key);
                    if (admin == null || name.equals(admin.getPackageName())) store.put(key, null);
                }
            }
        });
        pm.onPackageRemovalCheck(name -> {
            synchronized (pm) {
                for (String key : store.snapshot().keySet()) {
                    ComponentName admin = ComponentName.unflattenFromString(key);
                    if (admin != null && name.equals(admin.getPackageName()) && grant(admin, true) != null)
                        throw new SecurityException("Deactivate the device administrator before uninstalling it");
                }
            }
        });
    }
    boolean available() { return hostAvailable.getAsBoolean(); }
    Request prepare(ComponentName admin) {
        return prepare(admin, false);
    }
    private Request prepare(ComponentName admin, boolean removalCheck) {
        if (admin == null) return null;
        ApkPackage code = pm.get(admin.getPackageName());
        if (code == null || !removalCheck && pm.isRemovalPending(code.packageName)) return null;
        ActivityInfo info = code.receiverInfo(admin.getClassName(), PackageManager.GET_META_DATA);
        if (info == null || !Manifest.permission.BIND_DEVICE_ADMIN.equals(info.permission)
                || info.metaData == null || !info.enabled || !info.applicationInfo.enabled) return null;
        boolean enabledAction = false;
        for (var receiver : code.parsed.getReceivers()) if (admin.getClassName().equals(receiver.getName()))
            for (var filter : receiver.getIntents()) if (filter.getIntentFilter().hasAction(
                    DeviceAdminReceiver.ACTION_DEVICE_ADMIN_ENABLED)) enabledAction = true;
        if (!enabledAction) return null;
        int resource = info.metaData.getInt(DeviceAdminReceiver.DEVICE_ADMIN_META_DATA);
        if (resource == 0) return null;
        try (AssetManager assets = new AssetManager()) {
            if (assets.addAssetPath(info.applicationInfo.sourceDir) == 0) return null;
            if (info.applicationInfo.splitSourceDirs != null) for (String split : info.applicationInfo.splitSourceDirs)
                if (assets.addAssetPath(split) == 0) return null;
            DisplayMetrics metrics = new DisplayMetrics(); metrics.setToDefaults();
            Configuration configuration = new Configuration(); configuration.setToDefaults();
            Resources resources = new Resources(assets, metrics, configuration);
            String label = info.nonLocalizedLabel == null ? code.packageName : info.nonLocalizedLabel.toString();
            if (info.labelRes != 0) label = resources.getString(info.labelRes);
            String description = info.descriptionRes == 0 ? "" : resources.getString(info.descriptionRes);
            try (XmlResourceParser xml = resources.getXml(resource)) {
                int event;
                do { event = xml.next(); } while (event != XmlPullParser.START_TAG && event != XmlPullParser.END_DOCUMENT);
                if (event != XmlPullParser.START_TAG || !"device-admin".equals(xml.getName())) return null;
                int depth = xml.getDepth(), policies = 0;
                boolean supported = true, usesPolicies = false;
                while ((event = xml.next()) != XmlPullParser.END_DOCUMENT) {
                    if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
                    if (event == XmlPullParser.END_TAG && xml.getDepth() == depth + 1) usesPolicies = false;
                    if (event != XmlPullParser.START_TAG) continue;
                    if (xml.getDepth() == depth + 1) {
                        usesPolicies = "uses-policies".equals(xml.getName());
                        if (!usesPolicies) supported = false;
                    } else if (xml.getDepth() == depth + 2 && usesPolicies) {
                        if ("force-lock".equals(xml.getName())) policies |= FORCE_LOCK;
                        else supported = false;
                    } else supported = false;
                }
                return new Request(code, admin, label, description, supported && policies == FORCE_LOCK && available(), policies);
            }
        } catch (Exception invalid) { return null; }
    }
    boolean activate(Request request) {
        if (request == null || !request.supported || !available()) return false;
        synchronized (pm) {
            if (pm.get(request.code.packageName) != request.code) return false;
            Request current = prepare(request.admin);
            if (current == null || !current.supported || current.policies != request.policies) return false;
            String owner = request.code.enabledStateOwner();
            if (owner == null) return false;
            store.put(request.admin.flattenToString(), new DeviceAdminStore.Grant(owner, request.policies));
        }
        notifyAdmin(DeviceAdminReceiver.ACTION_DEVICE_ADMIN_ENABLED, request.admin);
        return true;
    }
    private static void user(int id) { if (id != 0) throw new SecurityException("Unsupported Android user"); }
    private DeviceAdminStore.Grant grant(ComponentName admin) {
        return grant(admin, false);
    }
    // Installer seals the current installation before its removal checks. Only
    // this veto may inspect its enrollment while sealed; normal operations and
    // new enrollment still reject it. Owner and current-manifest checks remain.
    private DeviceAdminStore.Grant grant(ComponentName admin, boolean removalCheck) {
        if (admin == null) return null;
        ApkPackage code = pm.get(admin.getPackageName());
        DeviceAdminStore.Grant grant = store.get(admin.flattenToString());
        if (code == null || !removalCheck && pm.isRemovalPending(code.packageName) || grant == null
                || !grant.owner().equals(code.enabledStateOwner())) return null;
        // Updating an app may remove/disable its receiver or remove a policy.
        // The old record must not keep authority the current manifest no longer declares.
        Request current = prepare(admin, removalCheck);
        return current != null && current.supported && current.policies == grant.policies() ? grant : null;
    }
    private List<ComponentName> activeAdmins() {
        synchronized (pm) {
            List<ComponentName> result = new ArrayList<>();
            for (String key : store.snapshot().keySet()) {
                ComponentName admin = ComponentName.unflattenFromString(key);
                if (grant(admin) != null) result.add(admin);
            }
            return result;
        }
    }
    @Override public boolean isAdminActive(ComponentName admin, int userId) {
        user(userId); synchronized (pm) { return grant(admin) != null; }
    }
    @Override public List<ComponentName> getActiveAdmins(int userId) {
        user(userId); List<ComponentName> result = activeAdmins(); return result.isEmpty() ? null : result;
    }
    @Override public boolean packageHasActiveAdmins(String packageName, int userId) {
        user(userId);
        for (ComponentName admin : activeAdmins()) if (admin.getPackageName().equals(packageName)) return true;
        return false;
    }
    @Override public boolean hasGrantedPolicy(ComponentName admin, int policy, int userId) {
        user(userId);
        synchronized (pm) {
            DeviceAdminStore.Grant grant = grant(admin);
            return policy == DeviceAdminInfo.USES_POLICY_FORCE_LOCK && grant != null && (grant.policies() & FORCE_LOCK) != 0;
        }
    }
    private ApkPackage caller(String claimed) {
        ApkPackage code = callers.apply(Binder.getCallingPid(), Binder.getCallingUid());
        if (code == null || claimed == null || !claimed.equals(code.packageName))
            throw new SecurityException("Device administrator operation requires its attached application");
        return code;
    }
    @Override public void removeActiveAdmin(ComponentName admin, int userId) {
        user(userId); if (admin == null) throw new IllegalArgumentException("Missing administrator");
        ApkPackage code = caller(admin.getPackageName());
        synchronized (pm) {
            if (pm.get(code.packageName) != code || pm.isRemovalPending(code.packageName)) throw new SecurityException("Retired caller");
            if (grant(admin) == null) return;
            store.put(admin.flattenToString(), null);
        }
        notifyAdmin(DeviceAdminReceiver.ACTION_DEVICE_ADMIN_DISABLED, admin);
    }
    /** Trusted server control plane only; never exposed as a permissive Binder method. */
    void revoke(ComponentName admin) {
        synchronized (pm) {
            if (grant(admin) == null) return;
            store.put(admin.flattenToString(), null);
        }
        notifyAdmin(DeviceAdminReceiver.ACTION_DEVICE_ADMIN_DISABLED, admin);
    }
    private void notifyAdmin(String action, ComponentName admin) {
        try { broadcasts.accept(new Intent(action).setComponent(admin)); }
        catch (RuntimeException failure) { SystemServer.log("Device admin broadcast failed: " + failure.getClass().getSimpleName()); }
    }
    @Override public void setMaximumTimeToLock(ComponentName admin, String caller, long timeout, boolean parent) {
        throw new UnsupportedOperationException("Windows automatic session lock policy is not supported");
    }
    @Override public void setCameraDisabled(ComponentName admin, String caller, boolean disabled, boolean parent) {
        throw new SecurityException("Camera policy is not granted");
    }
    @Override public void setKeyguardDisabledFeatures(ComponentName admin, String caller, int features, boolean parent) {
        throw new SecurityException("Keyguard policy is not granted");
    }
    @Override public void setPasswordQuality(ComponentName admin, int quality, boolean parent) {
        throw new SecurityException("Password policy is not granted");
    }
    @Override public void setPasswordMinimumLength(ComponentName admin, int length, boolean parent) {
        throw new SecurityException("Password policy is not granted");
    }
    @Override public void lockNow(int flags, String callingPackage, boolean parent) {
        if (flags != 0 || parent) throw new IllegalArgumentException("Unsupported lock options");
        ApkPackage code = caller(callingPackage);
        synchronized (pm) {
            if (pm.get(code.packageName) != code || pm.isRemovalPending(code.packageName)) throw new SecurityException("Retired caller");
            boolean permitted = false;
            for (ComponentName admin : activeAdmins()) if (code.packageName.equals(admin.getPackageName())
                    && hasGrantedPolicy(admin, DeviceAdminInfo.USES_POLICY_FORCE_LOCK, 0)) permitted = true;
            if (!permitted) throw new SecurityException("No active administrator with force-lock policy");
            // Serializes removal/revocation with submission of the real host operation.
            if (!lock.getAsBoolean()) throw new IllegalStateException("Windows did not confirm the session was locked");
        }
    }
}
