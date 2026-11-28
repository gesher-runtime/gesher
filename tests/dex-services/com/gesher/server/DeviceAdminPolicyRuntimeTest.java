/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.app.admin.DeviceAdminInfo;
import android.app.admin.IDevicePolicyManager;
import android.os.*;
import java.util.*;
import java.util.concurrent.atomic.*;

/** Real APK parser, resource XML, PM and service; only host locking and attached identity are boundaries. */
public final class DeviceAdminPolicyRuntimeTest {
    static int checks;
    static void check(boolean value, String message) {
        checks++; if (!value) throw new AssertionError(message);
    }
    interface Work { void run() throws Exception; }
    /** Force generated AIDL Proxy/Stub Parcel dispatch even inside this fixture VM. */
    static IDevicePolicyManager wire(IBinder service) {
        return IDevicePolicyManager.Stub.asInterface(new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException { return service.transact(code, data, reply, flags); }
        });
    }
    static void rejects(Class<? extends Throwable> kind, Work work) {
        try { work.run(); } catch (Throwable failure) {
            check(kind.isInstance(failure), "unexpected failure " + failure); return;
        }
        throw new AssertionError("missing " + kind.getSimpleName());
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        android.content.res.AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        var shared = Class.forName("com.android.internal.os.ApplicationSharedMemory");
        shared.getMethod("setInstance", shared).invoke(null, shared.getMethod("create").invoke(null));
        ApkPackage valid = ApkPackage.parse(args[0]);
        ApkPackage unsupported = ApkPackage.parse(args[1]);
        ApkPackage wrongPermission = ApkPackage.parse(args[2]);
        PackageManagerService pm = new PackageManagerService(List.of(valid, unsupported, wrongPermission), false);
        AtomicBoolean hostAvailable = new AtomicBoolean(true), lockResult = new AtomicBoolean(false);
        AtomicInteger locks = new AtomicInteger();
        AtomicReference<ApkPackage> caller = new AtomicReference<>(valid);
        List<Intent> notifications = new ArrayList<>();
        DeviceAdminPolicyService service = new DeviceAdminPolicyService(pm, hostAvailable::get,
                () -> { locks.incrementAndGet(); return lockResult.get(); });
        service.configure((pid, uid) -> caller.get(), false, notifications::add);
        StandIn.setStrict(false);
        IDevicePolicyManager binderPolicy = wire(StandIn.ofStrictDefaults(IDevicePolicyManager.class, service).asBinder());
        ComponentName admin = new ComponentName(valid.packageName, valid.packageName + ".Admin");
        ComponentName other = new ComponentName(unsupported.packageName, unsupported.packageName + ".Admin");
        check(!service.isAdminActive(admin, 0), "new app already active");
        check(service.getActiveAdmins(0) == null, "empty admin list contract");
        rejects(UnsupportedOperationException.class, () -> binderPolicy.setPasswordMinimumLetters(admin, 4, false));
        check(!binderPolicy.isAdminActive(admin, 0), "Strict-default Binder preserves implemented inactive query");
        rejects(SecurityException.class, () -> service.lockNow(0, valid.packageName, false));
        check(locks.get() == 0, "unconsented lock reached Windows");
        var request = service.prepare(admin);
        check(request != null && request.supported, "force-lock resource not recognized");
        check(!service.prepare(other).supported, "wipe policy accepted");
        check(service.prepare(new ComponentName(wrongPermission.packageName,
                wrongPermission.packageName + ".Admin")) == null, "unprotected receiver accepted");
        check(service.prepare(new ComponentName(valid.packageName, valid.packageName + ".Missing")) == null,
                "missing receiver accepted");
        hostAvailable.set(false);
        check(!service.activate(request), "missing native backend granted administration");
        hostAvailable.set(true);
        check(service.activate(request), "consented supported request not saved");
        check(service.isAdminActive(admin, 0), "approved receiver not active");
        rejects(UnsupportedOperationException.class, () -> binderPolicy.setPasswordMinimumLetters(admin, 4, false));
        check(binderPolicy.isAdminActive(admin, 0) && !binderPolicy.isDeviceManaged()
                && binderPolicy.hasGrantedPolicy(admin, DeviceAdminInfo.USES_POLICY_FORCE_LOCK, 0),
                "Strict-default Binder preserves implemented enrollment, policy and unmanaged queries");
        check(service.hasGrantedPolicy(admin, DeviceAdminInfo.USES_POLICY_FORCE_LOCK, 0), "force lock not granted");
        check(!service.hasGrantedPolicy(admin, DeviceAdminInfo.USES_POLICY_WIPE_DATA, 0), "wipe incorrectly granted");
        rejects(IllegalStateException.class, () -> service.lockNow(0, valid.packageName, false));
        check(locks.get() == 1, "host rejection not exercised");
        rejects(IllegalArgumentException.class, () -> service.lockNow(1, valid.packageName, false));
        rejects(IllegalArgumentException.class, () -> service.lockNow(0, valid.packageName, true));
        caller.set(unsupported);
        rejects(SecurityException.class, () -> service.lockNow(0, valid.packageName, false));
        rejects(SecurityException.class, () -> service.removeActiveAdmin(admin, 0));
        check(service.isAdminActive(admin, 0), "foreign caller revoked authority");
        rejects(SecurityException.class, () -> pm.checkPackageRemoval(valid.packageName));
        // Match Installer.uninstall: seal first, then run the removal veto.
        // The ordinary API stays unavailable without erasing the saved consent.
        caller.set(valid);
        pm.beginPackageRemoval(valid);
        try {
            check(!service.isAdminActive(admin, 0) && service.getActiveAdmins(0) == null
                    && !service.packageHasActiveAdmins(valid.packageName, 0), "pending admin remains visible");
            check(!service.hasGrantedPolicy(admin, DeviceAdminInfo.USES_POLICY_FORCE_LOCK, 0),
                    "pending administrator retains ordinary policy authority");
            check(service.prepare(admin) == null && !service.activate(request), "pending removal accepts enrollment");
            rejects(SecurityException.class, () -> service.lockNow(0, valid.packageName, false));
            rejects(SecurityException.class, () -> service.removeActiveAdmin(admin, 0));
            rejects(SecurityException.class, () -> pm.checkPackageRemoval(valid.packageName));
            check(locks.get() == 1, "pending administrator reached host lock");
        } finally { pm.endPackageRemoval(valid); }
        check(service.isAdminActive(admin, 0), "blocked uninstall erased administrator consent");
        service.revoke(admin);
        check(!service.isAdminActive(admin, 0), "trusted revocation failed");
        pm.beginPackageRemoval(valid);
        try { pm.checkPackageRemoval(valid.packageName); }
        finally { pm.endPackageRemoval(valid); }
        check(!service.isAdminActive(admin, 0), "revoked enrollment restored after removal check");
        caller.set(valid);
        check(service.activate(request), "re-enrollment failed");
        service.removeActiveAdmin(admin, 0);
        check(!service.isAdminActive(admin, 0), "own revocation failed");
        check(notifications.size() == 4, "committed callbacks missing");
        check(locks.get() == 1, "unauthorized operations reached host");
        check(service.activate(request), "update setup enrollment failed");
        ApkPackage changed = ApkPackage.parse(args[3]);
        pm.add(changed);
        check(!service.isAdminActive(admin, 0), "updated unsupported policy set inherited enrollment");
        check(!service.activate(request), "stale consent activated replacement APK");
        caller.set(changed);
        rejects(SecurityException.class, () -> service.lockNow(0, changed.packageName, false));
        pm.beginPackageRemoval(changed);
        try { pm.checkPackageRemoval(changed.packageName); }
        finally { pm.endPackageRemoval(changed); }
        check(!service.isAdminActive(admin, 0), "unsupported current policy gained a removal veto");
        // Retain the old policy row across real PM owner retirement/reallocation.
        // Even a supported receiver with the reused package name must not inherit it.
        String previousOwner = changed.enabledStateOwner();
        pm.remove(changed.packageName);
        ApkPackage reinstalled = ApkPackage.parse(args[0]);
        pm.add(reinstalled);
        check(!previousOwner.equals(reinstalled.enabledStateOwner()), "reinstall retained installation owner");
        pm.beginPackageRemoval(reinstalled);
        try { pm.checkPackageRemoval(reinstalled.packageName); }
        finally { pm.endPackageRemoval(reinstalled); }
        check(!service.isAdminActive(admin, 0) && !service.activate(request),
                "retired owner's enrollment or stale APK consent survived reinstall");
        // This is the actual Services registration, not a test-created policy wrapper.
        IDevicePolicyManager registered = wire(Services.createInProcess(List.of())
                .get(android.content.Context.DEVICE_POLICY_SERVICE));
        rejects(UnsupportedOperationException.class, () -> registered.setPasswordMinimumLetters(admin, 4, false));
        check(registered.getActiveAdmins(0) == null && !registered.isDeviceManaged(),
                "Production DPM registration rejects defaults without changing implemented queries");
        System.out.println("DeviceAdminPolicyRuntimeTest: " + checks + " checks passed; host lock not executed");
    }
}
